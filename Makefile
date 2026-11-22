# vfts Makefile.
# Compiler flags and link line follow /opt/vertica/sdk/examples/makefile (Vertica 26.x):
# -shared -fPIC, _GLIBCXX_USE_CXX11_ABI=1, Vertica.cpp compiled in. Difference: -std=c++17.
#
#   make deps                 download and build ICU, libstemmer_c and CRoaring (third_party/)
#   make                      build build/libvfts.so
#   make test                 engine unit tests, no Vertica needed
#   make tools                build/tools/* (command-line helpers used by tests and the benchmark)
#   make deploy [FENCED=yes|no]
#                             install the library, functions and procedures (default: unfenced)
#   make undeploy             remove them (the index tables stay unless DROP=yes)
#   make clean

SDK_HOME ?= /opt/vertica/sdk
CXX      ?= g++
OPT      ?= -O3
FENCED   ?= no

VERTICA_CXX11_ABI ?= 1

BUILD_DIR := build
LIB       := $(BUILD_DIR)/libvfts.so
TP        := third_party/install

ENGINE_SRC := $(wildcard src/engine/*.cpp)
ENGINE_HDR := $(wildcard src/engine/*.h)
UDX_SRC    := $(wildcard src/udx/*.cpp)
TEST_SRC   := $(wildcard tests/engine/test_*.cpp)
TEST_BIN   := $(patsubst tests/engine/%.cpp,$(BUILD_DIR)/tests/%,$(TEST_SRC))
TOOL_SRC   := $(wildcard tools/*.cpp)
TOOL_BIN   := $(patsubst tools/%.cpp,$(BUILD_DIR)/tools/%,$(TOOL_SRC))

DEPS_LIBS  := $(TP)/lib/libicuuc.a $(TP)/lib/libicudata.a $(TP)/lib/libstemmer.a $(TP)/lib/libroaring.a

BUILD_FLAGS := $(OPT) -ffp-contract=off -std=c++17 $(shell uname -m) $(notdir $(CXX))-$(shell $(CXX) -dumpfullversion 2>/dev/null || $(CXX) -dumpversion)

# -ffp-contract=off: no fused multiply-add, so every node computes bit-identical BM25 scores.
# Never -ffast-math, never -march=native: one .so must run on every node of a cluster.
COMMON_FLAGS := -std=c++17 -g $(OPT) -ffp-contract=off -Wall -pthread -I $(TP)/include \
                -DVFTS_BUILD_FLAGS='"$(BUILD_FLAGS)"' -D_GLIBCXX_USE_CXX11_ABI=$(VERTICA_CXX11_ABI)
UDX_FLAGS    := $(COMMON_FLAGS) -DNDEBUG -fno-plt -I $(SDK_HOME)/include -Wno-unused-value -shared -fPIC
# The static libraries stay private to libvfts.so: their symbols are not exported, so they never
# clash with the ICU of the Vertica server process.
UDX_LINK     := -Wl,--exclude-libs,ALL -Wl,-Bsymbolic -Wl,--no-undefined -ldl

.PHONY: all deps test tools deploy undeploy clean

all: $(LIB)

deps:
	third_party/fetch.sh
	third_party/build.sh

$(DEPS_LIBS):
	@echo "missing $@: run 'make deps' first" && exit 1

$(LIB): $(UDX_SRC) $(wildcard src/udx/*.h) $(ENGINE_SRC) $(ENGINE_HDR) $(SDK_HOME)/include/Vertica.cpp $(DEPS_LIBS) Makefile
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(UDX_FLAGS) -o $@ $(UDX_SRC) $(ENGINE_SRC) $(SDK_HOME)/include/Vertica.cpp $(DEPS_LIBS) $(UDX_LINK)

# The engine as an object archive for tests and tools.
$(BUILD_DIR)/libengine.a: $(ENGINE_SRC) $(ENGINE_HDR) Makefile
	@mkdir -p $(BUILD_DIR)/engine
	@for f in $(ENGINE_SRC); do o=$(BUILD_DIR)/engine/$$(basename $$f .cpp).o; \
	  $(CXX) $(COMMON_FLAGS) -fPIC -c $$f -o $$o || exit 1; done
	ar -crs $@ $(BUILD_DIR)/engine/*.o

$(BUILD_DIR)/tests/%: tests/engine/%.cpp $(wildcard tests/engine/*.h) $(BUILD_DIR)/libengine.a $(DEPS_LIBS)
	@mkdir -p $(BUILD_DIR)/tests
	$(CXX) $(COMMON_FLAGS) -o $@ $< $(BUILD_DIR)/libengine.a $(DEPS_LIBS) -ldl

$(BUILD_DIR)/tools/%: tools/%.cpp $(BUILD_DIR)/libengine.a $(DEPS_LIBS)
	@mkdir -p $(BUILD_DIR)/tools
	$(CXX) $(COMMON_FLAGS) -o $@ $< $(BUILD_DIR)/libengine.a $(DEPS_LIBS) -ldl

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "== $$t"; $$t || exit 1; done
	@echo "All engine tests passed."

tools: $(TOOL_BIN)

deploy: $(LIB)
	scripts/deploy.sh --fenced=$(FENCED)

undeploy:
	scripts/deploy.sh --undeploy

clean:
	rm -rf $(BUILD_DIR)
