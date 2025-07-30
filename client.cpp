// client.cpp – tcp file receiver
// usage: ./client <server host/ip> <port> "<client name>"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

// ------------- portable 64‑bit byte‑order helpers ------------------
#if defined(__APPLE__)
  #include <libkern/OSByteOrder.h>
  static inline uint64_t host_to_be64(uint64_t x) { return OSSwapHostToBigInt64(x); }
  static inline uint64_t be64_to_host(uint64_t x) { return OSSwapBigToHostInt64(x); }
#else
  static inline uint64_t host_to_be64(uint64_t x) {
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      return (uint64_t)htonl(uint32_t(x >> 32)) |
             ((uint64_t)htonl(uint32_t(x & 0xffffffff)) << 32);
  #else
      return x;
  #endif
  }
  static inline uint64_t be64_to_host(uint64_t x) { return host_to_be64(x); }
#endif
// ------------------------------------------------------------------

static void die(const char* msg) { perror(msg); std::exit(1); }

static void send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (len) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n < 0) { if (errno == EINTR) continue; die("send"); }
        p += n; len -= n;
    }
}

static void recv_all(int fd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len) {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n == 0) { std::cerr << "[client] server closed early\n"; std::exit(1); }
        if (n < 0) { if (errno == EINTR) continue; die("recv"); }
        p += n; len -= n;
    }
}

// small helpers for length‑prefixed strings
static void send_str(int fd, const std::string& s) {
    uint32_t n = htonl(static_cast<uint32_t>(s.size()));
    send_all(fd, &n, 4);
    if (!s.empty()) send_all(fd, s.data(), s.size());
}
static std::string recv_str(int fd) {
    uint32_t n = 0; recv_all(fd, &n, 4); n = ntohl(n);
    std::string s(n, '\0');
    if (n) recv_all(fd, &s[0], n);
    return s;
}

// pretty print peer address
static std::string peer_to_string(int fd) {
    sockaddr_storage ss = {};
    socklen_t slen = sizeof(ss);
    if (getpeername(fd, (sockaddr*)&ss, &slen) < 0) return "?";
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    if (getnameinfo((sockaddr*)&ss, slen, host, sizeof(host),
                    serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return "?";
    return std::string(host) + ":" + serv;
}

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);
    std::cout.setf(std::ios::unitbuf);
    std::cin.tie(nullptr);

    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " <host> <port> \"<client name>\"\n";
        return 1;
    }
    std::string host = argv[1];
    int port         = std::atoi(argv[2]);
    std::string name = argv[3];
    if (port <= 5000) { std::cerr << "error: port must be > 5000\n"; return 1; }

    std::signal(SIGPIPE, SIG_IGN);

    // resolve host
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) { std::cerr << "getaddrinfo: " << gai_strerror(rc) << "\n"; return 1; }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");

    sockaddr_in srv = {};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(static_cast<uint16_t>(port));
    srv.sin_addr   = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    if (connect(fd, (sockaddr*)&srv, sizeof(srv)) < 0) die("connect");

    std::cout << "[client] connected to " << peer_to_string(fd) << "\n";

    // handshake 1 – send name + dummy query
    send_str(fd, name);
    send_str(fd, "Query file name");

    // handshake 2 – receive server info
    std::string server_name = recv_str(fd);
    std::string file_name   = recv_str(fd);
    uint64_t netsize = 0; recv_all(fd, &netsize, 8);
    uint64_t file_size = be64_to_host(netsize);

    std::cout << "[client] client : " << name << "\n";
    std::cout << "[client] server : " << server_name << "\n";
    std::cout << "[client] file   : " << file_name << " (" << file_size << " bytes)\n";

    // tell server we’re ready
    send_str(fd, "Start");

    // receive loop – '1'+chunk, then '0','0'
    const size_t CHUNK = 100;
    uint64_t recvd = 0;
    while (true) {
        char flag = 0; recv_all(fd, &flag, 1);
        if (flag == '0') {
            char second = 0; recv_all(fd, &second, 1);
            std::cout << "\n[client] done – received termination pair\n";
            break;
        }
        if (flag != '1') { std::cerr << "[client] protocol error\n"; std::exit(1); }
        size_t want = std::min<uint64_t>(CHUNK, file_size - recvd);
        std::string buf(want, '\0'); recv_all(fd, &buf[0], want);
        std::cout << buf; recvd += want;
    }

    ::close(fd);
}
