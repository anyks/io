#include "io/net.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {
static io::socket_t make_listen(uint16_t &out_port) {
  io::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(fd, (sockaddr *)&addr, sizeof(addr));
  ::listen(fd, 256);
  socklen_t alen = sizeof(addr);
  ::getsockname(fd, (sockaddr *)&addr, &alen);
  out_port = ntohs(addr.sin_port);
  return fd;
}
}

// This test tries to induce EAGAIN on client send by not reading on the server initially,
// then starts reading and verifies the engine drains the queued data correctly.
TEST(NetEagain, PartialWriteThenDrain) {
  using namespace io;
  std::unique_ptr<INetEngine> engine(create_engine());
  ASSERT_NE(engine, nullptr);

  // Prepare server listen socket
  uint16_t port = 0;
  io::socket_t listen_fd = make_listen(port);

  // Create client socket early so we can distinguish its fd in on_accept
  io::socket_t cfd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(cfd, io::kInvalidSocket);

  std::atomic<io::socket_t> srv_client{io::kInvalidSocket};
  std::atomic<bool> accepted{false};
  std::atomic<size_t> server_bytes{0};
  std::atomic<size_t> client_written_callbacks{0};

  io::NetCallbacks cbs{};
  cbs.on_accept = [&](io::socket_t fd) {
    // on_accept fires both for server-accepted sockets and for client connect completion.
    // We only want the server-side accepted socket here.
    if (fd != listen_fd && fd != cfd) {
      srv_client.store(fd, std::memory_order_release);
      accepted.store(true, std::memory_order_release);
    }
  };
  cbs.on_write = [&](io::socket_t, size_t n) { client_written_callbacks += n; };

  ASSERT_TRUE(engine->init(cbs));
  ASSERT_TRUE(engine->accept(listen_fd, true, 0));

  // Client connects
  static thread_local char client_buf[1024];
  ASSERT_TRUE(engine->add_socket(cfd, client_buf, sizeof(client_buf), [&](int, char*, size_t){}));
  ASSERT_TRUE(engine->connect(cfd, "127.0.0.1", port, true));

  // Wait for accept
  auto t0 = std::chrono::steady_clock::now();
  while (!accepted.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() - t0 > 2s) {
      FAIL() << "timeout waiting for accept";
      break;
    }
    if (!engine->loop_once(2)) std::this_thread::sleep_for(2ms);
  }

  // Queue a large payload on the client to likely saturate send buffers
  const size_t total = 16 * 1024 * 1024; // 16 MB
  std::string payload(total, 'X');
  ASSERT_TRUE(engine->write(cfd, payload.data(), payload.size()));

  // Give the loop some time to push until EAGAIN; server is not yet reading
  {
    auto until = std::chrono::steady_clock::now() + 50ms;
    while (std::chrono::steady_clock::now() < until) {
      if (!engine->loop_once(2)) std::this_thread::sleep_for(2ms);
    }
  }

  // Now start reading on the server and count bytes
  io::socket_t sfd = srv_client.load(std::memory_order_acquire);
  static thread_local char srv_buf[4096];
  ASSERT_NE(sfd, io::kInvalidSocket);
  ASSERT_TRUE(engine->add_socket(sfd, srv_buf, sizeof(srv_buf), [&](io::socket_t, char* b, size_t n) {
    (void)b;
    server_bytes += n;
  }));

  // Wait for all bytes to arrive or timeout
  auto t1 = std::chrono::steady_clock::now();
  while (server_bytes.load() < total) {
    if (std::chrono::steady_clock::now() - t1 > 3s) break;
    if (!engine->loop_once(2)) std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(server_bytes.load(), total);
  // Ensure the client reported at least some physical writes
  EXPECT_GT(client_written_callbacks.load(), 0u);

  ::close(listen_fd);
  engine->destroy();
}
