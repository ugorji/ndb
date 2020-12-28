COMMON = $(CURDIR)/../cc-common
ROCKSDBLIBDIR = $(CURDIR)

include $(COMMON)/Makefile.include.mk

# LDFLAGS = -lpthread -lglog -lsnappy -lbz2 -lz -L$(ROCKSDBLIBDIR) -lrocksdb -g
LDFLAGS = -lpthread -lglog -lzstd -L$(ROCKSDBLIBDIR) -lrocksdb -g

objFiles = \
	$(BUILD)/ugorji/ndb/manager.o \
	$(BUILD)/ugorji/ndb/conn.o \
	$(BUILD)/ugorji/ndb/ndb.o \
	$(BUILD)/ugorji/ndb/ndb-c.o \
	$(BUILD)/ndbserver_main.o \


all: .common.all .shlib $(BUILD)/__ndbserver

clean:
	rm -f $(BUILD)/*

.shlib: $(BUILD)/libndb.a $(BUILD)/libndb.so

$(BUILD)/libndb.so: $(objFiles) | $(BUILD)
	mkdir -p $(BUILD) && \
	$(CXX) -shared -fPIC -o $(BUILD)/libndb.so $(objFiles) $(commonObjFiles) $(LDFLAGS)

$(BUILD)/libndb.a: $(objFiles) | $(BUILD)
	mkdir -p $(BUILD) && \
	$(AR) rcs $(BUILD)/libndb.a $^ $(commonObjFiles)

$(BUILD)/__ndbserver: $(BUILD)/libndb.a
	$(CXX) -o $(BUILD)/__ndbserver $^ $(LDFLAGS)

server:
	ulimit -c unlimited && \
	$(BUILD)/__ndbserver -p 9999 -s 1 16 -w -1 -x true -k false init.cfg

# To generate suppressions file, remove suppressions arg in server.valgrind below,
# and then run:
#
#    f1=$(mktemp)
#    make  server.valgrind 2>&1 | ~/.local/bin/parse-valgrind-suppressions.sh > $f1
#
# copy and paste from that file into valgrind.suppressions.txt,
# and trim using ... appropriately
#
# Note: parse-valgrind-suppressions.sh came from:
# - https://wiki.wxwidgets.org/Parse_valgrind_suppressions.sh
# - https://wiki.wxwidgets.org/Valgrind_Suppression_File_Howto

server.valgrind:
	ulimit -c unlimited && \
	valgrind -s --track-origins=yes --leak-check=full \
	--show-reachable=yes \
	--gen-suppressions=all --suppressions=$(SRC)/valgrind.suppressions.txt \
	$(BUILD)/__ndbserver -p 9999 -s 1 16 -w -1 -x true -k false init.cfg

# This creates ndbserver that expects to load libndb at runtime
# $(BUILD)/ndbserver: $(BUILD)/libndb.so
# 	$(CXX) -o $(BUILD)/ndbserver $(BUILD)/ndbserver_main.o -L$(BUILD) -lndb $(LDFLAGS)

# This runs the server, using the dynamic library
# server:
# 	ulimit -c unlimited
# 	LD_LIBRARY_PATH=$(CURDIR)/ $(CURDIR)/ndbserver -p 9999 -s 1 16 -w `nproc` -x true -k false init.cfg

# .clean: .util.lib.clean .codec.lib.clean .ndb.lib.clean .ndbserver.app.clean
#	rm -f $(BUILD)/libndb.so
#	rm -f $(BUILD)/my_gtest

# distclean: .ndbserver.distclean
# .%.distclean:
# 	rm -rf $(BUILD)/$*

# test -f $(CURDIR)/core && mv $(CURDIR)/core $(CURDIR)/core__$(date '+%m%d%Y_%H%M%S').bak

# Note that when linking, the precedence for finding libraries is:
# - search -L location
# - search standard locations
# - search LIBRARY_PATH
# Also, if a directory has both dynamic/shared (.so) and static libraries (.a), the static library is used.

# If you don't want to see any output if nothing to be done,
# make the recipe: @:
