# Makefile
# rules (always with .out)
# SRC-X.out += abc        # extra source: abc.c
# MOD-X.out += abc        # extra module: abc.c abc.h
# ASM-X.out += abc        # extra assembly: abc.S
# DEP-X.out += abc        # extra dependency: abc
# FLG-X.out += -finline   # extra flags
# LIB-X.out += abc        # extra -labc options

# X.out : xyz.h xyz.c # for extra dependences that are to be compiled/linked.

# X => X.out
TARGETS += xdbdemo xdbtest xdbexit
# X => X.c only
SOURCES +=
SOURCES += $(EXTRASRC)
# X => X.S only
ASSMBLY +=
ASSMBLY += $(EXTRAASM)
# X => X.c X.h
MODULES += lib kv wh blkio sst xdb
MODULES += $(EXTRAMOD)
# X => X.h
HEADERS += ctypes
HEADERS += $(EXTRAHDR)

# EXTERNSRC/EXTERNDEP do not belong to this repo.
# extern-src will be linked
EXTERNSRC +=
# extern-dep will not be linked
EXTERNDEP +=

FLG +=
LIB += m


#### all
.PHONY : all
all : bin libremixdb.so sotest.out

libremixdb.so : Makefile Makefile.common lib.h kv.h wh.h blkio.h sst.h xdb.h lib.c kv.c wh.c blkio.c sst.c xdb.c
	$(eval ALLFLG := $(CSTD) $(EXTRA) $(FLG) -shared -fPIC)
	$(eval ALLLIB := $(addprefix -l,$(LIB) $(LIB-$@)))
	$(CCC) $(ALLFLG) -o $@ lib.c kv.c wh.c blkio.c sst.c xdb.c $(ALLLIB)
	strip --strip-all --discard-all @remixdb.strip $@

sotest.out : sotest.c Makefile Makefile.common libremixdb.so remixdb.h
	$(eval ALLFLG := $(CSTD) $(EXTRA) $(FLG))
	$(CCC) $(ALLFLG) -o $@ $< -L . -lremixdb
	@echo "$(shell $(TPUT) setaf 4)Now run $ LD_LIBRARY_PATH=. ./sotest.out$(shell $(TPUT) sgr0)"

.PHONY : install
install : libremixdb.so remixdb.h
	install -D --mode=0755 libremixdb.so $(PREFIX)/lib/libremixdb.so
	install -D --mode=0644 remixdb.h $(PREFIX)/usr/include/remixdb.h

# append common rules (have to do it here)
include Makefile.common


# ===== 本项目新增的实验程序构建规则 =====
# xdbdemo 需要链接新增的 C++ 因果推断模块与实验模块，因此提供单独构建规则。
xdbdemo.out: xdbdemo.c lib.c kv.c wh.c blkio.c sst.c xdb.c cdit_module.cpp merge_cg.cpp causal_meta.cpp experiment/basin_experiment.cpp experiment/fluxnet_experiment.cpp experiment/windtunnel_experiment.cpp experiment/cdit_experiment.cpp experiment/cdit_merge_basin_experiment.cpp experiment/cdit_merge_fluxnet_experiment.cpp experiment/cdit_merge_windtunnel_experiment.cpp experiment/realdataset_fault_generation.cpp experiment/outdb_basin_experiment.cpp experiment/outdb_fluxnet_experiment.cpp experiment/outdb_windtunnel_experiment.cpp lib.h kv.h wh.h blkio.h sst.h xdb.h ctypes.h causal_meta.h cdit_module.h merge_cg.h experiment/basin_experiment.h experiment/fluxnet_experiment.h experiment/windtunnel_experiment.h experiment/cdit_experiment.h experiment/cdit_merge_basin_experiment.h experiment/cdit_merge_fluxnet_experiment.h experiment/cdit_merge_windtunnel_experiment.h experiment/realdataset_fault_generation.h experiment/outdb_basin_experiment.h experiment/outdb_fluxnet_experiment.h experiment/outdb_windtunnel_experiment.h Makefile Makefile.common
	$(CCC) $(CSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o lib.o lib.c
	$(CCC) $(CSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o kv.o kv.c
	$(CCC) $(CSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o wh.o wh.c
	$(CCC) $(CSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o blkio.o blkio.c
	$(CCC) $(CSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o sst.o sst.c
	$(CCC) $(CSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o xdb.o xdb.c
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o cdit_module.o cdit_module.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o merge_cg.o merge_cg.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o causal_meta.o causal_meta.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o basin_experiment.o experiment/basin_experiment.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o fluxnet_experiment.o experiment/fluxnet_experiment.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o windtunnel_experiment.o experiment/windtunnel_experiment.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o cdit_experiment.o experiment/cdit_experiment.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o cdit_merge_basin_experiment.o experiment/cdit_merge_basin_experiment.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o cdit_merge_fluxnet_experiment.o experiment/cdit_merge_fluxnet_experiment.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o cdit_merge_windtunnel_experiment.o experiment/cdit_merge_windtunnel_experiment.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o realdataset_fault_generation.o experiment/realdataset_fault_generation.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o outdb_basin_experiment.o experiment/outdb_basin_experiment.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o outdb_fluxnet_experiment.o experiment/outdb_fluxnet_experiment.cpp
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o outdb_windtunnel_experiment.o experiment/outdb_windtunnel_experiment.cpp
	$(CCC) $(CSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -c -o xdbdemo_main.o xdbdemo.c
	$(XCC) $(XSTD) $(EXTRA) $(filter-out -flto,$(FLG)) -o $@ xdbdemo_main.o lib.o kv.o wh.o blkio.o sst.o xdb.o cdit_module.o merge_cg.o causal_meta.o basin_experiment.o fluxnet_experiment.o windtunnel_experiment.o cdit_experiment.o cdit_merge_basin_experiment.o cdit_merge_fluxnet_experiment.o cdit_merge_windtunnel_experiment.o realdataset_fault_generation.o outdb_basin_experiment.o outdb_fluxnet_experiment.o outdb_windtunnel_experiment.o $(addprefix -l,$(LIB))
