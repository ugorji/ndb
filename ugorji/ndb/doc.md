## Limits

- Values in the datastore cannot be more than 65536 in size.

## ndb as pure C++ application

This will allow the following:

- full control over threads, including ability to start and stop them.
  This is important for supporting ACPI events to enable use of more 
  or less cores.
- No fake query limit. We can give back client as many results as requested.
- Zero-Copy support.
- No keeping memory long unnecessarily.  
  We can stream results to clients quickly.  
  We can send partial results before an error is got.
- Much better performance for same reasons above.
- Much better thread management. CGO usage creates a lot of threads, 
  never destroys them, and they are not necessarily managed well.
- Remove CGO overhead
- Remove RPC Overhead
- Simplifies codebase immensely:
  - No RPC
  - No Need for the elaborate release mechanism across cgo boundary
  - No CGO

On the C++ side, we will have to implement:

- Simple custom protocol 
- LockSet on the data keys (not indexes)
- Threaded server
- Buffered Reader / Writer
- Simple command line arguments
- Logging

### Custom Protocol

The custom protocol is simple. 

A request is a simple byte stream representing the parameters. The first packet 
describes the function, and is followed by the parameters. Each parameter is one of:

The data types supported are:
- descriptor: 01 XX
- boundary: 02 00 ff 00 \r \n
- byte: 03 XX
- uint16 number: 04 XX XX (big-endian)
- uint64 number: 05 XX XX XX XX XX XX XX XX (big-endian)
- []byte: 06 XX XX YY YY ... (XX XX denotes number of bytes. YY YY ... are the bytes)
- error:  07 XX XX YY YY ... 

Each request or response ends with a boundary.
A request is not processed until the boundary is seen.

The functions supported are:

- Update: IN (3 arrays of bytes), OUT (Error | Success string)
- Retrieve: IN (1 array of bytes), OUT (1 array of Error or Success strings)
- Query: IN (Query Parameters), OUT (1 array of Success results, 1 optional error)
- IncrDecr: IN (IncrDecr Parameters), OUT (Error | Success string)
- ...

### LockSet

The locks will now be implemented on the datastore. We can scale out the 
backend but not the datastore at this time.

### Threaded Server

A thread pool with size equal to #CPU will be used on the datastore.

Epoll will be used to manage a potentially high number of clients. 

Since only backends can connect, the backends will be in charge of 
limiting the number of connections they make to the datastore.

Clients will be expected to maintain persistent connections to the database.

Initial setup will not include a handshake. Just the simple connection
and start communicating.

### Buffered Reader / Writer

Socket communication will use a buffered reader and writer.

### Logging

We need support for:

- TIMESTAMP
- LEVEL
- SUBSYSTEM

We are writing a logging framework based on fprintf. We will integrate
our custom logger and pass it in when creating leveldb Logger on the
options passed when opening the database.

### Simple command line parsing

It will support taking parameters for

- dbdir
- port
- clearDatastoreOnStartup

### Quick Graceful Shutdown

For graceful'ish shutdown

- All read which have started must be performed
- All client sockets must be closed
- No more sockets must be accepted
- ServerSocket is closed

## C++ Exceptions usage

This package may throw StreamError to signal errors. We should handle
it and only it appropriately. Other exceptions will crash the program.

## ndbserver Leveldb Model

To mitigate issues with global read stalls, we will segregate the
datastore into multiple leveldb databases. For data, we use either one
database per shard (`basedir/shard-shardid`) or one database per shard
per kind (`basedir/shard-shardid/root_kind-kindid`), and one database
per index (`basedir/index-indexid`). This is ok because:

- we don't do anything across kinds.
- all entities and their children live in the same database, to allow
  for consistent updates (e.g. User and UserProfile, etc).

This gives the following benefits:

- A shard or an index can easily be moved to a different server. Since a
  shard manages different databases, moving involves no work.
- Indexes can be re-built offline if necessary.

Using different database per kind gives the following benefits:

- Contention (showing up as read latency and stalls due to compaction,
  etc) on a kind (e.g. TopicComments) does not affect other kinds.
- Less number of levels per database, lower size of database, etc
  results in more information at level-0's, and less disk thrashing.

We will still be able to take advantage of:

- Sharing resources. We can still share block caches, etc across databases.
- Background Threads. We should not depend on just one background thread
  to do compaction. Instead, we use a pool of background threads and
  have a work queue per database. This will be done by implementing an
  Env which just overrides the Schedule method of env.cc. (Look at
  env_posix.cc).
  
However, they has the following possible issues:

