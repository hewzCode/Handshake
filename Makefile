# Makefile for C++17 TCP client/server (UTD dcxx machines)
CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic

SERVER_EXE := server
CLIENT_EXE := client
SERVER_SRC := server.cpp
CLIENT_SRC := client.cpp

.PHONY: all clean rebuild

all: $(SERVER_EXE) $(CLIENT_EXE)

$(SERVER_EXE): $(SERVER_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(CLIENT_EXE): $(CLIENT_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -f $(SERVER_EXE) $(CLIENT_EXE)

rebuild: clean all
