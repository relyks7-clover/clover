CXX=g++
CXXFLAGS=-std=c++17 -O2 -Wall -Wextra
LDFLAGS=-lssl -lcrypto
# every tools/*.cpp is auto-discovered; drop a file in tools/ to add a tool,
# delete it to remove one. no edits to main.cpp needed.
TOOL_SRCS=$(wildcard tools/*.cpp)
SRCS=main.cpp tool_registry.cpp $(TOOL_SRCS)

all: a.out

a.out: $(SRCS) tool.h
	$(CXX) $(CXXFLAGS) -o a.out $(SRCS) $(LDFLAGS)

clean:
	rm -f a.out

.PHONY: all clean
