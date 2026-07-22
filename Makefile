CXX ?= g++
CXXFLAGS ?= -O3 -g -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS ?= -Iinclude
NATIVE ?= 0
FAST_MATH ?= 0
LTO ?= 0
SIMD_WIDTH ?=

ifeq ($(NATIVE),1)
CXXFLAGS += -march=native -mtune=native
endif
ifeq ($(FAST_MATH),1)
CXXFLAGS += -ffast-math
endif
ifeq ($(LTO),1)
CXXFLAGS += -flto
endif
ifneq ($(SIMD_WIDTH),)
CXXFLAGS += -mprefer-vector-width=$(SIMD_WIDTH)
endif

CORE := src/asset.cpp src/curve_cache.cpp src/curve_payload.cpp
HEADERS := $(wildcard include/nanoxgen/*.h)

.PHONY: all test fast-math-check clean

all: bin/nanoxgen_demo bin/nanoxgen_tests bin/nanoxgen_benchmark bin/nanoxgen_cache_benchmark bin/nanoxgen_precision

bin/nanoxgen_demo: $(CORE) tools/demo.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/demo.cpp -o $@

bin/nanoxgen_tests: $(CORE) tests/test_main.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/test_main.cpp -o $@

bin/nanoxgen_benchmark: $(CORE) tools/benchmark.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/benchmark.cpp -o $@

bin/nanoxgen_cache_benchmark: $(CORE) tools/cache_benchmark.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/cache_benchmark.cpp -o $@

bin/nanoxgen_precision: $(CORE) tools/precision.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/precision.cpp -o $@

test: bin/nanoxgen_tests
	./bin/nanoxgen_tests

fast-math-check:
	./scripts/run_fast_math_comparison.sh

clean:
	rm -f bin/nanoxgen_demo bin/nanoxgen_tests bin/nanoxgen_benchmark bin/nanoxgen_cache_benchmark bin/nanoxgen_precision
