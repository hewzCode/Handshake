// server.cpp – single‑client tcp file sender
// usage: ./server "<server name>" <file> <port>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
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
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// ------------------------------------------------------------------
// little helpers for 64‑bit host/network conversion
// gcc < 5 and clang on macos have no htonll, so we roll our own
// ------------------------------------------------------------------
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
// i/o wrappers
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
        if (n == 0) { std::cerr << "[server] client closed early\n"; std::exit(1); }
        if (n < 0) { if (errno == EINTR) continue; die("recv"); }
        p += n; len -= n;
    }
}

// length‑prefixed string helpers (uint32 len + bytes)
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

// find the first non‑loopback ipv4 so we can show the user
static std::string find_local_ip() {
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1 || !ifaddr) return "127.0.0.1";
    std::string best;
    for (ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;

        std::string name = ifa->ifa_name ? ifa->ifa_name : "";
        if (name.rfind("docker", 0) == 0 || name.rfind("br-", 0) == 0 || name.rfind("veth", 0) == 0)
            continue;

        char ip[NI_MAXHOST] = {0};
        if (getnameinfo(ifa->ifa_addr, sizeof(sockaddr_in), ip, sizeof(ip),
                        nullptr, 0, NI_NUMERICHOST) == 0) {
            best = ip; break;
        }
    }
    freeifaddrs(ifaddr);
    if (best.empty()) best = "127.0.0.1";
    return best;
}

// pretty print a peer address
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
    std::cout.setf(std::ios::unitbuf);   // flush every << automatically
    std::cin.tie(nullptr);

    // quick arg check
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " \"<server name>\" <file> <port>\n";
        return 1;
    }
    std::string server_name = argv[1];
    std::string file_path   = argv[2];
    int port                = std::atoi(argv[3]);
    if (port <= 5000) { std::cerr << "error: port must be > 5000\n"; return 1; }

    // read whole file into memory (fine for our small test files)
    std::ifstream in(file_path.c_str(), std::ios::binary);
    if (!in) { std::cerr << "error: cannot open file " << file_path << "\n"; return 1; }
    std::vector<char> file((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    uint64_t file_size = file.size();

    // ignore sigpipe so a broken client doesn’t crash us
    std::signal(SIGPIPE, SIG_IGN);

    // prepare listening socket
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) die("socket");

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};                // zero all fields
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port   = htons(static_cast<uint16_t>(port));

    if (bind(lfd, (sockaddr*)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(lfd, 8) < 0) die("listen");

    std::string ip = find_local_ip();
    std::cout << "[server] listening on " << ip << ":" << port
              << "  file=\"" << file_path << "\"  size=" << file_size << " bytes\n";

    // simple, single‑client loop (one at a time)
    while (true) {
        sockaddr_in cli = {};
        socklen_t clen = sizeof(cli);
        int cfd = accept(lfd, (sockaddr*)&cli, &clen);
        if (cfd < 0) { if (errno == EINTR) continue; die("accept"); }

        std::cout << "[server] accepted from " << peer_to_string(cfd) << "\n";

        // handshake 1: get client name + query
        std::string client_name = recv_str(cfd);
        std::string query       = recv_str(cfd); (void)query;
        std::cout << "[server] client says: " << client_name << "\n";

        // handshake 2: send server name, file name, file size
        send_str(cfd, server_name);
        send_str(cfd, file_path);
        uint64_t netsize = host_to_be64(file_size);
        send_all(cfd, &netsize, 8);

        // wait for "start" (we ignore content)
        std::string start = recv_str(cfd); (void)start;

        // send file in 100‑byte chunks, each preceded by '1'
        const size_t CHUNK = 100;
        uint64_t sent = 0;
        while (sent < file_size) {
            char flag = '1';
            send_all(cfd, &flag, 1);
            size_t n = std::min<uint64_t>(CHUNK, file_size - sent);
            send_all(cfd, &file[sent], n);
            sent += n;
        }
        // send terminating '0','0'
        char zero = '0';
        send_all(cfd, &zero, 1);
        send_all(cfd, &zero, 1);

        std::cout << "[server] done; closing connection\n";
        ::close(cfd);
    }
}
