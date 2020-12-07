#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <thread>
#include <mutex>
#include <chrono>
#include <iostream>
#include <atomic>

//#include <string.h>
#include <netinet/in.h>
#include <fcntl.h>
//#include <sys/types.h>
#include <signal.h>
//#include <sys/epoll.h>

#include <ugorji/codec/codec.h>
#include <ugorji/codec/binc.h>
#include <ugorji/util/bufio.h>
#include <ugorji/util/logging.h>
#include "conn.h"

namespace ugorji { 
namespace ndb { 

codec_encode encoder = codec_binc_encode;
codec_decode decoder = codec_binc_decode;

std::atomic<size_t> SEQ;

const bool GET_VIA_ITER = false; // Iteration doesn't support bloom filter optimization

bool to_codec_value(std::string* serr, codec_value& out1) {
    if(serr != nullptr) {
        out1.type = CODEC_VALUE_STRING;
        out1.v.vString.bytes.v = (char*)serr->c_str();
        out1.v.vString.bytes.len = serr->length();
        return true;
    }
    return false;
}

bool to_codec_value(leveldb::Slice* serr, codec_value& out1) {
    if(serr != nullptr) {
        out1.type = CODEC_VALUE_STRING;
        out1.v.vString.bytes.v = (char*)serr->data();
        out1.v.vString.bytes.len = serr->size();
        return true;
    }
    return false;
}


struct dbBatchUpdateT {
    std::vector<leveldb::Slice> putkeys;
    std::vector<leveldb::Slice> putvalues;
    std::vector<leveldb::Slice> delkeys;
};

class db2BatchUpdateT {
public:
    std::unordered_map<Ndb*, dbBatchUpdateT*> m_;
    ~db2BatchUpdateT() {
        for(auto iter = m_.begin(); iter != m_.end(); ++iter) delete iter->second;
    }
    dbBatchUpdateT* getT(Ndb* n) {
        dbBatchUpdateT* iter;
        auto niter = m_.find(n);
        if(niter == m_.end()) {
            iter = new dbBatchUpdateT;
            m_[n] = iter;
        } else {
            iter = niter->second;
        }
        return iter;
    }
};

class dbAndIterGuard {
public:
    std::unordered_map<Ndb*, leveldb::Iterator*> m_;
    ~dbAndIterGuard() {
        for(auto iter = m_.begin(); iter != m_.end(); ++iter) delete iter->second;
    }
    leveldb::Iterator* getIter(Ndb* n) {
        leveldb::Iterator* iter;
        auto niter = m_.find(n);
        if(niter == m_.end()) {
            iter = n->db_->NewIterator(n->ropt_);
            m_[n] = iter;
        } else {
            iter = niter->second;
        }
        return iter;
    }
};


// Get codec_value from bytes, call appropriate function, and write out value
void ReqHandler::handle(slice_bytes in, slice_bytes& out, char** err) {
    fprintf(stderr, ">>>>>> ReqHandler::handle called\n");
    // req: [ id, method, paramsArr]
    // resp:[ id, error, result]
    codec_value cvIn, cvOut;
    
    *err = nullptr;
    decoder(in, &cvIn, err);
    if(*err != nullptr) return;

    cvOut.type = CODEC_VALUE_ARRAY;
    cvOut.v.vArray.len = 3;
    cvOut.v.vArray.v = (codec_value*)calloc(3, sizeof(codec_value));
    cvOut.v.vArray.v[0] = cvIn.v.vArray.v[0];
    for(int i = 1; i < 3; i++) {
        cvOut.v.vArray.v[i].type = CODEC_VALUE_NIL;
        cvOut.v.vArray.v[i].v.vNil = true;
    }
    
    if(cvIn.v.vArray.v[1].type != CODEC_VALUE_STRING ||
       cvIn.v.vArray.v[1].v.vString.bytes.len != 1) {
        *err = (char*)&("Invalid type for second parameter. Must be single-char string"[0]);
        return;
    }

    std::string* serr = nullptr;
    codec_value_list params = cvIn.v.vArray.v[2].v.vArray;
    int pi = 0;
    codec_value& out1 = cvOut.v.vArray.v[1];
    codec_value& out2 = cvOut.v.vArray.v[2];

    switch(cvIn.v.vArray.v[1].v.vString.bytes.v[0]) {
    case 'N':
    {
        if(params.len < 4 || 
           params.v[0].type != CODEC_VALUE_BYTES || 
           params.v[1].type != CODEC_VALUE_BOOL ||
           params.v[2].type != CODEC_VALUE_POS_INT ||
           params.v[3].type != CODEC_VALUE_POS_INT) {
            *serr = "Invalid input";
            if(to_codec_value(serr, out1)) break;
        }
        leveldb::Slice key(params.v[0].v.vBytes.bytes.v, params.v[0].v.vBytes.bytes.len);
        bool incr = params.v[1].v.vBool;
        uint16_t delta = (uint16_t)params.v[2].v.vUint64;
        uint16_t initVal = (uint16_t)params.v[3].v.vUint64;
        LOG(TRACE, "IncrDecr: Request fully received", 0);
        auto db = mgr_->ndbForKey(key, serr);
        if(to_codec_value(serr, out1)) break;
        uint64_t nextval;
        db->incrdecr(key, incr, delta, initVal, &nextval, serr);
        if(to_codec_value(serr, out1)) break;
        out2.type = CODEC_VALUE_POS_INT;
        out2.v.vUint64 = nextval;
    }
    break;
    case 'G': 
    {
        LOG(TRACE, "Get: #keys: %u", params.v[0].v.vArray.len);
        std::vector<leveldb::Slice> keys(params.v[0].v.vArray.len);
        for(int i = 0; i < params.v[0].v.vArray.len; ++i) {
            keys[i] = leveldb::Slice(params.v[0].v.vArray.v[i].v.vBytes.bytes.v, 
                                     params.v[0].v.vArray.v[i].v.vBytes.bytes.len);
        }
        codec_value cx;
        cx.type = CODEC_VALUE_ARRAY;
        cx.v.vArray.len = params.v[0].v.vArray.len;
        cx.v.vArray.v = (codec_value*)calloc(cx.v.vArray.len, sizeof (codec_value));
        LOG(TRACE, "Get: Request fully received", 0);
        uint16_t xshd;
        uint8_t xrk, xk, xshp;
        if(GET_VIA_ITER) {
            dbAndIterGuard dbiterg;
            for(int i = 0; i < cx.v.vArray.len; ++i) {
                auto db = mgr_->ndbForKey(keys[i], serr);
                if(to_codec_value(serr, out1)) break;
                auto iter = dbiterg.getIter(db);
                leveldb::Slice* sv;
                db->getViaIter(iter, keys[i], sv, serr);
                if(to_codec_value(serr, out1)) break;
                to_codec_value(sv, cx.v.vArray.v[i]);
            }
        } else {
            for(int i = 0; i < cx.v.vArray.len; ++i) {
                auto db = mgr_->ndbForKey(keys[i], serr);
                if(to_codec_value(serr, out1)) break;
                std::string* sv;
                db->get(keys[i], sv, serr);
                if(to_codec_value(serr, out1)) break;
                to_codec_value(sv, cx.v.vArray.v[i]);
            }
        }
    }
    break;
    case 'Q':
    {
        leveldb::Slice seekpos1(params.v[0].v.vBytes.bytes.v, params.v[0].v.vBytes.bytes.len);
        leveldb::Slice seekpos2(params.v[1].v.vBytes.bytes.v, params.v[1].v.vBytes.bytes.len);
        uint8_t kindid = (uint8_t)params.v[2].v.vUint64;
        uint8_t shapeid = (uint8_t)params.v[3].v.vUint64;
        bool ancestorOnly = params.v[4].v.vBool;
        bool withCursor = params.v[5].v.vBool;
        uint8_t lastFilterOp = (uint8_t)params.v[6].v.vUint64;
        size_t offset = params.v[7].v.vUint64;
        size_t limit = params.v[8].v.vUint64;
        LOG(TRACE, "Query: Request fully received", 0);
        auto db = mgr_->ndbForKey(seekpos1, serr);
        if(to_codec_value(serr, out1)) break;
        std::vector<leveldb::Slice> sls;
        auto iterFn = [&] (leveldb::Slice& sl) { sls.push_back(sl); };
        db->query(seekpos1, seekpos2, kindid, shapeid, ancestorOnly, withCursor, 
                  lastFilterOp, offset, limit, iterFn, serr);
        if(to_codec_value(serr, out1)) break;
        out2.type = CODEC_VALUE_ARRAY;
        out2.v.vArray.len = sls.size();
        out2.v.vArray.v = (codec_value*)calloc(out2.v.vArray.len, sizeof (codec_value));
        for(int i = 0; i < sls.size(); ++i) {
            out2.v.vArray.v[i].type = CODEC_VALUE_BYTES;
            out2.v.vArray.v[i].v.vBytes.bytes.v = (char*)sls[i].data();
            out2.v.vArray.v[i].v.vBytes.bytes.len = sls[i].size();
        }
    }
    break;
    case 'U':
    {
        codec_value_list lx = params.v[0].v.vArray;       
        LOG(TRACE, "Update: #Puts: %u", lx.len);
        db2BatchUpdateT db2bt;
        for(int i = 0; i < lx.len; ++i) {
            leveldb::Slice sl(lx.v[i].v.vBytes.bytes.v, lx.v[i].v.vBytes.bytes.len);
            leveldb::Slice sl2(lx.v[++i].v.vBytes.bytes.v, lx.v[i].v.vBytes.bytes.len);
            auto db = mgr_->ndbForKey(sl, serr);
            if(to_codec_value(serr, out1)) break;
            auto bt = db2bt.getT(db);
            bt->putkeys.push_back(sl);
            bt->putvalues.push_back(sl2);
        }
        if(out1.type != CODEC_VALUE_NIL) break;
        
        lx = params.v[1].v.vArray;
        LOG(TRACE, "Update: #Deletes: %u", lx.len);
        for(int i = 0; i < lx.len; ++i) {
            leveldb::Slice sl(lx.v[i].v.vBytes.bytes.v, lx.v[i].v.vBytes.bytes.len);
            auto db = mgr_->ndbForKey(sl, serr);
            if(to_codec_value(serr, out1)) break;
            auto bt = db2bt.getT(db);
            bt->delkeys.push_back(sl);
        }
        if(out1.type != CODEC_VALUE_NIL) break;
        LOG(TRACE, "Update: Request fully received", 0);

        for(auto iter = db2bt.m_.begin(); iter !=db2bt. m_.end(); ++iter) {
            auto bt = iter->second;
            iter->first->update(bt->putkeys, bt->putvalues, bt->delkeys, serr);
            if(to_codec_value(serr, out1)) break;
        }
    }
    break;
    default:
        char errbuf[64];
        snprintf(errbuf, 64, "Invalid desc byte: 0x%x", cvIn.v.vArray.v[1].v.vString.bytes.v[0]);
        *serr = std::string(errbuf);
        if(to_codec_value(serr, out1)) break;
    }

    LOG(TRACE, "Response sent to client", 0);

    encoder(&cvOut, &out, err);
    if(*err != nullptr) return;        
}

} //close namespace ndb
} // close namespace ugorji

