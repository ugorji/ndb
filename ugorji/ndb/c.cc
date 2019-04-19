#include "c.h"
#include "ndb.h"
#include <iostream>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <rocksdb/cache.h>
#include <rocksdb/env.h>

#include <unistd.h>
#include <thread>

// Notes:
// - Most assignments in C++ 11 involve copying of data.
//   Use std::move when possible, to prevent constant copying, or use references/pointers.
// - We don't interop with containing process logger. 
//   That will involve more chatter between Go and C.
//   Instead, for now, we will use a NDB_DEBUG flag (on/off) to just write debug messages to stderr.
//   leveldb will use its own LOG file (1 per open leveldb database ie per shard)
// - ndb_XXX(...) and ndb_release(...) work together like a try/finally.
//   Typical usage for this is:
//     reqkey = ndb_XXX(...)
//     ndb_release(&reqkey, 1)
// 
// TODO: 
// - Need to add merge operator to be shared.
// 

extern "C" {

struct ndb_t { 
    ugorji::ndb::Ndb* rep; 
    ~ndb_t() {
        delete rep;
    }
};

namespace ugorji { namespace ndb { namespace c {

class freeinfo {
public:
    ndb_t* db_;
    std::vector<std::string> strings_;
    std::vector<slice_bytes_t> sliceBytes_;
    std::vector<std::unique_ptr<slice_bytes_t[]>> arrSliceBytes_;
    std::vector<std::unique_ptr<char[]>> arrBytes_;
    freeinfo() : db_(nullptr) { } // define this, so db_ is set to nullptr at init time
    ~freeinfo() {
        delete db_;
    }
};


// these are shared "static-level" values, including
// global sequence, mutex and map of items to hold onto for later release.

std::atomic<uint32_t> seq;
std::mutex mu;
std::unordered_map<uint32_t,std::unique_ptr<freeinfo>> mfree;
std::once_flag once;
leveldb::Env* env; 
std::shared_ptr<leveldb::Cache> cache;
leveldb::Options opt;
bool inited = false;
const std::string& NOT_INITED_ERR = "ndb not initialised";
}}}; // namespace ugorji::ndb::c

namespace ndbc = ugorji::ndb::c;

void ndb_noop() {}

void ndb_once_init() {    
    uint64_t ram = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    size_t cacheSize = ram / 16;
    if(cacheSize > 64<<20) cacheSize = 64<<20; //64MB 
    size_t cores = std::thread::hardware_concurrency();
    
    if(NDB_DEBUG) std::cerr << ">>>> ndb_once_init: ram: " << ram << ", cacheSize: " 
                            << cacheSize << ", cores: " << cores << std::endl;
    
    ndbc::cache = leveldb::NewLRUCache(cacheSize); 
    ndbc::env = leveldb::Env::Default();
    ndbc::env->SetBackgroundThreads(cores, rocksdb::Env::LOW);
    ndbc::env->SetBackgroundThreads(cores, rocksdb::Env::HIGH);

    ndbc::opt.env = ndbc::env;
    // ndbc::opt.block_cache = ndbc::cache;
    ndbc::opt.create_if_missing = true;
    ndbc::opt.max_background_compactions = cores;
    ndbc::opt.max_background_flushes = cores;
    // ndbc::opt.max_open_files = ;
    ndbc::opt.write_buffer_size = 16<<20; //16MB (Default: 4MB)
    // ndbc::opt.block_size = 4<<10; //4K
    // ndbc::opt.info_log = l;
    ndbc::seq = 1; 
    ndbc::inited = true;
}

void ndb_trigger_crash() {
    leveldb::Env* env2; 
    env2->SetBackgroundThreads(1, rocksdb::Env::LOW); // trigger crash
}

void ndb_init() {
    //ndb_trigger_crash();
    std::call_once(ndbc::once, ndb_once_init);
}

std::string ndb_to_hex(const char* input, size_t len) {
    static const char* const lut = "0123456789ABCDEF";
    std::string output;
    output.reserve(2 * (len+2));
    output.push_back('0');
    output.push_back('X');
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = input[i];
        output.push_back(lut[c >> 4]);
        output.push_back(lut[c & 15]);
    }
    return output;
}

void ndb_release(uint32_t* reqkeys, size_t num) {
    std::lock_guard<std::mutex> lk(ndbc::mu);
    for(int i = 0; i < num; i++) ndbc::mfree.erase(reqkeys[i]);
}

// Note: this does a std::move of the string, so it's not copied, 
// and we maintain ref to backing array.
bool ndb_str_to_slice(std::string* src, slice_bytes_t* dest, ndbc::freeinfo* rkv) {
    if(src == nullptr || src->length() == 0) return false;
    *dest = slice_bytes_t{(char*)src->data(), src->length()};
    rkv->strings_.push_back(std::move(*src));
    return true;
}

