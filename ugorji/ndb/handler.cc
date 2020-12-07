#include <unistd.h>
#include <ugorji/util/logging.h>
#include <ugorji/util/bigendian.h>

#include "conn.h"

namespace ugorji { 
namespace ndb { 

const size_t FD_BUF_INCR = 256; // ugorji::conn::CONN_BUF_INCR;

void ConnHandler::stateFor(int fd, connFdStateMach** x) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = clientfds_.find(fd);
    if(it == clientfds_.end()) {
        auto fdbufs = new connFdStateMach(fd);
        *x = fdbufs;
        clientfds_[fd] = fdbufs;
        LOG(INFO, "Adding Connection Socket fd: %d", fd);
    } else {
        *x = it->second;        
    }
}

void ConnHandler::unregisterFd(int fd, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = clientfds_.find(fd);
    if(it != clientfds_.end()) {
        LOG(INFO, "Removing socket fd: %d", fd);
        delete it->second; // TODO: does this free the memory for the connFdStateMach?
        clientfds_.erase(it);
    }
}

void ConnHandler::handleFd(int fd, std::string* err) {
    connFdStateMach* h;
    stateFor(fd, &h);
    // h is either waiting, reading, processing, writing
    std::lock_guard<std::mutex> lk(h->mu_);
    switch(h->state_) {
    case ugorji::conn::CONN_READY:           
        doStartFd(h, err);
        break;
    case ugorji::conn::CONN_READING:
        doReadFd(h, err);
        break;
    case ugorji::conn::CONN_PROCESSING:
        doProcessFd(h, err);
        break;
    case ugorji::conn::CONN_WRITING:
        doWriteFd(h, err);
    }
}

void ConnHandler::doStartFd(connFdStateMach* x, std::string* err) {
    auto fd = x->fd_;
    x->reinit();

    while(true) {
        uint8_t arr[8];
        int n2 = ::read(fd, arr, 8);
        if(n2 <= 0) { // if n2 == 0, EOF (which is an error as we expect something)
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK) return;
            snprintf(errbuf_, 128, "read returned %d, with errno: %s", n2, ugorji::conn::errnoStr().c_str());
            *err = std::string(errbuf_);
            x->reinit();
            return;            
        }
        
        auto numBytesForLen = arr[0];
        if(numBytesForLen > 7) {
            snprintf(errbuf_, 128, "expect up to 7 bytes for reading length, but received %d", numBytesForLen);
            *err = std::string(errbuf_);
            return;
        }

        n2 = numBytesForLen+1;
        if(n2 < 8) {
            ::slice_bytes_expand(&x->in_, 8-n2);
            memcpy(&x->in_.bytes.v[0], &arr[n2], 8-n2);
            x->in_.bytes.len = 8-n2;
            memmove(&arr[8-n2], &arr[0], n2);
            for(int i = 0; i < (8-n2); i++) {
                arr[i] = 0;
            }
        }
        
        x->reqlen_ = util_big_endian_read_uint64(arr);
        x->state_ = ugorji::conn::CONN_READING;
        doReadFd(x, err);
        return;
    }
    return;
}

void ConnHandler::doReadFd(connFdStateMach* x, std::string* err) {
    auto fd = x->fd_;
    LOG(TRACE, "<conn-hdlr>: reading fd: %d", fd);
    int n2 = 0;
    while(x->reqlen_ > x->in_.bytes.len) {
        n2 = x->reqlen_ - x->in_.bytes.len;
        if(n2 > FD_BUF_INCR) n2 = FD_BUF_INCR;
        ::slice_bytes_expand(&x->in_, n2);
        n2 = ::read(fd, &x->in_.bytes.v[x->in_.bytes.len], n2);
        if(n2 > 0) {
            x->in_.bytes.len += n2;
            continue;
        }
        if(n2 == 0) return; // EOF
        if(errno == EINTR) continue;
        if(errno == EAGAIN || errno == EWOULDBLOCK) return;
        snprintf(errbuf_, 128, "read returned %d, with errno: %s", n2, ugorji::conn::errnoStr().c_str());
        *err = std::string(errbuf_);
        x->reinit();
        return;
    }

    x->state_ = ugorji::conn::CONN_PROCESSING;
    doProcessFd(x, err);
}

void ConnHandler::doProcessFd(connFdStateMach* x, std::string* err) {
    char* cerr = nullptr;
    reqHdlr_->handle(x->in_, x->out_, &cerr);
    if(cerr != nullptr) {
        *err = std::string(cerr);
        x->reinit();
        return;
    }
    x->state_ = ugorji::conn::CONN_WRITING;
    doWriteFd(x, err);
}

void ConnHandler::doWriteFd(connFdStateMach* x, std::string* err) {
    auto fd = x->fd_;
    LOG(TRACE, "<conn-hdlr>: writing fd: %d", fd);
    while(x->cursor_ < x->out_.bytes.len) {
        int n2 = ::write(fd, &x->out_.bytes.v[x->cursor_], x->out_.bytes.len-x->cursor_);
        if(n2 < 0) {
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK) return;
            snprintf(errbuf_, 128, "write returned %d, with errno: %s", n2, ugorji::conn::errnoStr().c_str());
            *err = std::string(errbuf_);
            x->reinit();
            return;
        }
        x->cursor_ += n2;
    }
    x->reinit();
}

} //close namespace ndb
} // close namespace ugorji

