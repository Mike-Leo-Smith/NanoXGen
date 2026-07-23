CXX ?= g++
CXXFLAGS ?= -O3 -g -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS ?= -Iinclude
LDLIBS ?= -lz
NATIVE ?= 0
FAST_MATH ?= 0
LTO ?= 0
SIMD_WIDTH ?=

ifeq ($(NATIVE),1)
CXXFLAGS += -march=native -mtune=native
endif
ifeq ($(FAST_MATH),1)
CXXFLAGS += -ffast-math
else
CXXFLAGS += -ffp-contract=off
endif
ifeq ($(LTO),1)
CXXFLAGS += -flto
endif
ifneq ($(SIMD_WIDTH),)
CXXFLAGS += -mprefer-vector-width=$(SIMD_WIDTH)
endif

CORE := src/asset.cpp src/context.cpp src/curve_cache.cpp src/curve_payload.cpp src/task_executor.cpp src/xgen.cpp src/xgen_classic.cpp src/xgen_classic_runtime.cpp src/xgen_expression.cpp src/xgen_package.cpp src/xpd.cpp
HEADERS := $(wildcard include/nanoxgen/*.h)

.PHONY: all test fast-math-check clean

all: bin/nanoxgen_demo bin/nanoxgen_tests bin/nanoxgen_package_tests bin/nanoxgen_classic_tests bin/nanoxgen_expression_tests bin/nanoxgen_benchmark bin/nanoxgen_cache_benchmark bin/nanoxgen_precision bin/nanoxgen_xgen_inspect bin/nanoxgen_xgen_process bin/nanoxgen_xgen_cache bin/nanoxgen_xgen_read_benchmark bin/nanoxgen_xgen_package bin/nanoxgen_xgen_classic_inspect bin/nanoxgen_xpd_inspect

bin/nanoxgen_demo: $(CORE) tools/demo.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/demo.cpp -o $@ $(LDLIBS)

bin/nanoxgen_tests: $(CORE) tests/test_main.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/test_main.cpp -o $@ $(LDLIBS)

bin/nanoxgen_package_tests: $(CORE) tests/package_test.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/package_test.cpp -o $@ $(LDLIBS)

bin/nanoxgen_classic_tests: $(CORE) tests/classic_test.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/classic_test.cpp -o $@ $(LDLIBS)

bin/nanoxgen_expression_tests: $(CORE) tests/expression_test.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/expression_test.cpp -o $@ $(LDLIBS)

bin/nanoxgen_benchmark: $(CORE) tools/benchmark.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/benchmark.cpp -o $@ $(LDLIBS)

bin/nanoxgen_cache_benchmark: $(CORE) tools/cache_benchmark.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/cache_benchmark.cpp -o $@ $(LDLIBS)

bin/nanoxgen_precision: $(CORE) tools/precision.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/precision.cpp -o $@ $(LDLIBS)

bin/nanoxgen_xgen_inspect: $(CORE) tools/xgen_inspect.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/xgen_inspect.cpp -o $@ $(LDLIBS)

bin/nanoxgen_xgen_process: $(CORE) tools/xgen_process.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/xgen_process.cpp -o $@ $(LDLIBS)

bin/nanoxgen_xgen_cache: $(CORE) tools/xgen_to_nxc.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/xgen_to_nxc.cpp -o $@ $(LDLIBS)

bin/nanoxgen_xgen_read_benchmark: $(CORE) tools/xgen_read_benchmark.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/xgen_read_benchmark.cpp -o $@ $(LDLIBS)

bin/nanoxgen_xgen_package: $(CORE) tools/xgen_package.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/xgen_package.cpp -o $@ $(LDLIBS)

bin/nanoxgen_xgen_classic_inspect: $(CORE) tools/xgen_classic_inspect.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/xgen_classic_inspect.cpp -o $@ $(LDLIBS)

bin/nanoxgen_xpd_inspect: $(CORE) tools/xpd_inspect.cpp $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tools/xpd_inspect.cpp -o $@ $(LDLIBS)

test: bin/nanoxgen_tests bin/nanoxgen_package_tests bin/nanoxgen_classic_tests bin/nanoxgen_expression_tests
	./bin/nanoxgen_tests
	./bin/nanoxgen_package_tests
	./bin/nanoxgen_classic_tests
	./bin/nanoxgen_expression_tests

fast-math-check:
	./scripts/run_fast_math_comparison.sh

clean:
	rm -f bin/nanoxgen_demo bin/nanoxgen_tests bin/nanoxgen_package_tests bin/nanoxgen_classic_tests bin/nanoxgen_expression_tests bin/nanoxgen_benchmark bin/nanoxgen_cache_benchmark bin/nanoxgen_precision bin/nanoxgen_xgen_inspect bin/nanoxgen_xgen_process bin/nanoxgen_xgen_cache bin/nanoxgen_xgen_read_benchmark bin/nanoxgen_xgen_package bin/nanoxgen_xgen_classic_inspect bin/nanoxgen_xpd_inspect
