# Out-of-tree Yosys plugin: OpenSTA Network adapter over RTLIL.
#
# Point these at your checkouts (absolute or relative to this directory):
#   YOSYS_SRC  yosys source tree with kernel/netview.h, built into $(YOSYS_SRC)/build
#   OPENSTA    The-OpenROAD-Project/OpenSTA, built into $(OPENSTA)/build
#              (RelWithDebInfo or Release with IPO off, and -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
# They are exposed to the test scripts as the deps/ symlinks.
YOSYS_SRC ?= ../yosys-opensta
OPENSTA ?= ../OpenSTA

YOSYS_BUILD := $(abspath $(YOSYS_SRC))/build
YOSYS_CONFIG := $(YOSYS_BUILD)/yosys-config
YOSYS := $(YOSYS_BUILD)/yosys
STA_ROOT := $(abspath $(OPENSTA))

STA_LIBS = $(STA_ROOT)/build/sta_swig.a $(STA_ROOT)/build/libOpenSTA.a
STA_INC = -I$(STA_ROOT)/include -I$(STA_ROOT)/include/sta -I$(STA_ROOT)/build/include/sta

SRCS = src/opensta_net.cc src/yosys_network.cc

# tcl.h, zlib.h and cudd are not part of yosys-config's flags; they come from
# shell.nix (or your system's tcl, zlib and cudd development packages).
HAVE_TOOLCHAIN := $(shell printf '#include <tcl.h>\n#include <zlib.h>\n' | $(CXX) -E -x c++ - > /dev/null 2>&1 && echo yes)

all: opensta_net.so

opensta_net.so: $(SRCS) src/yosys_network.hh $(STA_LIBS) deps
ifneq ($(HAVE_TOOLCHAIN),yes)
	$(error tcl.h or zlib.h not found: build inside the toolchain shell, e.g. nix-shell --run "make $(MAKECMDGOALS)")
endif
	$(YOSYS_CONFIG) --build $@ $(SRCS) $(STA_INC) $(STA_LIBS) -lcudd -lz -ltcl

# Stable paths for the .ys/.tcl test files, whatever the checkouts are called.
.PHONY: deps
deps:
	@mkdir -p deps
	@ln -sfn $(abspath $(YOSYS_SRC)) deps/yosys
	@ln -sfn $(STA_ROOT) deps/OpenSTA

.PHONY: smoke
smoke: opensta_net.so
	$(YOSYS) -m ./opensta_net.so -p 'opensta_net -eval report_units'

.PHONY: test
test: opensta_net.so
	bash test/run.sh
	bash conformance/run.sh

.PHONY: clean
clean:
	rm -f opensta_net.so
	rm -rf conformance/work
	rm -f test/*_yosys.out test/*_yosys.log test/*_ref.out test/stress.diff test/kh_out.sdc
