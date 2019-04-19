# ndb

ndb is a NoSQL schema-less database server for storage of structured
entities with high performance, rich API, strong consistency for reads
and queries (including ancestor queries), and efficient inserts/updates
that scales to petabytes of data (big data) and unlimited rows.

It provides a `linux`-based server executable and 
client `.so` shared libraries.

Communication between the client and server happens over one 
of the following encoding formats:
- cbor (preferred - via libcbor go-codec libraries)
- msgpack (via lipmsgpack and go-codec libraries)
- simple (for testing purposes - via c_cpp/ code and go-codec libraries)

# Installation

It only builds and runs on linux. 
It uses epoll internally to serve multiple connections in a very fast and 
performant way.

The datastore build depends on rocksdb, which also depends on some libraries:

    leveldb:  depends on: snappy, tcmalloc
    rocksdb   depends on: zlib, bzip2, snappy, gflags, tcmalloc
    snappy:   depends on: tcmalloc
    tcmalloc: depends on: libunwind

The easiest way to grab the dependencies is to install the libraries 
packaged and provided by your OS maintainers e.g. ubuntu.

```sh
sudo apt-get install libgflags-dev 
sudo apt-get install libsnappy-dev
sudo apt-get install zlib1g-dev
sudo apt-get install libbz2-dev
sudo apt-get install liblz4-dev
sudo apt-get install libzstd-dev
sudo apt-get install librocksdb-dev
sudo apt-get install libgoogle-glog-dev
sudo apt-get install libcbor-dev
sudo apt-get install libmsgpack-dev
sudo apt-get install libleveldb-dev
sudo apt-get install libgtest-dev
```

Note that `libgtest-dev` on ubuntu only install sources, so you have 
to build it appropriately.

```sh
sudo apt-get install cmake # install cmake
cd /usr/src/gtest
sudo cmake CMakeLists.txt
sudo make
sudo ln -s libgtest.a /usr/lib/
sudo ln -s libgtest_main.a /usr/lib/
```

It may be necessary to interact with libndb from within `go`. 
If so, that go program will depend on the 
shared library `ndb` which is currently only supported on linux.
The `go` application MUST then be built on
a linux machine. You will need to download the go installer there.

```sh
mkdir ~/opt && cd ~/opt
wget https://dl.google.com/go/go1.12.4.linux-amd64.tar.gz
tar xzf https://dl.google.com/go/go1.12.4.linux-amd64.tar.gz
```

# Building

To build, the `cc-common` project should be available.

By default, it is a sibling folder to this `ndb` folder.

```
make clean all
```

If `cc-common` is in a different location, pass it during make.

```
make COMMON=/__where_cc_common_dir_is__ clean all
```

## Possible errors and resolution

You might get an error of the form 
```
libndb.so: undefined reference to `typeinfo for rocksdb::Logger'
```

This is because rocksdb is built in release mode without RTTI information,
and rocksdb did not create a definition for the virtual constructor and 
methods in the Logger class.

To fix, you can either build rocksdb yourself, with the command like

```
# Build rocksdb static library
git clone https://github.com/facebook/rocksdb.git
make USE_RTTI=1 static_lib

# Build ndb using this one, running in the .../ndb directory
# (assuming rocksdb built into `.../rocks/db/dir`)
make ROCKSDBLIBDIR=.../rocks/db/dir all
```

OR edit the /usr/include/rocksdb/env.h as seen below
https://github.com/facebook/rocksdb/pull/5208 

```
800c800
<   virtual ~Logger();
---
>   virtual ~Logger() {}
818c818
<   virtual void Logv(const InfoLogLevel log_level, const char* format, va_list ap);
---
>   virtual void Logv(const InfoLogLevel log_level, const char* format, va_list ap) = 0;
```

# Running

```
make server
```
