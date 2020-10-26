COMMON = $(CURDIR)/../cc-common
ROCKSDBLIBDIR = $(CURDIR)

SRC = $(CURDIR)
DIST = $(CURDIR)

CFLAGS = -std=c99 -w
CXXFLAGS = -std=c++11 -w
CPPFLAGS = -fPIC -I$(CURDIR) -I$(COMMON) -g
# LDFLAGS = -lpthread -lglog -lsnappy -lbz2 -lz -L$(ROCKSDBLIBDIR) -lrocksdb -g
LDFLAGS = -lpthread -lglog -lzstd -L$(ROCKSDBLIBDIR) -lrocksdb -g

ndbObjFiles = \
	$(SRC)/ugorji/ndb/manager.o \
	$(SRC)/ugorji/ndb/conn.o \
	$(SRC)/ugorji/ndb/ndb.o \
	$(SRC)/ugorji/ndb/c.o \

commonObjFiles = \
	$(COMMON)/ugorji/util/lockset.o \
	$(COMMON)/ugorji/util/logging.o \
	$(COMMON)/ugorji/util/bigendian.o \
	$(COMMON)/ugorji/util/slice.o \
	$(COMMON)/ugorji/util/bufio.o \
	$(COMMON)/ugorji/codec/codec.o \
	$(COMMON)/ugorji/codec/simplecodec.o \
	$(COMMON)/ugorji/codec/binc.o \
	$(COMMON)/ugorji/conn/conn.o \

.PHONY: clean all

all: .common.all .ndb.shlib $(DIST)/__ndbserver
	@:

clean: .common.clean
	rm -f $(CURDIR)/ugorji/ndb/*.o $(DIST)/ndbserver.o $(DIST)/libndb.a $(DIST)/__ndbserver

.ndb.shlib: $(DIST)/libndb.a

$(DIST)/libndb.so: $(DIST) $(ndbObjFiles)
	$(CXX) -shared -fPIC -o $(DIST)/libndb.so $(ndbObjFiles) $(commonObjFiles) $(LDFLAGS)

$(DIST)/libndb.a: $(DIST) $(ndbObjFiles)
	$(AR) rcs $(DIST)/libndb.a $(ndbObjFiles) $(commonObjFiles)

$(DIST)/__ndbserver: $(DIST)/ndbserver.o $(DIST)/libndb.a
	$(CXX) -o $(DIST)/__ndbserver $(DIST)/ndbserver.o $(DIST)/libndb.a $(LDFLAGS)

.DEFAULT:
	:

server:
	ulimit -c unlimited
	$(DIST)/__ndbserver -p 9999 -s 1 16 -w `nproc` -x true -k false init.cfg

.common.all:
	@$(MAKE) -C $(COMMON) all

.common.clean:
	@$(MAKE) -C $(COMMON) clean


# This creates ndbserver that expects to load libndb at runtime
# $(DIST)/ndbserver: $(DIST)/ndbserver.o $(DIST)/libndb.so
# 	$(CXX) -o $(DIST)/ndbserver $(DIST)/ndbserver.o -L$(DIST) -lndb $(LDFLAGS)

# This runs the server, using the dynamic library
# server:
# 	ulimit -c unlimited
# 	LD_LIBRARY_PATH=$(CURDIR)/ $(CURDIR)/ndbserver -p 9999 -s 1 16 -w `nproc` -x true -k false init.cfg

# .clean: .util.lib.clean .codec.lib.clean .ndb.lib.clean .ndbserver.app.clean
#	rm -f $(DIST)/libndb.so
#	rm -f $(DIST)/my_gtest

# distclean: .ndbserver.distclean
# .%.distclean:
# 	rm -rf $(DIST)/$*

# test -f $(CURDIR)/core && mv $(CURDIR)/core $(CURDIR)/core__$(date '+%m%d%Y_%H%M%S').bak

# Note that when linking, the precedence for finding libraries is:
# - search -L location
# - search standard locations
# - search LIBRARY_PATH
# Also, if a directory has both dynamic/shared (.so) and static libraries (.a), the static library is used.