- Indexes and entities can be out of sync
- We will never be able to support cross-kind queries.
- Too many databases per process. Imagine we have 1 server handling 16
  shards and easily 80 kinds in the app (currently at 30 without not
  much custom kinds). That is 1280 databases in that server. At 150ms to
  open each db, startup will take 192 seconds.
- Too many open files. Not an issue, since linux now supports many open
  files. More databases means many more open files.

We will need a way to configure the kinds, and dbOpen options
(cachesize, etc) for each one. This will be done by providing ndbserver
with a simple configuration file that looks like below:

    # options are: max_open_files, write_buffer_size(MB), block_size(K), block_cache:string or int(MB)
    # defaults are provided for data and indexes.
    # overrides can be done by kind(s)
    # multiple keys can be defined together 
    basedir = 
    block_cache.default,index_default = 64
    kind.default = 200, 4, 4, default
    index.default = 100, 4, 4, index_default
    kind.17,25 = 200, 4, 4, 32 # use separate 32MB block_cache
    index.73 = 200, 4, 4, 32 # use separate 32MB block_cache

ndbserver reads this into an Options struct that looks like:

    struct Options {
        black_cache map<string, leveldb::Cache*>
        kinds map<int, leveldb::Option>
        indexes map<int, leveldb::Option>
    }

The ndbserver command line will take an argument for the shard range
(inclusive).

    ndbserver -shards 1 24 ...

This has the following implications:

- We need to be able to extract root-kindid, shardid from keys for every
  get/update request. This way, we can store in appropriate locations.

Updates/Deletes will include:

- Read old entity metadata (to get the current index rows)
- Compare current index rows with new updates to see delta (add/removes)
  for indexes
- Write/Delete entity
- If success, write/delete index rows for that entity as a batch.

ndbserver doesn't know much about an ndb app. It just knows the following:

- A key is a multiple of 8 bytes.  
  Each set of 8 bytes has a given structure, and the kind, shard and
  localid can be extracted.   
  The first set of 8 bytes is the root ancestor.
- index rows are arbitrary but end with `0` `key` 
- When a key is put, ndbserver stores metadata containing the indexRows.
  So the indexRows can be got easily from the datastore.

All other information (e.g. about how index-row prefixes are created,
etc) are functionality bound to the app or the client (e.g. client.go).
The client is smart and can create appropriate functions to Put, Delete,
Get, Query, etc.

In the first cut, we will have 1 database server with 16 shards, and
default Env (1 background thread). This will be run in development so we
can see how expensive things are. This is also the configuration we will
go live with.

As we grow, we will need to support multiple servers, multiple
background threads and the GO client must be able to infer the right
server to contact for any request (e.g. for indexing, for data,
etc). This will involve more configuration at ndb.json, and the ability
to broadcast configuration changes to live instances.

    servers.data = dserver1,dserver2,...
    servers.index = iserver1,...
    
    server.addr.dserver1 =
    server.addr.iserver1 =
    
    # map shard range to data servers
    shard.0-31 = dserver1
    
    # map indexid to index servers
    indexid.37,23,15 = iserver1
    
This is read into a struct like:

    struct {
        ...
        DataServers: struct {
            Name string
            Addr string
            ShardMin int
            ShardMax int
        }
        IndexServers: struct {
            Name string
            Addr string
            IndexIds []int
        }
    }

### ndbserver Replication / Backup integration

Backup/Replication is integrated into ndbserver.

To support backups, each database instance has a work queue and only one
item from the queue can be serviced at any time. This way, compaction
and backup cannot happen at same time on any database. This can be
supported by a sophisticated Env implementation, via enhance/override
Schedule() method and use EnvWrapper to pass all other calls to
Env::default. The naive implementation is to use a background thread per
database; however, this will cause a lot of un-necessary threads. The
smarter implementation is to use a work queue per database, and to have
a pool of background threads, with the restriction that no two work
items from a database can occur concurrently.

## OPEN ISSUES

- Sometimes, reads after deletes in the leveldb can be slow. It may help
  to do a compact_range on the range of keys deleted after each delete
  call.
- Leveldb Compaction can stall read requests, even frequently. This is
  partly because there is a single background thread for the whole
  process doing all compaction work.
- Currently we set sync = false. We need a model that works
  e.g. fsync=true every second. Each ndb keeps track of last fsync time,
  and update checks it and sets fsync to true if 1 second has elapsed
  since. CompareAndSwap will be used so only one thread calls fsync per
  hour. This way, we're sure that we only lose 1 second worth of data on
  a crash. fsync=true however helps prevent database corruption, which
  would cause a little loss of data, and require possible down time and
  time-intensive repair. It seems the only workable model is fsync=true.
- handle std::bad_alloc (out-of-memory). Maybe reserve memory for the
  database at startup.
