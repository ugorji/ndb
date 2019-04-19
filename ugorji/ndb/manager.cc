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

#include <ugorji/util/logging.h>
#include <ugorji/util/bigendian.h>

#include <rocksdb/comparator.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/cache.h>

#include "manager.h"

// namespace leveldb = rocksdb;
namespace ugorji { 
namespace ndb { 

// We support 2 models: 1 shard = 1 db OR dbPerKind (shard/kind == 1 db) 

//__thread int TLS::reqNum; //C++ wart. must define it somewhere.

void LeveldbLogger::Logv(const leveldb::InfoLogLevel log_level, const char* format, va_list ap) {
    std::string s(prefix_);
    s.append(format);
    // enum Level { ALL, TRACE, DEBUG, INFO, WARNING, ERROR, SEVERE, OFF };
    ugorji::util::Log::Level lv = ugorji::util::Log::INFO;
    switch(log_level) {
    case leveldb::InfoLogLevel::DEBUG_LEVEL: lv = ugorji::util::Log::DEBUG; break;
    case leveldb::InfoLogLevel::INFO_LEVEL:  lv = ugorji::util::Log::INFO; break;
    case leveldb::InfoLogLevel::WARN_LEVEL:  lv = ugorji::util::Log::WARNING; break;
    case leveldb::InfoLogLevel::ERROR_LEVEL: lv = ugorji::util::Log::ERROR; break;
    case leveldb::InfoLogLevel::FATAL_LEVEL: lv = ugorji::util::Log::SEVERE; break;
    }
    ugorji::util::Log::getInstance().Logv(lv, __FILE__, __LINE__, s.c_str(), ap);
}

void trim(std::string& s1, bool left = true, bool right = true) {
    int n;
    if(right) {
        n = s1.find_last_not_of(" \n\r\t");
        if(n != std::string::npos) s1.erase(n+1);
    } 
    if(left) {
        n = s1.find_first_not_of(" \n\r\t");
        if(n != std::string::npos) s1.erase(0, n);
    }
}

std::string trim2(std::string s1, bool left = true, bool right = true) {
    trim(s1, left, right);
    return s1;
}

void ensureDir(std::string s, std::string* err) {
    struct stat st;
    if(::stat(s.c_str(), &st) == 0) {
        if(!S_ISDIR(st.st_mode)) *err = "Error: Not a directory";
    } else {
        int i = ::mkdir(s.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        if(i != 0) *err = "Error: Error making directory";
    }
}

void extractKeyParts(const uint8_t* ikey, 
                     const size_t sz,
                     uint16_t* shardid,
                     uint8_t* discrim,
                     uint8_t* indexid,
                     uint8_t* rootkindid,
                     uint8_t* kindid, 
                     uint8_t* shapeid
) {
    *discrim = ikey[0] >> 4;
    switch(*discrim) {
    case D_IDGEN: 
    case D_ENTITY:
        *shardid = ((0x0f & ikey[0]) << 8) | ikey[1];
        *rootkindid = ikey[6];
        *kindid = ikey[sz-2];
        *shapeid = (ikey[sz-1] >> 3);
        break;
    case D_INDEX:
        *kindid = ikey[1];
        *indexid = ikey[2];
        break;
    }
}

Manager::~Manager() {
    // for(int i = 0; i < caches_.size(); i++) delete caches_[i];
    // for(int i = 0; i < loggers_.size(); i++) delete loggers_[i];
    
    for(auto iter = indexDbs_.begin(); iter != indexDbs_.end(); iter++) delete iter->second;
    for(auto iter = shardDbs_.begin(); iter != shardDbs_.end(); iter++) delete iter->second;
    for(auto iter = perkindDbs_.begin(); iter != perkindDbs_.end(); ) {
        for(auto iter2 = iter->second->begin(); iter2 != iter->second->end(); iter2++) delete iter2->second;
        //have to do the erase/incr here, not in for loop definition (else iter is destroyed on erase)
        perkindDbs_.erase(iter++);
    }
}

Ndb* Manager::ndbForKey(leveldb::Slice& key, std::string* err) {
    uint16_t xshd;
    uint8_t xd, xi, xrk, xk, xshp;
    extractKeyParts((const uint8_t*)key.data(), key.size(), &xshd, &xd, &xi, &xrk, &xk, &xshp);
    Ndb* db = nullptr;
    switch(xd) {
    case D_IDGEN: 
    case D_ENTITY:
        if(xshd < shardMin_ || xshd >= (shardMin_ + shardRange_)) {
            *err = "ndbForKey: Invalid shard: " + std::to_string(xshd);
            break;
        }
        db = dataDb(xshd, xrk, err);
        break;
    case D_INDEX:
        db = indexDb(xi, err);
        break;
    default:
        *err = "ndbForKey: Error: unidentified discrim: " + std::to_string(xd);
    }
    return db;
}

// void Manager::status() {
//     char a[8];
//     memset(a, 0xff, 8);
//     sw.writeLong((uint8_t*)a);
// }

Ndb* Manager::openDb(std::string dbdir, leveldb::Options& opt, std::string* err) {
    LOG(INFO, "Opening DB: %s ...", dbdir.c_str());
    
    ugorji::util::LockSetLock lsl;
    auto vs = std::vector<std::string>{"db-dir:" + dbdir};
    locks_.locksFor(vs, &lsl);
    
    leveldb::DB* db = nullptr;
    leveldb::Status s = leveldb::DB::Open(opt, dbdir, &db);
    if(!s.ok() || db == nullptr) {
        *err = s.ToString();
        LOG(ERROR, "Error Opening DB: %s: err: %s", dbdir.c_str(), err->c_str());
        return nullptr;
    }
    Ndb* l = new Ndb();
    l->db_ = db;
    //l->wopt_.sync = 1;
    l->wopt_.sync = 0;
    LOG(INFO, "Successfully opened DB: %s", dbdir.c_str());
    return l;
}
  
Ndb* Manager::indexDb(uint8_t index, std::string* err) {
    Ndb* n = nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    auto dbiter = indexDbs_.find(index); 
    if(dbiter == indexDbs_.end()) {
        leveldb::Options opt;
        auto dbiter3 = indexOptions_.find(index);
        if(dbiter3 == indexOptions_.end()) {
            opt = defIndexOption_;
        } else {
            opt = dbiter3->second;
        }
        std::string dbdir = basedir_ + "/index-" + std::to_string(index);
        n = openDb(dbdir, opt, err);
        if(n != nullptr) {
            indexDbs_[index] = n;
        }        
    } else {
        n = dbiter->second;
    }
    return n;
}

Ndb* Manager::dataDb(uint16_t shard, uint8_t kind, std::string* err) {
    if(dbPerKind_) return perkindDb(shard, kind, err);
    return shardDb(shard, err);
}

Ndb* Manager::shardDb(uint16_t shard, std::string* err) {
    Ndb* n = nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    auto dbiter = shardDbs_.find(shard); 
    if(dbiter == shardDbs_.end()) {
        leveldb::Options opt = defKindOption_;
        std::string dbdir = basedir_ + "/shard-" + std::to_string(shard);
        n = openDb(dbdir, opt, err);
        if(n != nullptr) {
            shardDbs_[shard] = n;
        }
    } else {
        n = dbiter->second;
    }
    return n;
}

Ndb* Manager::perkindDb(uint16_t shard, uint8_t kind, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    Ndb* n = nullptr;
    auto dbiter = perkindDbs_.find(shard); 
    std::unordered_map<uint8_t, Ndb*>* m2 = nullptr;
    if(dbiter == perkindDbs_.end()) {
        m2 = new std::unordered_map<uint8_t, Ndb*>;
        perkindDbs_[shard] = m2;
    } else {
        m2 = dbiter->second;
    }
    auto dbiter2 = m2->find(kind);
    if(dbiter2 == m2->end()) {
        auto dbiter3 = kindOptions_.find(kind);
        leveldb::Options opt;
        if(dbiter3 == kindOptions_.end()) {
            opt = defKindOption_;
        } else {
            opt = dbiter3->second;
        }
        //make directory if not exist
        std::string dbdir = basedir_ + "/shard-" + std::to_string(shard);
        ensureDir(dbdir, err);
        if(err->size() > 0) return nullptr;
        dbdir = dbdir + "/root-kind-" + std::to_string(kind);
        ensureDir(dbdir, err);
        if(err->size() > 0) return nullptr;
        //printf(">>>>>> shard: %d, kind: %d, dbdir: %s\n", shard, kind, dbdir.c_str());
        n = openDb(dbdir, opt, err);
        if(n != nullptr) {
            //(*m2)[kind] = n;
            m2->emplace(kind, n);
        }
    } else {
        n = dbiter2->second;
    }
    return n;
}

// see doc.md for file format.
void Manager::load(std::istream& fs) {
    for(std::string line; std::getline(fs, line); ) {
        int n = line.find('=', 0);
        if(n == -1) continue;
        auto s0 = trim2(line.substr(0, n));
        auto s1 = trim2(line.substr(n+1));
        if(s0 == "basedir") {
            basedir_ = s1;
        } else {
            n = s0.find('.', 0);
            if(n == std::string::npos) continue;
            auto s2 = s0.substr(0, n);
            auto s3 = s0.substr(n+1);
            std::vector<std::string> ss;
            n = s3.find(',', 0);
            if(n == std::string::npos) {
                ss.push_back(s3);
            } else {
                for(int n0 = 0; n != std::string::npos; n = s3.find(',', n0)) {
                    ss.push_back(trim2(s3.substr(n0, n-n0)));
                    n0 = n+1;
                }
            }
            if(s2 == "block_cache") {
                for(int i = 0; i < ss.size(); i++) {
                    auto c = leveldb::NewLRUCache(std::stoi(s1));
                    blockCache_[ss[i]] = c;
                    caches_.push_back(c);
                }
            } else if(s2 == "kind" || s2 == "index") {
                leveldb::Options opt;
                opt.create_if_missing = 1;
                int n0 = 0;
                n = s1.find(',', 0);
                opt.max_open_files = std::stoi(trim2(s1.substr(0, n)));
                n0 = n+1;
                n = s1.find(',', n0);
                opt.write_buffer_size = std::stoi(trim2(s1.substr(n0, n-n0))) * (1 << 20); //MB
                n0 = n+1;
                n = s1.find(',', n0);
                // opt.block_size = std::stoi(trim2(s1.substr(n0, n-n0))) * (1 << 10); //KB
                n0 = n+1;
                auto blockCacheS = trim2(s1.substr(n0));
                int blockCacheI = -1;
                try { blockCacheI = std::stoi(blockCacheS); } catch(std::exception e) { }
                
                for(int i = 0; i < ss.size(); i++) {
                    leveldb::Options opt2 = opt;
                    // // comment these out: rocksdb now uses different block_cache configuration
                    // // so no standard way to do this for leveldb and rocksdb
                    // if(blockCacheI == -1) {
                    //     opt2.block_cache = blockCache_[blockCacheS];
                    // } else {
                    //     opt2.block_cache = leveldb::NewLRUCache(blockCacheI * (1 << 20));
                    //     caches_.push_back(opt2.block_cache);
                    // }
                    auto l = std::make_shared<LeveldbLogger>(s2 + "." + ss[i]);
                    opt2.info_log = l;
                    loggers_.push_back(l);
                    int k = -1;
                    try { k = std::stoi(ss[i]); }  catch(std::exception e) { }
                    if(k != -1) {
                        auto& x = (s2 == "kind" ? kindOptions_ : indexOptions_);
                        x[k] = opt2;
                    } else if(ss[i] == "default") {
                        auto& x = (s2 == "kind" ? defKindOption_ : defIndexOption_);
                        x = opt2;
                    } 
                }
            }
        }
    }
}

} //close namespace ndb
} // close namespace ugorji

