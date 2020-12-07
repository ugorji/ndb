#ifndef _incl_ugorji_ndb_conn_
#define _incl_ugorji_ndb_conn_

#include <ugorji/conn/conn.h>

#include "manager.h"

namespace ugorji { 
namespace ndb { 

class connFdStateMach {
public:
    std::mutex mu_;
    slice_bytes in_;
    slice_bytes out_;
    int fd_;
    int reqlen_;
    int cursor_;
    ugorji::conn::ConnState state_;
    connFdStateMach(int fd) : fd_(fd) { reinit(); };
    ~connFdStateMach() {};
    void reinit() {
        in_.bytes.len = 0;
        out_.bytes.len = 0;
        reqlen_ = 0;
        cursor_ = 0;
        state_ = ugorji::conn::CONN_READY;
    }
};

// class reqFrame {
// public:
//     int fd_;
//     slice_bytes r_;
//     reqFrame(int fd, slice_bytes r) : fd_(fd), r_(r) {};
//     ~reqFrame() {};
// };

class ReqHandler {
private:
    Manager* mgr_;
public:
    void handle(slice_bytes in, slice_bytes& out, char** err);
    ReqHandler(Manager* n) : mgr_(n) { }
    ~ReqHandler() { }
};

class ConnHandler : public ugorji::conn::Handler {
private:
    ReqHandler* reqHdlr_;
    char errbuf_[128];
    std::mutex mu_;
    std::unordered_map<int,connFdStateMach*> clientfds_;
    void stateFor(int fd, connFdStateMach** x);
    void doStartFd(connFdStateMach* x, std::string* err);
    void doReadFd(connFdStateMach* x, std::string* err);
    void doProcessFd(connFdStateMach* x, std::string* err);
    void doWriteFd(connFdStateMach* x, std::string* err);
    // void stopFds();
    void acceptFd(int fd, std::string* err);
public:
    ConnHandler(ReqHandler* reqHdlr) : ugorji::conn::Handler(), reqHdlr_(reqHdlr) {}
    ~ConnHandler() {
        for(auto it = clientfds_.begin(); it != clientfds_.end(); it++) delete it->second;
    } 
    virtual void handleFd(int fd, std::string* err) override;
    virtual void unregisterFd(int fd, std::string* err) override;
};

}
}

#endif //_incl_ugorji_ndb_conn_