uint32_t ndb_open(slice_bytes_t basedir, ndb_t** db, slice_bytes_t* err) {
    // We will not integrate Logging into Go's logging, instead opting to keep 
    // each leveldb log for each shard local to the shard.
    ndb_init();
    if(!ndbc::inited) {
        *err = slice_bytes_t{(char*)ndbc::NOT_INITED_ERR.data(), ndbc::NOT_INITED_ERR.length()};
        return 0;
    }
    auto rk = ++ndbc::seq;
    auto rkv = new ndbc::freeinfo;
    leveldb::DB* ldb = nullptr;
    auto dbdir = std::string(basedir.v, basedir.len);
    leveldb::Status s = leveldb::DB::Open(ndbc::opt, dbdir, &ldb);
    if(NDB_DEBUG) std::cerr << ">>> >>>>>>> calling ndb_open: " << dbdir << std::endl;
    if(s.ok() && ldb != nullptr) {
        auto l = new ugorji::ndb::Ndb();
        l->db_ = ldb;
        l->wopt_.sync = 0; 
        *db = new ndb_t{ .rep = l };
        rkv->db_ = *db;
    } else {
        auto str = s.ToString();
        *err = slice_bytes_t{(char*)str.data(), str.length()};     
        rkv->strings_.push_back(std::move(str));
        if(NDB_DEBUG) std::cerr << ">>> >>>>>>> ndb_open fail " << str << std::endl;
    }
    std::lock_guard<std::mutex> lk(ndbc::mu);
    ndbc::mfree[rk] = std::unique_ptr<ndbc::freeinfo>(rkv);
    return rk;
}

uint32_t ndb_get_multi(ndb_t* db, 
                   size_t numKeys,
                   slice_bytes_t* keys, 
                   slice_bytes_t** vals, 
                   slice_bytes_t** errs) {
    auto rk = ++ndbc::seq;
    std::vector<leveldb::Slice> vkeys;
    for(int i = 0; i < numKeys; i++) {
        vkeys.push_back(leveldb::Slice(keys[i].v, keys[i].len));
    }
    std::vector<std::string> verrs;
    std::vector<std::string> vvals;
    db->rep->gets(vkeys, &vvals, &verrs);
    auto errs2 = new slice_bytes_t[numKeys];
    auto vals2 = new slice_bytes_t[numKeys];
    
    for(int i = 0; i < numKeys; i++) {
        errs2[i] = slice_bytes_t{ (char*)verrs[i].data(), verrs[i].length() };
        vals2[i] = slice_bytes_t{ (char*)vvals[i].data(), vvals[i].length() };
    }
    *errs = errs2;
    *vals = vals2;
    auto rkv = new ndbc::freeinfo;
    rkv->strings_.reserve((numKeys*2)+4);
    for(int i = 0; i < numKeys; i++) {
        rkv->strings_.push_back(std::move(verrs[i]));
        rkv->strings_.push_back(std::move(vvals[i]));
    }
    // rkv->strings_.insert(rkv->strings_.end(), verrs.begin(), verrs.end());
    // rkv->strings_.insert(rkv->strings_.end(), vvals.begin(), vvals.end());
    rkv->arrSliceBytes_.push_back(std::unique_ptr<slice_bytes_t[]>(errs2));
    rkv->arrSliceBytes_.push_back(std::unique_ptr<slice_bytes_t[]>(vals2));
    std::lock_guard<std::mutex> lk(ndbc::mu);
    ndbc::mfree[rk] = std::unique_ptr<ndbc::freeinfo>(rkv);
    return rk;
}

uint32_t ndb_get(ndb_t* db, 
             slice_bytes_t key, 
             slice_bytes_t* val,
             slice_bytes_t* err) {
    auto rk = ++ndbc::seq;
    auto rkv = new ndbc::freeinfo;
    std::string* sval = nullptr;
    std::string* serr = nullptr;
    db->rep->get(leveldb::Slice(key.v, key.len), sval, serr);
    ndb_str_to_slice(serr, err, rkv);
    ndb_str_to_slice(sval, val, rkv);
    std::lock_guard<std::mutex> lk(ndbc::mu);
    ndbc::mfree[rk] = std::unique_ptr<ndbc::freeinfo>(rkv);
    return rk;     
}

