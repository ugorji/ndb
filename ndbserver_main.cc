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

ugorji::conn::Manager* connmgr_;

// // run Server in main thread, and don't create a thread for the server.
// // If set to false, we can test out signl handling well
// const bool SERVER_IN_MAIN_THREAD = true;

int main(int argc, char** argv) {
    //if(true) { return 0; }
    //setbuf(stdout, nullptr);
    //setbuf(stderr, nullptr);
    ugorji::util::Log::getInstance().minLevel_ = ugorji::util::Log::TRACE;
    ugorji::ndb::Manager mgr;
    int workers = -1;
    int port = 9999;
    // int maxWorkers = -1;
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
            workers = std::stoi(argv[++i]);
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

    if(clearOnStartup) {
        system(("rm -rf " + mgr.basedir_).c_str());
        system(("mkdir -p " + mgr.basedir_).c_str());
    }
    LOG(INFO, "<ndbserver> %d, BaseDir: %s, ClearOnStartup: %d, dbPerKind: %d", 
        port, mgr.basedir_.c_str(), clearOnStartup, mgr.dbPerKind_);

    auto connmgr = std::make_unique<ugorji::conn::Manager>(port, workers);
    connmgr_ = connmgr.get();

    //always install signal handler in main thread, and before making other threads.
    LOG(INFO, "<ndbserver> Setup Signal Handler (SIGINT, SIGTERM, SIGUSR1, SIGUSR2)", 0);
    auto sighdlr = [](int sig) {
                       LOG(INFO, "<simplehttpfileserver> receiving signal: %d", sig);
                       if(connmgr_ != nullptr) {
                           connmgr_->close();
                           connmgr_ = nullptr;
                       }
                   };
    auto sighdlr_noop = [](int sig) {};
    std::signal(SIGINT,  sighdlr); // ctrl-c
    std::signal(SIGTERM, sighdlr); // kill <pid>
    std::signal(SIGUSR1, sighdlr_noop); // used to interrupt epoll_wait
    std::signal(SIGUSR2, sighdlr_noop); // used to interrupt epoll_wait

    std::string err = "";
    connmgr->open(err);
    if(err.size() > 0) {
        LOG(ERROR, "%s", err.data());
        return 1;
    }

    ugorji::ndb::ReqHandler reqHdlr(&mgr);
    
    std::vector<std::unique_ptr<ugorji::ndb::ConnHandler>> hdlrs;
    auto fn = [&]() mutable -> decltype(auto) {
                  auto hh = std::make_unique<ugorji::ndb::ConnHandler>(&reqHdlr);
                  auto hdlr = hh.get();
                  hdlrs.push_back(std::move(hh));
                  return *hdlr;
              };
    
    connmgr->run(fn, true);
    connmgr->wait();
    
    int exitcode = (connmgr->hasServerErrors() ? 1 : 0);

    // ugorji::ndb::ReqHandler reqHdlr(&mgr);
    // auto fn = [&] (slice_bytes x1, slice_bytes& x2, char** x3) { reqHdlr.handle(x1, x2, x3); };
    // std::function<void (slice_bytes, slice_bytes&, char**)> fn = reqHdlr.handle;

    // ugorji::ndb::ConnHandler hdlr(&reqHdlr);
    // ugorji::conn::Handler& chdlr = hdlr;
    // ugorji::conn::Server server(chdlr, port, maxWorkers);
    // server_ = &server;
    
    // //Need to do all server work in different thread (from signal handling thread),
    // //else some signals handling, condition variables, etc just do not work (with no error).
    // std::string err = "";
    // if(SERVER_IN_MAIN_THREAD) {
    //     server_->start(&err);
    // } else {
    //     std::thread thr(&ugorji::conn::Server::start, server_, &err);
    //     thr.join();
    // }
    // int exitcode = (err.size() == 0) ? 0 : 1;
    // //std::cerr << err << std::endl;
    // if(err.size() > 0) LOG(ERROR, "%s", err.c_str());
    
    LOG(INFO, "<main>: shutdown completed", 0);
    return exitcode;
}

