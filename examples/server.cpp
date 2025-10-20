// Echo server example: accept clients, read length-prefixed frames and echo them back
#include "io/net.hpp"
#include <iostream>
#include <unordered_map>
#include <vector>
#include <cstring>
#if defined(_WIN32) || defined(_WIN64)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif
#include <cstdint>

static inline uint32_t be32_read(const char* in) { uint32_t n; std::memcpy(&n, in, 4); return ntohl(n); }
static inline void be32_write(uint32_t v, char* out) { uint32_t n = htonl(v); std::memcpy(out, &n, 4); }

int main(){
  using namespace io;

#if defined(_WIN32) || defined(_WIN64)
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) { std::cerr << "WSAStartup failed" << std::endl; return 1; }
#endif

  auto* engine = create_engine();
  if (!engine) { std::cerr << "No engine available for this platform" << std::endl; return 1; }

  // Per-connection read buffers and partial frame storage
  std::unordered_map<socket_t, std::vector<char>> accum;
  std::unordered_map<socket_t, std::vector<char>> readbufs;

  NetCallbacks cbs{};
  cbs.on_accept = [&](socket_t s){
    // Register a read buffer for this client socket so we can receive data
    std::vector<char> buf; buf.resize(8192);
    readbufs[s] = buf; // store to keep memory alive
    engine->add_socket(s, readbufs[s].data(), readbufs[s].size(), [&](socket_t fd, char* b, size_t n){
      auto &acc = accum[fd]; acc.insert(acc.end(), b, b + n);
      size_t off = 0;
      while (true) {
        if (acc.size() - off < 4) break;
        uint32_t len = be32_read(&acc[off]);
        if (acc.size() - off < 4u + len) break;
        // echo the full frame back (length + payload)
        engine->write(fd, &acc[off], 4u + len);
        off += (4u + len);
      }
      if (off > 0) acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(off));
    });
    std::cout << "accepted: 0x" << std::hex << (static_cast<std::uintptr_t>(s)) << std::dec << std::endl;
  };
  cbs.on_close = [&](socket_t s){ std::cout << "closed: 0x" << std::hex << (static_cast<std::uintptr_t>(s)) << std::dec << std::endl; accum.erase(s); readbufs.erase(s); };

  if (!engine->init(cbs)) { std::cerr << "init failed" << std::endl; return 1; }

  socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd == kInvalidSocket) { std::cerr << "socket() failed" << std::endl; return 1; }
  sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(8080); addr.sin_addr.s_addr = INADDR_ANY;
  int opt=1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr))<0) { perror("bind"); return 1; }
  if (listen(listen_fd, 64)<0) { perror("listen"); return 1; }

  if (!engine->accept(listen_fd, /*async*/true, /*max*/0)) {
    std::cerr << "accept register failed" << std::endl;
  }

  std::cout << "Echo server up on 0.0.0.0:8080 (press Ctrl+C to quit)" << std::endl;
  // Keep process alive
  for(;;) {
#if defined(_WIN32) || defined(_WIN64)
    ::Sleep(1000);
#else
    ::sleep(1);
#endif
  }

#if defined(_WIN32) || defined(_WIN64)
  WSACleanup();
#endif
}