uint32_t ndb_update(ndb_t* db, 
                slice_bytes_t* putKvs, size_t numPutKvs,
                slice_bytes_t* dels, size_t numDels,
                slice_bytes_t* err) {
    auto rk = ++ndbc::seq;
    auto rkv = new ndbc::freeinfo;
    std::vector<leveldb::Slice> putkeys, putvalues, delkeys;
    for(int i = 0; i < numPutKvs; ) {
        slice_bytes_t sl = putKvs[i++];
        putkeys.push_back(leveldb::Slice(sl.v, sl.len));
        sl = putKvs[i++];
        putvalues.push_back(leveldb::Slice(sl.v, sl.len));
    }
    for(int i = 0; i < numDels; i++) {
        slice_bytes_t sl = dels[i];
        delkeys.push_back(leveldb::Slice(sl.v, sl.len));
    }
    std::string* serr = nullptr;
    db->rep->update(putkeys, putvalues, delkeys, serr);
    ndb_str_to_slice(serr, err, rkv);
    std::lock_guard<std::mutex> lk(ndbc::mu);
    ndbc::mfree[rk] = std::unique_ptr<ndbc::freeinfo>(rkv);
    return rk;     
}

uint32_t ndb_query(ndb_t* db, 
                   slice_bytes_t seekpos1, 
                   slice_bytes_t seekpos2,
                   const uint8_t kindid, 
                   const uint8_t shapeid,
                   const bool ancestorOnly,
                   const bool withCursor,
                   const uint8_t lastFilterOp,
                   const size_t offset, 
                   const size_t limit, 
                   slice_bytes_t** results, 
                   size_t* numResults,
                   slice_bytes_t* err) {
    auto rk = ++ndbc::seq;
    auto rkv = new ndbc::freeinfo;
    std::vector<slice_bytes_t> sls{};
    auto iterFn = [&] (leveldb::Slice& sl) { 
        if(sl.size() == 0 || sl.data() == nullptr) return;
        auto slarr = new char[sl.size()];
        memcpy(slarr, sl.data(), sl.size());
        sls.push_back(slice_bytes_t{slarr, sl.size()}); 
        if(NDB_DEBUG) std::cerr << ">>>>> callback: Query Res: slarr: " << (void*)slarr 
                                << ", res: " << ndb_to_hex(slarr, sl.size()) << std::endl;
        rkv->arrBytes_.push_back(std::unique_ptr<char[]>(slarr));
    };
    std::string* serr = nullptr;
    db->rep->query(leveldb::Slice(seekpos1.v, seekpos1.len), 
                   leveldb::Slice(seekpos2.v, seekpos2.len), 
                   kindid, shapeid, ancestorOnly, withCursor, 
                   lastFilterOp, offset, limit, iterFn, serr);
    ndb_str_to_slice(serr, err, rkv);
    *numResults = sls.size();
    if(*numResults > 0) *results = sls.data(); //&sls.front(); //sls.data(); //&sls[0];
    if(NDB_DEBUG) {
        if(*numResults > 0) std::cerr << ">>>>> return: Query Res: *result: " 
                                      << (void*)(*results) << std::endl;
        std::cerr << ">>>>> return: Query Res: sls.data(): " 
                  << (void*)sls.data() << std::endl;    
        for(int i = 0; i < *numResults; i++) {
            std::cerr << ">>>>> return: Query Res: sls[" << i << "].v: " 
                      << (void*)sls[i].v << ", res: " 
                      << ndb_to_hex(sls[i].v, sls[i].len) << std::endl;
        }
    }
    // move it, so we maintain ref to backing array
    rkv->sliceBytes_ = std::move(sls);
    std::lock_guard<std::mutex> lk(ndbc::mu);
    ndbc::mfree[rk] = std::unique_ptr<ndbc::freeinfo>(rkv);
    if(NDB_DEBUG && *numResults > 0) {
        std::cerr << ">>>>> return: F Query Res: *ptr result (64bit): " 
                  << std::hex << *((uint64_t*)(*results)) << std::endl;
        std::cerr << ">>>>> return: F Query Res: *ptr arr[0] (64bit): " 
                  << std::hex << *((uint64_t*)(&rkv->sliceBytes_[0])) << std::endl;
        std::cerr << ">>>>> return: F Query Res: *ptr: " 
                  << (void*)(&rkv->sliceBytes_[0]) << std::endl;
    }
    return rk;
}

uint32_t ndb_incr_decr(ndb_t* db, 
                   slice_bytes_t key, bool incr,
                   uint16_t delta,
                   uint16_t initVal,
                   uint64_t* nextVal,
                   slice_bytes_t* err) {
    auto rk = ++ndbc::seq;
    auto rkv = new ndbc::freeinfo;
    std::string* serr = nullptr;
    db->rep->incrdecr(leveldb::Slice(key.v, key.len), incr, delta, initVal, nextVal, serr);
    ndb_str_to_slice(serr, err, rkv);
    std::lock_guard<std::mutex> lk(ndbc::mu);
    ndbc::mfree[rk] = std::unique_ptr<ndbc::freeinfo>(rkv);
    return rk;     
}

}  // end extern "C"
