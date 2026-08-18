CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?=
CPPFLAGS += -Iinclude

ifeq ($(shell uname -s),Darwin)
MACOS_SDK := $(shell xcrun --show-sdk-path)
CPPFLAGS += -isysroot $(MACOS_SDK) -isystem $(MACOS_SDK)/usr/include/c++/v1
LDFLAGS += -isysroot $(MACOS_SDK)
endif

BUILD_DIR := build
LIB_OBJECTS := $(BUILD_DIR)/bloom.o $(BUILD_DIR)/sstable.o \
	$(BUILD_DIR)/wal.o $(BUILD_DIR)/recovery.o $(BUILD_DIR)/compaction.o \
	$(BUILD_DIR)/lsm.o
TOOL := $(BUILD_DIR)/lsm_tool
TEST := $(BUILD_DIR)/test_lsm
BENCH := $(BUILD_DIR)/lsm_bench

.PHONY: all test crash-test benchmark bench bench-sweep clean

all: $(TOOL)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

HEADERS := $(wildcard include/lsm/*.h)

$(BUILD_DIR)/%.o: src/%.cpp $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TOOL): src/main.cpp $(LIB_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(TEST): tests/test_lsm.cpp $(LIB_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(BENCH): benchmarks/benchmark_driver.cpp $(LIB_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

test: $(TEST)
	./$(TEST)

crash-test: $(TOOL)
	python3 scripts/crash_torture.py --binary ./$(TOOL) \
		--rounds $${CRASH_ROUNDS:-100}

benchmark: $(TOOL)
	./$(TOOL) benchmark $(BUILD_DIR)/benchmark.db 20000

# Configurable benchmark framework: run every workload with default settings.
bench: $(BENCH)
	mkdir -p benchmarks/results
	./$(BENCH) benchmarks/workloads/write_heavy.conf $(BUILD_DIR)/bench_write.db
	./$(BENCH) benchmarks/workloads/read_heavy.conf $(BUILD_DIR)/bench_read.db
	./$(BENCH) benchmarks/workloads/mixed.conf $(BUILD_DIR)/bench_mixed.db

# Controlled experiments across the memtable / bloom / compaction grid.
bench-sweep: $(BENCH)
	bash scripts/run_experiments.sh

clean:
	rm -rf $(BUILD_DIR)
