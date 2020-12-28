
#include <vector>
#include <string>

#include <cstdio>
#include <unordered_map>
#include <mutex>
#include <cstdlib>
#include <cstdint>

#include <unistd.h>
#include <sys/stat.h>
#include <istream> 
#include <iostream>

#include <ugorji/util/logging.h>
#include <ugorji/util/bigendian.h>

#include "ndb.h"

#include <rocksdb/comparator.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/cache.h>

namespace ugorji { 
namespace ndb { 

//using ugorji::util::Tracef;

// This interfaces to leveldb. 
// 
// We expose Get, Batch Update (Put/Delete) and Query functionality.
// By implementing it here, we get full access to the Leveldb C++ API, 
// and C++ libraries and do not have to cross back/forth between C and 
// Go boundaries a lot like we did before. Also, we can use C++11 features
// and library.
// 
// Things I learned:
//   - Always initialize structs. Else you may get garbage.
//     If need be, initialize them to nullptr explicitly (else U'd waste 5 hours debugging aimlessly).
//   - No need to call status() everytime just before calling Valid().
//     Calling Valid() is sufficient. 
// 

const leveldb::Comparator* COMPARATOR = leveldb::BytewiseComparator();

leveldb::Slice ndbEntityBytesFromSlice(
    const uint8_t* ikey, 
    const size_t sz,
    const uint8_t kindid,
    const uint8_t shapeid
) {
    size_t el = sz;
    size_t discrim = ikey[0] >> 4;
    switch(discrim) {
    case D_IDGEN:
        break;
    case D_INDEX:
        // It's index row. Check backwards till you see a zero just before a sequence of 8. 
        // That shows demarcation.
        for(int j = sz-1-8; j >= 0; j=j-8) {
            if(ikey[j] == 0) {
                el = sz - j - 1;
                break;
            }
        }
        break;
    case D_ENTITY:
        // 2nd to last bytes is entity kind
        // last byte is entity shape (top 5) and entry type (lower 3)
        if(((0x07 & ikey[sz-1]) != E_DATA) ||
           (shapeid != 0 && shapeid != (ikey[sz-1] >> 3)) ||
           (kindid != 0 && kindid != ikey[sz-2])) {
            return leveldb::Slice();
        }
        break;
    }
    return leveldb::Slice((const char*)&(ikey[sz-el]), el);
}
       
void Ndb::gets(std::vector<leveldb::Slice>& keys, std::vector<std::string>* values, std::vector<std::string>* errs) {
    std::vector<leveldb::Status> ss = db_->MultiGet(ropt_, keys, values);
    for(size_t i = 0; i < ss.size(); i++) {
        // std::string tmp;
        if(ss[i].ok()) {
            errs->push_back("");
        } else if(ss[i].IsNotFound()) {
            errs->push_back(NOT_FOUND_ERROR);
        } else {
            errs->push_back(std::move(ss[i].ToString()));
        }
    }
}

void Ndb::get(leveldb::Slice key, std::string& value, std::string& err) {
    std::string tmp;
    leveldb::Status s = db_->Get(ropt_, key, &tmp);
    if(s.ok()) {
        value = std::move(tmp);
    } else if(s.IsNotFound()) {
        err = NOT_FOUND_ERROR;
    } else {
        err = std::move(s.ToString());
    }
}

void Ndb::getViaIter(leveldb::Iterator* iter, leveldb::Slice key, 
                     leveldb::Slice* value, std::string& err) {
    if(iter->status().ok()) {
        iter->Seek(key);
        if(!iter->status().ok()) {
            err = BAD_ITERATOR_ERROR;
        } else if(iter->Valid() && COMPARATOR->Compare(key, iter->key()) == 0) {
            *value = iter->value();
        } else {
            err = NOT_FOUND_ERROR;
        }
    } else {
        err = BAD_ITERATOR_ERROR;
    }
}

void Ndb::update(
    std::vector<leveldb::Slice>& putkeys,
    std::vector<leveldb::Slice>& putvalues, 
    std::vector<leveldb::Slice>& delkeys,
    std::string& err
) {
    auto numputs = putkeys.size();
    auto numdels = delkeys.size();
    leveldb::WriteBatch wb;
    for(size_t i = 0; i < numputs; i++) {
        wb.Put(putkeys[i], putvalues[i]);
    }
    for(size_t i = 0; i < numdels; i++) {
        wb.Delete(delkeys[i]);
    }
    leveldb::Status s = db_->Write(wopt_, &wb);
    if(!s.ok()) {
        err = std::move(s.ToString());
    }
}

void Ndb::incrdecr(
    leveldb::Slice key,
    bool incr,
    uint16_t delta,
    uint16_t initVal,
    uint64_t* nextVal,
    std::string& err
) {
    //typedef unsigned long long int uint64;
    //lock the key (with unlock after this is done) (RAII)
    std::vector<std::string> skeys { std::string(key.data(), key.size()) };
    ugorji::util::LockSetLock ls;
    locks_.locksFor(skeys, ls);
    uint64_t v(0);
    std::string t;
    leveldb::Status s = db_->Get(ropt_, key, &t);
    if(s.ok()) {
        if(t.size() != 8) {
            err = "Value for incr/decr must be 8-bytes. Got: " + 
                std::to_string(t.size()) + " bytes";
            return;
        }
        //big-endian binary decode this 8-byte value, into v
        v = util_big_endian_read_uint64((uint8_t*)&t[0]);
    } else if(s.IsNotFound()) {
        v = initVal;
    } else {
        err = std::move(s.ToString());
        return;
    }
    if(incr) {
        v += delta;
    } else {
        v -= delta;
    }
    //big-endian binary encode v, store it back, and write success or failure.
    char va[8];
    util_big_endian_write_uint64((uint8_t*)va, v);
    s = db_->Put(wopt_, key, leveldb::Slice(va, 8));
    LOG(TRACE, "IncrDecr: sending out: %llu, status: %s", v, s.ToString().c_str());
    if(s.ok()) {
        *nextVal = v;
    } else {
        err = std::move(s.ToString());
    }
}


// This can query for ancestor-only queries (checking D_DATA section) 
// or non-ancestor-only queries (checkin D_INDEXROW section).
// 
// if =, start at seek pos, once not match, return 
// if >, start at seek pos, skip till not match, and start returning
//       stop at irpfx2 >=
// if >=, start at seek pos, start returning
//       stop at irpfx2 >=
// if <, start at -1 seek pos, start returning, return EOF once >= irpfx
// if <=, start at -1 seek pos, return EOF once > irpfx	
// 
// The arguments here allow us specify these different conditions
// Below, char arguments are really bools (0=false, else true)
// 
// Ancestor queries are denoted as ^.
// skipOne: If true, On forward, skip first one if a match, 
//          and on !forward, always skip first one.
//          This supports ^ and <.
// forward: If true, Use Next() in iterator, else Use Prev().
//          True value supports ^, =, >, >=, 
//          False value supports <, <=
// skipFirstMatch: Skip every match till a non-match occurs.
//                 Supports >.
// stopIfNotMatch: Stop if not match found.
//                 Supports ^ and ==
// 
// limit: says maximum number of rows to return.
// offset: how many rows to after initial positioning.
void Ndb::query(
    const leveldb::Slice seekpos1,
    leveldb::Slice seekpos2,
    const uint8_t kindid,
    const uint8_t shapeid,
    const bool ancestorOnlyC,
    const bool withCursor,
    const uint8_t lastFilterOp,     
    const size_t offset,
    const size_t limit,
    std::function<void (leveldb::Slice&)> iterFn,
    std::string& err
) {
    uint8_t discrim = seekpos1[0] >> 4;
        
    bool forward = true;
    bool skipOne = false;
    bool skipFirstMatch = false;
    bool stopIfNotMatch = false;
        
    if(ancestorOnlyC) {
        seekpos2 = leveldb::Slice();
        skipOne = true;
        stopIfNotMatch = true;
    } else {
        switch(lastFilterOp) {
        case F_EQ: 
            stopIfNotMatch = true;
            break;
        case F_GTE: 
            break;
        case F_GT: 
            skipFirstMatch = true;
            break;
        case F_LTE: 
            forward = false;
            break;
        case F_LT: 
            forward = false;
            skipOne = true;
            skipFirstMatch = true;
            break;
        default:
            err = "ndb/leveldb: Invalid last filter operator: [" + 
                std::to_string(char(lastFilterOp)) + "]";
            return;
        }
    }
    int numscans = 0;
    size_t numResults = 0;
    if(withCursor) {
        skipOne = false;
        skipFirstMatch = false;
    }
    leveldb::Status s;
    leveldb::Slice ikey;
    leveldb::Iterator* iter = db_->NewIterator(ropt_);
    iterGuard iterg(iter);
    if(!iter->status().ok()) goto finish;
    iter->Seek(seekpos1);
    //if(!iter->status().ok()) goto finish;
    if(!iter->Valid()) goto finish;
    ikey = iter->key();
    //take care of <, <= and ancestor query
    if(skipOne) {
        if(forward) {
            if(ikey.size() >= seekpos1.size() && memcmp(seekpos1.data(), ikey.data(), seekpos1.size()) == 0) {
                iter->Next();
            }
        } else {
            iter->Prev();
        }
        //if(!iter->status().ok()) goto finish;
        if(!iter->Valid()) goto finish;
        ikey = iter->key();
    }
    if(skipFirstMatch) {
        //skip first if match
        for( ; 
             ikey.size() >= seekpos1.size() && memcmp(seekpos1.data(), ikey.data(), seekpos1.size()) == 0; 
             ikey = iter->key()) {
            if(forward) iter->Next();
            else iter->Prev();
            //if(!iter->status().ok()) goto finish;
            if(!iter->Valid()) goto finish;
        }
    }
    if(offset > 0) {
        for(size_t i = 0; i < offset; i++) {
            if(forward) iter->Next();
            else iter->Prev();
            //if(!iter->status().ok()) goto finish;
            if(!iter->Valid()) goto finish;
        }
        ikey = iter->key();
    }
    while(numResults < limit) {
        ++numscans;
        // If outside the block for which this iteration is valid, break out.
        // E.g. We're checking for indexes, but see an entity or idgen block.
        uint8_t discrim2 = ikey[0] >> 4;
        if(discrim != discrim2) {
            goto finish;
        }
        if(stopIfNotMatch) {
            if(ikey.size() < seekpos1.size() || memcmp(seekpos1.data(), ikey.data(), seekpos1.size()) != 0) {
                goto finish;
            }
        }
        // if end seekpos set and we're past it, finish
        if(seekpos2.size() > 0) {
            if(forward) {
                if(memcmp(seekpos2.data(), ikey.data(), seekpos2.size()) > 0) {
                    goto finish;
                }
            } else {
                if(memcmp(seekpos2.data(), ikey.data(), seekpos2.size()) < 0) {
                    goto finish;
                }
            } 
        }
            
        leveldb::Slice nt = ndbEntityBytesFromSlice((uint8_t*)ikey.data(), ikey.size(), kindid, shapeid);
        if(nt.size() != 0) {
            iterFn(nt);
            numResults++;
        }
        // continue loop
        if(forward) iter->Next();
        else iter->Prev();
        //if(!iter->status().ok()) goto finish;
        if(!iter->Valid()) goto finish;
        ikey = iter->key();            
    }
    
 finish:
    LOG(TRACE, "In Query: #scans: %d, #results: %d", numscans, numResults);
    if(!iter->status().ok()) {
        err = std::move(iter->status().ToString());
    }
}

} //close namespace ndb
} // close namespace ugorji

