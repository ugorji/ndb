#pragma once

#include "ndb.h"
#include <rocksdb/env.h>

namespace ugorji { 
namespace ndb { 

//Thread Locals are tricky to use. 
//  - thread_local is not supported till GCC 4.8
//  - If you don't wrap in a class, you see it only in the scope
//    that you update it (e.g. see in conn.cc, but it's 0 in ndb.cc)
//  - Even after using a class, you have to declare it somewhere (e.g. ndb.cc)
//    and do it only once, else you get unresolved or re-defined references error.
class TLS {
public:
    // static __thread int reqNum; //defined in ndb.cc 
};

//class LeveldbLogger : public leveldb::NullLogger {
//class LeveldbLogger {
class LeveldbLogger : public leveldb::Logger {
    std::string prefix_;
public:
    explicit LeveldbLogger(const std::string& name) :
        leveldb::Logger(leveldb::InfoLogLevel::INFO_LEVEL),
        prefix_("<leveldb:" + name + "> ") {}
    LeveldbLogger() : LeveldbLogger("ndb") {}
    ~LeveldbLogger() {} //TODO: should we do more?
    using leveldb::Logger::Close;
    leveldb::Status Close() override {
        return leveldb::Status::NotSupported("explicit close is unsupported by custom leveldb logger");
    }
    using leveldb::Logger::Logv;
    void Logv(const char* format, va_list ap) override {
        Logv(GetInfoLogLevel(), format, ap);
    }
    void Logv(const leveldb::InfoLogLevel log_level, const char* format, va_list ap) override;
};

class Manager {
private:
    ugorji::util::LockSet locks_;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<leveldb::Cache>> blockCache_ ;
    std::unordered_map<int, leveldb::Options> kindOptions_ ;
    std::unordered_map<int, leveldb::Options> indexOptions_ ;
    leveldb::Options defKindOption_ ;
    leveldb::Options defIndexOption_ ;
    std::vector<std::shared_ptr<leveldb::Cache>> caches_;
    std::vector<std::shared_ptr<leveldb::Logger>> loggers_ ;
    std::unordered_map<uint8_t, Ndb*> indexDbs_ ;
    std::unordered_map<uint16_t, Ndb*> shardDbs_ ;
    std::unordered_map<uint16_t, std::unique_ptr<std::unordered_map<uint8_t, Ndb*>>> perkindDbs_;
    std::vector<std::unique_ptr<Ndb>> dbs_;
    Ndb* openDb(const std::string& dbdir, leveldb::Options& opt, std::string& err);
    Ndb* shardDb(uint16_t shard, std::string& err);
    Ndb* perkindDb(uint16_t shard, uint8_t kind, std::string& err);
public:
    bool dbPerKind_ ;
    uint16_t shardMin_ = 1;
    uint16_t shardRange_ = 1;
    std::string basedir_;
    Ndb* dataDb(uint16_t shard, uint8_t kind, std::string& err);
    Ndb* indexDb(uint8_t index, std::string& err);
    void load(std::istream& initfs);
    Ndb* ndbForKey(leveldb::Slice& key, std::string& err);
    ~Manager() {};
};

void extractKeyParts(const uint8_t* ikey, 
                     const size_t sz,
                     uint16_t* shardid,
                     uint8_t* discrim,
                     uint8_t* indexid,
                     uint8_t* rootkindid,
                     uint8_t* kindid, 
                     uint8_t* shapeid
);

};
}

