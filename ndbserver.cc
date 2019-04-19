#include <cstdlib>
#include <iostream>
#include <csignal>
#include <thread>
#include <mutex>
#include <fstream>
#include <cstdint>

#include <ugorji/conn/conn.h>
#include <ugorji/ndb/conn.h>
#include <ugorji/util/logging.h>

ugorji::conn::Server* server_;

void trapSignal(int sig) {
    //We need to trap all signals we depend on, so their default is not called.
    //We depend on SIGUSR1/SIGUSR2 for interrupting blocking calls (e.g. waits, etc),
    //and on SIGTERM/SIGINT for graceful shutdown.
    LOG(INFO, "NdbServer: Trapping signal: %d", sig);
    switch(sig) {
    case SIGUSR1:
    case SIGUSR2:
        //noop
        break;
    case SIGTERM:
    case SIGINT:
        if(server_ != nullptr) {
            server_->stop();
            server_ = nullptr;
        }
        //exit(1);
    }
}

int main(int argc, char** argv) {
    //if(true) { return 0; }
    //setbuf(stdout, nullptr);
    //setbuf(stderr, nullptr);
    ugorji::util::Log::getInstance().minLevel_ = ugorji::util::Log::TRACE;
    ugorji::ndb::Manager mgr;
    int port = 9999;
    int numWorkers = 8;
    bool clearOnStartup = false;
    std::string initfile = "init.cfg";
    mgr.shardMin_ = 1;
    mgr.shardRange_ = 1;
    for(int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if(arg == "-p" || arg == "-port") {
            port = std::stoi(argv[++i]);
        } else if(arg == "-i" || arg == "-initfile") {
            initfile = argv[++i];
        } else if(arg == "-w" || arg == "-workers") {
            numWorkers = std::stoi(argv[++i]);
        } else if(arg == "-k" || arg == "-perkind") {
            mgr.dbPerKind_ = memcmp("true", argv[++i], 4) == 0;
        } else if(arg == "-s" || arg == "-shards") {
            mgr.shardMin_ = (uint16_t)(std::stoi(argv[++i]));
            mgr.shardRange_ = (uint16_t)(std::stoi(argv[++i]));
        } else if(arg == "-h" || arg == "-help") {
            std::cout << "Usage: ndbserver " << std::endl
                      << "\t[-i|-initfile file] Default: init.cfg" << std::endl
                      << "\t[-p|-port portno] Default: 9999"  << std::endl
                      << "\t[-k|-perkind true|false] Default: false" << std::endl
                      << "\t[-s|-shards shardMin shardRange] Default: 1, 1" << std::endl;
            return 0;
        } else if(arg == "-x" || arg == "-clear") {
            clearOnStartup = memcmp("true", argv[++i], 4) == 0;
        }
    }
    {
        std::fstream fs;
        fs.open(initfile);
        if(!fs.is_open()) {
            LOG(ERROR, "<Error>: Manager init failed. Unable to open init file: %s", initfile.c_str());
        }
        mgr.load(fs);
    }
    std::string err;
    if(clearOnStartup) {
        system(("rm -rf " + mgr.basedir_).c_str());
        system(("mkdir -p " + mgr.basedir_).c_str());
    }
    LOG(INFO, "NdbServer: %d, BaseDir: %s, ClearOnStartup: %d, dbPerKind: %d", 
        port, mgr.basedir_.c_str(), clearOnStartup, mgr.dbPerKind_);

    //always install signal handler in main thread, and before making other threads.
    LOG(INFO, "NdbServer: Setup Signal Handler (SIGINT, SIGTERM, SIGUSR1, SIGUSR2)", 0);
    struct sigaction sa;
    sa.sa_handler = trapSignal;
    sa.sa_flags = 0;
    
    sigaction(SIGTERM, &sa, nullptr); // kill <pid>
    sigaction(SIGINT,  &sa, nullptr);  // ctrl-c
    sigaction(SIGUSR1, &sa, nullptr); 
    sigaction(SIGUSR2, &sa, nullptr); 
    
    ugorji::ndb::ReqHandler reqHdlr(&mgr);  
    auto fn = [&] (slice_bytes x1, slice_bytes& x2, char** x3) { reqHdlr.handle(x1, x2, x3); };
    // std::function<void (slice_bytes, slice_bytes&, char**)> fn = reqHdlr.handle;
    ugorji::conn::Server server(port, numWorkers, SIGUSR1, fn);
    server_ = &server;
    
    //Need to do all server work in different thread (from signal handling thread),
    //else some signals handling, condition variables, etc just do not work (with no error).
    //server.start(&err);
    std::thread thr(&ugorji::conn::Server::start, &server, &err);
    thr.join();
    
    int exitcode = (err.size() == 0) ? 0 : 1;
    //std::cerr << err << std::endl;
    if(err.size() > 0) LOG(ERROR, "%s", err.c_str());
    
    LOG(INFO, "<main>: shutdown completed", 0);
    return exitcode;
}

