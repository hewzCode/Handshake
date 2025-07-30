#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <algorithm>

// ---------- portable 64-bit host<->network (big-endian) helpers ----------
#if defined(__APPLE__)
  #include <libkern/OSByteOrder.h>
  static inline uint64_t host_to_be64(uint64_t x) { return OSSwapHostToBigInt64(x); }
  static inline uint64_t be64_to_host(uint64_t x) { return OSSwapBigToHostInt64(x); }
#else
  static inline uint64_t host_to_be64(uint64_t x) {
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      return ( (uint64_t)htonl((uint32_t)(x >> 32)) ) |
             ( ((uint64_t)htonl((uint32_t)(x & 0xFFFFFFFF))) << 32 );
  #else
      return x;
  #endif
  }
  static inline uint64_t be64_to_host(uint64_t x) { return host_to_be64(x); }
#endif
// ------------------------------------------------------------------------

static void die(const char* msg) { perror(msg); std::exit(1); }

static void send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n < 0) { if (errno == EINTR) continue; die("send"); }
        p += n; len -= n;
    }
}

static void recv_all(int fd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, p + got, len - got, 0);
        if (n == 0) { std::cerr << "[client] connection closed by server\n"; std::exit(1); }
        if (n < 0) { if (errno == EINTR) continue; die("recv"); }
        got += n;
    }
}

// length-prefixed strings (uint32 length + bytes)
static void send_string(int fd, const std::string& s) {
    uint32_t n = htonl(static_cast<uint32_t>(s.size()));
    send_all(fd, &n, sizeof(n));
    if (!s.empty()) send_all(fd, s.data(), s.size());
}
static std::string recv_string(int fd) {
    uint32_t n = 0; recv_all(fd, &n, sizeof(n)); n = ntohl(n);
    std::string s(n, '\0'); if (n) recv_all(fd, &s[0], n); return s;
}

static std::string peer_to_string(int fd) {
    sockaddr_storage ss{}; socklen_t slen = sizeof(ss);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &slen) < 0) return "?:?";
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    if (getnameinfo(reinterpret_cast<sockaddr*>(&ss), slen, host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) return "?:?";
    return std::string(host) + ":" + serv;
}

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false); std::cin.tie(nullptr);
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <server-host> <port> \"<client name>\"\n";
        return 1;
    }
    std::string host = argv[1];
    int port = std::atoi(argv[2]);
    std::string clientName = argv[3];
    if (port <= 5000) { std::cerr << "Error: port must be > 5000\n"; return 1; }

    std::signal(SIGPIPE, SIG_IGN);

    // resolve server
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) { std::cerr << "getaddrinfo: " << gai_strerror(rc) << "\n"; return 1; }

    int sockfd = ::socket(AF_INET, SOCK_STREAM, 0); if (sockfd < 0) die("socket");

    sockaddr_in srv{}; srv.sin_family = AF_INET;
    srv.sin_port = htons(static_cast<uint16_t>(port));
    sockaddr_in* got = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    srv.sin_addr = got->sin_addr; freeaddrinfo(res);

    if (connect(sockfd, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) < 0) die("connect");
    std::cout << "[client] connected to " << peer_to_string(sockfd) << "\n";

    // handshake
    send_string(sockfd, clientName);
    send_string(sockfd, std::string("Query file name"));

    // receive server name, file name, size (uint64 big-endian)
    std::string serverName = recv_string(sockfd);
    std::string fileName   = recv_string(sockfd);
    uint64_t netSize = 0; recv_all(sockfd, &netSize, sizeof(netSize));
    uint64_t fileSize = be64_to_host(netSize);

    std::cout << "[client] Client name : " << clientName << "\n";
    std::cout << "[client] Server name : " << serverName << "\n";
    std::cout << "[client] File name   : " << fileName << "\n";
    std::cout << "[client] File size   : " << fileSize << " bytes\n";

    // tell server to start
    send_string(sockfd, std::string("Start"));

    // receive: '1'+up to 100 bytes ... until '0','0'
    const size_t CHUNK = 100;
    uint64_t received = 0;
    while (true) {
        char ctrl = 0; recv_all(sockfd, &ctrl, 1);
        if (ctrl == '0') { char second = 0; recv_all(sockfd, &second, 1);
            std::cout << "[client] received termination pair 0,0 — closing.\n"; break; }
        if (ctrl != '1') { std::cerr << "[client] protocol error: unexpected control byte "
                                     << int(ctrl) << "\n"; std::exit(1); }
        size_t want = static_cast<size_t>(std::min<uint64_t>(CHUNK, fileSize - received));
        if (want == 0) continue; // wait for closing 0,0
        std::string buf(want, '\0'); recv_all(sockfd, &buf[0], want);
        std::cout << buf; received += want;
    }

    ::close(sockfd);
    return 0;
}
