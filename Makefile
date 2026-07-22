CXX ?= g++
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS ?= -Iinclude

CORE := src/asset.cpp

.PHONY: all test clean

all: bin/nanoxgen_demo bin/nanoxgen_tests bin/nanoxgen_benchmark

bin/nanoxgen_demo: $(CORE) tools/demo.cpp
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

bin/nanoxgen_tests: $(CORE) tests/test_main.cpp
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

bin/nanoxgen_benchmark: $(CORE) tools/benchmark.cpp
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: bin/nanoxgen_tests
	./bin/nanoxgen_tests

clean:
	rm -f bin/nanoxgen_demo bin/nanoxgen_tests bin/nanoxgen_benchmark
