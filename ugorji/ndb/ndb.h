#pragma once

#define NDB_DEBUG 0

#include <stdint.h>
#include <ugorji/util/lockset.h>
#include <rocksdb/db.h>

namespace leveldb = rocksdb;

namespace ugorji { 
namespace ndb {

const std::string BAD_ITERATOR_ERROR = "<BAD_ITERATOR>";
const std::string NOT_FOUND_ERROR = "<App_Entity_Not_Found>"; //match ugorji.net/util.ErrEntityNotFoundMsg

enum EntryType     { E_METADATA = 1, E_DATA, E_INDEXROW }; //Must match order in ndb.go
enum Discriminator { D_INDEX = 1, D_ENTITY, D_IDGEN };     //Must match order in ndb.go
enum QueryFilterOp { F_EQ = 1, F_GTE, F_GT, F_LTE, F_LT }; //Must match order in app/appcore.go

class Ndb {
public:
    leveldb::DB* db_;
    leveldb::ReadOptions ropt_;
    leveldb::WriteOptions wopt_;
    ugorji::util::LockSet locks_;
    void gets(
        std::vector<leveldb::Slice>& keys, 
        std::vector<std::string>* values, 
        std::vector<std::string>* errs);
    void get(
        leveldb::Slice key, 
        std::string& value, std::string& err
    );
    void getViaIter(
        leveldb::Iterator* iter, 
        leveldb::Slice key, 
        leveldb::Slice* value, std::string& err
    );
    void update(
        std::vector<leveldb::Slice>& putkeys,
        std::vector<leveldb::Slice>& putvalues, 
        std::vector<leveldb::Slice>& delkeys,
        std::string& err
    );
    void query(
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
    );
    void incrdecr(
        leveldb::Slice key,
        bool incr,
        uint16_t delta,
        uint16_t initVal,
        uint64_t* nextVal,
        std::string& err
    );
    ~Ndb() {
        // db_->CancelAllBackgroundWork(true);
        delete db_;
    }
};

// iterGuard works to ensure the iterator, got from NewIterator, is deleted once out of scope
class iterGuard {
public:
    leveldb::Iterator* iter_;
    explicit iterGuard(leveldb::Iterator* iter) : iter_(iter) {}
    ~iterGuard() { delete iter_; }
};

}
}

