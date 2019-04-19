#ifndef _incl_ugorji_ndb_c_
#define _incl_ugorji_ndb_c_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

// #include <ugorji/util/slice.h>
// inline slice.h here, so that this c.h file is standalone
// keep this block (_incl_ugorji_util_slice_) consistent with slice.h
#ifndef _incl_ugorji_util_slice_
#define _incl_ugorji_util_slice_
typedef struct slice_bytes_t {
    char* v;
    size_t len;
} slice_bytes_t;
typedef struct slice_bytes {
    slice_bytes_t bytes;
    size_t cap;
} slice_bytes;
extern void slice_bytes_append(slice_bytes* v, void* b, size_t len);
extern void slice_bytes_expand(slice_bytes* v, size_t len);
extern void slice_bytes_append_1(slice_bytes* v, uint8_t bd);
#endif //_incl_ugorji_util_slice_


typedef struct ndb_t ndb_t;

// typedef struct ndb_env_t ndb_env_t;
// 
// extern void ndb_env_create(ndb_env_t* env);
// extern void ndb_open(char* basedir, ndb_env_t* env, ndb_t* dbs, char** err);

// extern uint32_t ndb_close(ndb_t** db, size_t num, char*** err);
// extern uint32_t ndb_free(void** ptrs, size_t num);

extern void ndb_noop();
extern void ndb_init();
extern void ndb_release(uint32_t* reqkey, 
                        size_t num);

extern uint32_t ndb_open(slice_bytes_t basedir, 
                         ndb_t** db, 
                         slice_bytes_t* err);

extern uint32_t ndb_get(ndb_t* db, 
                        slice_bytes_t key, 
                        slice_bytes_t* val,
                        slice_bytes_t* err);

extern uint32_t ndb_get_multi(ndb_t* db, 
                              size_t numKeys,
                              slice_bytes_t* keys, 
                              slice_bytes_t** vals, 
                              slice_bytes_t** errs);

extern uint32_t ndb_update(ndb_t* db, 
                           slice_bytes_t* putKvs, size_t numPutKvs,
                           slice_bytes_t* dels, size_t numDels,
                           slice_bytes_t* err);

extern uint32_t ndb_query(ndb_t* db, 
                          slice_bytes_t seekpos1, 
                          slice_bytes_t seekpos2,
                          const uint8_t kindid, 
                          const uint8_t shapeid,
                          const bool ancestorOnlyC,
                          const bool withCursor,
                          const uint8_t lastFilterOp,     
                          const size_t offset, 
                          const size_t limit, 
                          slice_bytes_t** results,
                          size_t* numResults,
                          slice_bytes_t* err);

extern uint32_t ndb_incr_decr(ndb_t* db, 
                              slice_bytes_t key, bool incr,
                              uint16_t delta,
                              uint16_t initVal,
                              uint64_t* nextVal,
                              slice_bytes_t* err);

#ifdef __cplusplus
}  // end extern "C" 
#endif
#endif //_incl_ugorji_ndb_c_
