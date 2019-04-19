#ifndef _incl_ugorji_ndb_conn_
#define _incl_ugorji_ndb_conn_

#include "manager.h"

namespace ugorji { 
namespace ndb { 

class ReqHandler {
private:
    Manager* mgr_;
public:
    void handle(slice_bytes in, slice_bytes& out, char** err);
    ReqHandler(Manager* n) : mgr_(n) { }
    ~ReqHandler();
};

}
}

#endif //_incl_ugorji_ndb_conn_
