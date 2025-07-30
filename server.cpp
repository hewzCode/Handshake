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
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

// enumerate local interfaces (for printing the actual IP)
#include <ifaddrs.h>
#include <net/if.h>

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
        if (n == 0) { std::cerr << "[server] peer closed unexpectedly\n"; std::exit(1); }
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

// choose a non-loopback, UP IPv4 to display
static std::string detect_local_ip() {
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1 || !ifaddr) return "127.0.0.1";
    std::string best;
    for (ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        std::string name = ifa->ifa_name ? ifa->ifa_name : "";
        if (name.rfind("docker", 0) == 0 || name.rfind("br-", 0) == 0 || name.rfind("veth", 0) == 0)
            continue;
        char host[NI_MAXHOST] = {0};
        if (getnameinfo(ifa->ifa_addr, sizeof(sockaddr_in), host, sizeof(host), nullptr, 0,
                        NI_NUMERICHOST) == 0) { best = host; break; }
    }
    freeifaddrs(ifaddr);
    if (best.empty()) best = "127.0.0.1";
    return best;
}

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false); std::cin.tie(nullptr);
    if (argc != 4) { std::cerr << "Usage: " << argv[0] << " \"<server name>\" <file> <port>\n"; return 1; }

    std::string serverName = argv[1];
    std::string filePath   = argv[2];
    int port               = std::atoi(argv[3]);
    if (port <= 5000) { std::cerr << "Error: port must be > 5000\n"; return 1; }

    // read file
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) { std::cerr << "Error: cannot open file " << filePath << "\n"; return 1; }
    std::vector<char> fileData((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    uint64_t fileSize = fileData.size();

    std::signal(SIGPIPE, SIG_IGN);

    int listenfd = ::socket(AF_INET, SOCK_STREAM, 0); if (listenfd < 0) die("socket");
    int opt = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) die("setsockopt");

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listenfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) die("bind");
    if (listen(listenfd, 8) < 0) die("listen");

    std::string local_ip = detect_local_ip();
    std::cout << "[server] listening on " << local_ip << ":" << port
              << "  file=\"" << filePath << "\"  size=" << fileSize << " bytes\n";

    while (true) {
        sockaddr_in cli{}; socklen_t clen = sizeof(cli);
        int cfd = accept(listenfd, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (cfd < 0) { if (errno == EINTR) continue; die("accept"); }

        std::cout << "[server] accepted from " << peer_to_string(cfd) << "\n";

        // handshake
        std::string clientName = recv_string(cfd);
        std::string query      = recv_string(cfd); (void)query;
        std::cout << "[server] client says: " << clientName << "\n";
        std::cout << "[server] server name: " << serverName << "\n";

        // respond with server name, file name, and size (uint64 big-endian)
        send_string(cfd, serverName);
        send_string(cfd, filePath);
        uint64_t netSize = host_to_be64(fileSize);
        send_all(cfd, &netSize, sizeof(netSize));

        // wait for "Start"
        std::string start = recv_string(cfd); (void)start;

        // send file in 100B chunks: '1'+data ... then '0','0'
        const size_t CHUNK = 100;
        uint64_t sent = 0;
        while (sent < fileSize) {
            char cont = '1'; send_all(cfd, &cont, 1);
            size_t n = static_cast<size_t>(std::min<uint64_t>(CHUNK, fileSize - sent));
            send_all(cfd, fileData.data() + sent, n); sent += n;
        }
        char z = '0'; send_all(cfd, &z, 1); send_all(cfd, &z, 1);
        std::cout << "[server] done sending; closing connection.\n";
        ::close(cfd);
        std::cout << "[server] waiting for next client...\n";
    }
    ::close(listenfd);
    return 0;
}
