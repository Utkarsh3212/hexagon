CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic
THREAD_FLAGS := -pthread

DATA_STRUCTURE_SOURCES := hashtable.cpp avl.cpp zset.cpp heap.cpp thread_pool.cpp
SERVER_SOURCES := server.cpp $(DATA_STRUCTURE_SOURCES)

.PHONY: all test unit-test integration-test clean

all: server client

server: $(SERVER_SOURCES) common.h hashtable.h avl.h zset.h heap.h list.h thread_pool.h
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) $(SERVER_SOURCES) -o $@

client: client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o $@

build/test_data_structures: tests/test_data_structures.cpp $(DATA_STRUCTURE_SOURCES) common.h hashtable.h avl.h zset.h heap.h thread_pool.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) -I. tests/test_data_structures.cpp $(DATA_STRUCTURE_SOURCES) -o $@

unit-test: build/test_data_structures
	./build/test_data_structures

integration-test: server
	python3 tests/test_server.py ./server

test: unit-test integration-test

clean:
	rm -rf build server client
