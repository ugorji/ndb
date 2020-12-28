#pragma once

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
    size_t reqlen_;
    size_t cursor_;
    ugorji::conn::ConnState state_;
    explicit connFdStateMach(int fd) : fd_(fd) { reinit(); };
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
    explicit ReqHandler(Manager* n) : mgr_(n) { }
    ~ReqHandler() { }
};

class ConnHandler : public ugorji::conn::Handler {
private:
    ReqHandler* reqHdlr_;
    char errbuf_[128] {};
    std::mutex mu_;
    std::unordered_map<int,std::unique_ptr<connFdStateMach>> clientfds_;
    connFdStateMach& stateFor(int fd);
    void doStartFd(connFdStateMach& x, std::string& err);
    void doReadFd(connFdStateMach& x, std::string& err);
    void doProcessFd(connFdStateMach& x, std::string& err);
    void doWriteFd(connFdStateMach& x, std::string& err);
    // void stopFds();
    void acceptFd(int fd, std::string& err);
public:
    explicit ConnHandler(ReqHandler* reqHdlr) : ugorji::conn::Handler(), reqHdlr_(reqHdlr) {}
    ~ConnHandler() {} 
    void handleFd(int fd, std::string& err) override;
    void unregisterFd(int fd, std::string& err) override;
};

}
}

