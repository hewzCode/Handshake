# simple makefile – builds server and client
# pass CXX=g++ or CXX=clang++ if you like; default is g++
# default standard is c++11 so it compiles on the dcxx boxes.
CXX      ?= g++
CXXSTD   ?= 11         
CXXFLAGS ?= -std=c++$(CXXSTD) -O2 -Wall -Wextra -Wpedantic -Wno-missing-field-initializers

SERVER_EXE := server
CLIENT_EXE := client

.PHONY: all clean rebuild

all: $(SERVER_EXE) $(CLIENT_EXE)

$(SERVER_EXE): server.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

$(CLIENT_EXE): client.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -f $(SERVER_EXE) $(CLIENT_EXE)

rebuild: clean all
