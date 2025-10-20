#include <gtest/gtest.h>
#include "io/net.hpp"
#include <atomic>
#include <thread>
#include <future>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std::chrono_literals;

TEST(NetIntegration, ClientServerEchoAndPost) {
  using namespace io;
  std::unique_ptr<INetEngine> engine(create_engine());
  ASSERT_NE(engine, nullptr);

  std::atomic<bool> client_connected{false};
  std::atomic<bool> server_connected{false};
  std::atomic<int> close_count{0};
  auto user_mtx = std::make_shared<std::mutex>();
  auto user_events = std::make_shared<std::vector<uint32_t>>();
  std::promise<void> echo_promise; auto echo_future = echo_promise.get_future();
  std::atomic<bool> echo_set{false};

  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listen_fd, 0);
  int opt=1; ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.sin_port=0; // ephemeral port
  ASSERT_EQ(::bind(listen_fd, (sockaddr*)&addr, sizeof(addr)), 0);
  ASSERT_EQ(::listen(listen_fd, 16), 0);
  // discover port
  socklen_t alen=sizeof(addr); ASSERT_EQ(::getsockname(listen_fd, (sockaddr*)&addr, &alen), 0);
  uint16_t port = ntohs(addr.sin_port);

  // buffers
  static thread_local char client_buf[4096];
  static thread_local char server_buf[4096];

  int server_client_fd = -1;
  int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(client_fd, 0);

  io::NetCallbacks cbs{};
  cbs.on_accept = [&](int fd){
    if (fd == client_fd) {
      client_connected = true;
    } else if (fd != listen_fd) {
      server_connected = true;
      server_client_fd = fd;
      // start reading on server-side accepted socket and echo back
      engine->add_socket(server_client_fd, server_buf, sizeof(server_buf), [&](int s, char* b, size_t n){
        // echo
        engine->write(s, b, n);
      });
    }
  };
  cbs.on_read = [&](int s, char* b, size_t n){
    (void)s;
    // client read callback will notify when got echo
    std::string got(b, b + n);
    if (got == "hello") {
      if (!echo_set.exchange(true)) {
        echo_promise.set_value();
      }
    }
  };
  cbs.on_close = [&](int){ close_count++; };
  cbs.on_user = [user_mtx, user_events](uint32_t v){ std::lock_guard<std::mutex> lk(*user_mtx); user_events->push_back(v); };

  ASSERT_TRUE(engine->init(cbs));

  // accept on server side
  ASSERT_TRUE(engine->accept(listen_fd, true, 8));

  // prepare client side
  ASSERT_TRUE(engine->add_socket(client_fd, client_buf, sizeof(client_buf), cbs.on_read));
  ASSERT_TRUE(engine->connect(client_fd, "127.0.0.1", port, true));

  // wait for both connections
  auto start = std::chrono::steady_clock::now();
  while (!(client_connected.load() && server_connected.load())) {
    if (std::chrono::steady_clock::now() - start > 2s) {
      FAIL() << "Timeout waiting for connections";
      break;
    }
    std::this_thread::sleep_for(10ms);
  }

  // send echo
  const char* msg = "hello";
  ASSERT_TRUE(engine->write(client_fd, msg, std::strlen(msg)));
  ASSERT_EQ(echo_future.wait_for(2s), std::future_status::ready);

  // user events
  for (uint32_t i=1;i<=5;i++) engine->post(i);
  auto start2 = std::chrono::steady_clock::now();
  while (true) {
    size_t sz = 0;
    {
      std::lock_guard<std::mutex> lk(*user_mtx);
      sz = user_events->size();
    }
    if (sz >= 5) break;
    if (std::chrono::steady_clock::now() - start2 > 2s) break;
    std::this_thread::sleep_for(10ms);
  }
  {
    std::lock_guard<std::mutex> lk(*user_mtx);
    ASSERT_EQ(user_events->size(), 5u);
    for (uint32_t i=0;i<5;i++) EXPECT_EQ((*user_events)[i], i+1);
  }

  // cleanup
  engine->disconnect(client_fd);
  if (server_client_fd != -1) engine->disconnect(server_client_fd);
  ::close(listen_fd);
  engine->destroy();
}
