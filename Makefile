COMMON = $(CURDIR)/../cc-common
ROCKSDBLIBDIR = $(CURDIR)

include $(COMMON)/Makefile.include.mk

# LDFLAGS = -lpthread -lglog -lsnappy -lbz2 -lz -L$(ROCKSDBLIBDIR) -lrocksdb -g
LDFLAGS = -lpthread -lglog -lzstd -L$(ROCKSDBLIBDIR) -lrocksdb -g

ndbObjFiles = \
	$(SRC)/ugorji/ndb/manager.o \
	$(SRC)/ugorji/ndb/conn.o \
	$(SRC)/ugorji/ndb/handler.o \
	$(SRC)/ugorji/ndb/ndb.o \
	$(SRC)/ugorji/ndb/ndb-c.o \

all: .common.all .shlib $(DIST)/__ndbserver

clean:
	rm -f $(CURDIR)/ugorji/ndb/*.o $(DIST)/ndbserver.o $(DIST)/libndb.a $(DIST)/__ndbserver

.shlib: $(DIST)/libndb.a $(DIST)/libndb.so

$(DIST)/libndb.so: $(ndbObjFiles) | $(DIST)
	$(CXX) -shared -fPIC -o $(DIST)/libndb.so $(ndbObjFiles) $(commonObjFiles) $(LDFLAGS)

$(DIST)/libndb.a: $(ndbObjFiles) | $(DIST)
	$(AR) rcs $(DIST)/libndb.a $(ndbObjFiles) $(commonObjFiles)

$(DIST)/__ndbserver: $(DIST)/ndbserver.o $(DIST)/libndb.a
	$(CXX) -o $(DIST)/__ndbserver $(DIST)/ndbserver.o $(DIST)/libndb.a $(LDFLAGS)

server:
	ulimit -c unlimited && \
	$(DIST)/__ndbserver -p 9999 -s 1 16 -w -1 -x true -k false init.cfg

server.valgrind:
	ulimit -c unlimited && \
	valgrind -s --track-origins=yes --leak-check=full \
	$(DIST)/__ndbserver -p 9999 -s 1 16 -w -1 -x true -k false init.cfg

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

# If you don't want to see any output if nothing to be done,
# make the recipe: @:
