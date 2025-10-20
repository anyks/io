#include "io/net.hpp"
#if defined(IO_ENGINE_DEVPOLL)
#include <sys/devpoll.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <atomic>
#include <cstdio>

#ifndef NDEBUG
#  define IO_LOG_ERR(fmt, ...) std::fprintf(stderr, "[io/devpoll][ERR] " fmt "\n", ##__VA_ARGS__)
#  define IO_LOG_DBG(fmt, ...) std::fprintf(stderr, "[io/devpoll][DBG] " fmt "\n", ##__VA_ARGS__)
#else
#  define IO_LOG_ERR(fmt, ...) ((void)0)
#  define IO_LOG_DBG(fmt, ...) ((void)0)
#endif

namespace io {

class DevPollEngine : public INetEngine {
public:
  DevPollEngine() = default; ~DevPollEngine() override { destroy(); }
  bool init(const NetCallbacks& cbs) override {
    cbs_ = cbs; dpfd_ = ::open("/dev/poll", O_RDWR); if (dpfd_ < 0) { IO_LOG_ERR("open(/dev/poll) failed errno=%d (%s)", errno, std::strerror(errno)); return false; }
    if (pipe(user_pipe_) == 0) {
      int f0 = fcntl(user_pipe_[0], F_GETFL, 0); fcntl(user_pipe_[0], F_SETFL, f0 | O_NONBLOCK);
      int f1 = fcntl(user_pipe_[1], F_GETFL, 0); fcntl(user_pipe_[1], F_SETFL, f1 | O_NONBLOCK);
      add_poll(user_pipe_[0], POLLIN);
    }
    running_ = true; loop_ = std::thread(&DevPollEngine::event_loop, this); return true;
  }
  void destroy() override {
    if (dpfd_ != -1) { running_ = false; if (user_pipe_[1] != -1) { uint32_t v=0; (void)::write(user_pipe_[1], &v, sizeof(v)); }
      if (loop_.joinable()) loop_.join(); if (user_pipe_[0] != -1) ::close(user_pipe_[0]); if (user_pipe_[1] != -1) ::close(user_pipe_[1]); ::close(dpfd_); dpfd_=-1; }
  }
  bool add_socket(socket_t fd, char* buffer, size_t buffer_size, ReadCallback cb) override {
    int flags = fcntl(fd, F_GETFL, 0); if (flags < 0) flags = 0; fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    { std::lock_guard<std::mutex> lk(mtx_); sockets_[fd] = SockState{buffer, buffer_size, std::move(cb), {}, false, false, false}; }
    add_poll(fd, POLLIN | POLLRDHUP);
    return true;
  }
  bool delete_socket(socket_t fd) override {
    rem_poll(fd);
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = timers_.find(fd); if (it != timers_.end()) { ::close(it->second); timers_.erase(it); }
    sockets_.erase(fd); timeouts_ms_.erase(fd);
    return true;
  }
  bool connect(socket_t fd, const char* host, uint16_t port, bool async) override {
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port); if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) return false;
    int flags = fcntl(fd, F_GETFL, 0); if (flags < 0) flags = 0; if (!async) { if (flags & O_NONBLOCK) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK); }
    int r = ::connect(fd, (sockaddr*)&addr, sizeof(addr)); int err = (r==0)?0:errno; int cur = fcntl(fd, F_GETFL, 0); if (cur<0) cur=0; fcntl(fd, F_SETFL, cur | O_NONBLOCK);
    if (r == 0) { add_poll(fd, POLLIN | POLLRDHUP); if (cbs_.on_accept) cbs_.on_accept(fd); return true; }
    if (async && (err == EINPROGRESS)) { add_poll(fd, POLLOUT | POLLIN | POLLRDHUP); std::lock_guard<std::mutex> lk(mtx_); sockets_[fd].connecting = true; return true; }
    IO_LOG_ERR("connect(fd=%d) failed, res=%d, errno=%d (%s)", (int)fd, r, err, std::strerror(err));
    return false;
  }
  bool disconnect(socket_t fd) override { if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); IO_LOG_DBG("close: fd=%d", (int)fd); if (cbs_.on_close) cbs_.on_close(fd);} return true; }
  bool accept(socket_t listen_socket, bool async, uint32_t max_connections) override {
    if (async) { int flags = fcntl(listen_socket, F_GETFL, 0); fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK); }
    max_conn_ = max_connections; { std::lock_guard<std::mutex> lk(mtx_); listeners_.insert(listen_socket); }
    add_poll(listen_socket, POLLIN);
    return true;
  }
  bool write(socket_t fd, const char* data, size_t data_size) override {
    std::lock_guard<std::mutex> lk(mtx_); auto it = sockets_.find(fd); if (it == sockets_.end()) return false; auto& st = it->second; st.out_queue.insert(st.out_queue.end(), data, data + data_size);
    if (!st.want_write) { st.want_write = true; add_poll(fd, POLLIN | POLLOUT | POLLRDHUP); }
    return true;
  }
  bool pause_read(socket_t socket) override {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sockets_.find(socket); if (it == sockets_.end()) return false;
    it->second.paused = true;
    short mask = POLLRDHUP; if (it->second.want_write) mask |= POLLOUT;
    add_poll(socket, mask);
    return true;
  }
  bool resume_read(socket_t socket) override {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sockets_.find(socket); if (it == sockets_.end()) return false;
    it->second.paused = false;
    short mask = POLLIN | POLLRDHUP; if (it->second.want_write) mask |= POLLOUT;
    add_poll(socket, mask);
    return true;
  }
  bool post(uint32_t val) override { if (user_pipe_[1] == -1) return false; ssize_t n = ::write(user_pipe_[1], &val, sizeof(val)); return n == (ssize_t)sizeof(val); }
  bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override {
    // /dev/poll не умеет таймеры; эмулируем через pipe и отдельный поток, как и в event ports выше
    if (timeout_ms == 0) {
      std::lock_guard<std::mutex> lk(mtx_); auto it = timers_.find(socket); if (it != timers_.end()) { rem_poll(it->second); ::close(it->second); timers_.erase(it); } timeouts_ms_.erase(socket); return true;
    }
    {
      std::lock_guard<std::mutex> lk(mtx_); if (sockets_.find(socket) == sockets_.end()) return false;
    }
    int pfd[2]{-1,-1}; if (pipe(pfd) != 0) return false; int f0 = fcntl(pfd[0], F_GETFL, 0); fcntl(pfd[0], F_SETFL, f0 | O_NONBLOCK);
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = timers_.find(socket); if (it != timers_.end()) { rem_poll(it->second); ::close(it->second); timers_.erase(it); }
      timers_[socket] = pfd[0]; timeouts_ms_[socket] = timeout_ms; timer_targets_[pfd[0]] = socket; add_poll(pfd[0], POLLIN);
    }
    std::thread([this, pd=pfd[1], timeout_ms]() {
      ::usleep(timeout_ms * 1000);
      uint64_t one = 1; (void)::write(pd, &one, sizeof(one)); ::close(pd);
    }).detach();
    return true;
  }
private:
  struct SockState { char* buf{nullptr}; size_t buf_size{0}; ReadCallback read_cb; std::vector<char> out_queue; bool want_write{false}; bool connecting{false}; bool paused{false}; };
  int dpfd_{-1}; NetCallbacks cbs_{}; std::thread loop_{}; std::atomic<bool> running_{false}; int user_pipe_[2]{-1,-1};
  std::mutex mtx_; std::unordered_map<socket_t, SockState> sockets_; std::unordered_set<socket_t> listeners_;
  std::unordered_map<socket_t,uint32_t> timeouts_ms_; std::unordered_map<socket_t,int> timers_; std::unordered_map<int,socket_t> timer_targets_;
  std::atomic<uint32_t> max_conn_{0}; std::atomic<uint32_t> cur_conn_{0};

  void add_poll(int fd, short events) {
    struct pollfd p{}; p.fd = fd; p.events = events;
    ssize_t wr = ::write(dpfd_, &p, sizeof(p));
    if (wr != (ssize_t)sizeof(p)) {
      IO_LOG_ERR("write(/dev/poll add fd=%d events=0x%x) failed errno=%d (%s)", fd, (unsigned)events, errno, std::strerror(errno));
    }
  }
  void rem_poll(int fd) {
    struct pollfd p{}; p.fd = fd; p.events = 0; p.revents = 0;
    ssize_t wr = ::write(dpfd_, &p, sizeof(p));
    if (wr != (ssize_t)sizeof(p)) {
      IO_LOG_ERR("write(/dev/poll del fd=%d) failed errno=%d (%s)", fd, errno, std::strerror(errno));
    }
  }

  void event_loop() {
    while (running_) {
      dvpoll dv{}; pollfd evs[128]; dv.dp_fds = evs; dv.dp_nfds = 128; dv.dp_timeout = 1000; int n = ::ioctl(dpfd_, DP_POLL, &dv);
      if (n < 0) { if (errno == EINTR) continue; IO_LOG_ERR("ioctl(DP_POLL) failed errno=%d (%s)", errno, std::strerror(errno)); continue; }
      if (n == 0) continue;
      for (int i=0;i<n;++i) {
        int fd = evs[i].fd; short re = evs[i].revents;
        if (fd == user_pipe_[0] && (re & POLLIN)) { uint32_t v; while (::read(user_pipe_[0], &v, sizeof(v)) == sizeof(v)) { if (cbs_.on_user) cbs_.on_user(v);} continue; }
        if (timer_targets_.find(fd) != timer_targets_.end() && (re & POLLIN)) { uint64_t one; while (::read(fd,&one,sizeof(one))==sizeof(one)){} socket_t sfd=-1; { std::lock_guard<std::mutex> lk(mtx_); sfd = timer_targets_[fd]; timer_targets_.erase(fd); for(auto it=timers_.begin(); it!=timers_.end(); ++it){ if(it->second==fd){ timers_.erase(it); break; } } timeouts_ms_.erase(sfd);} if (sfd!=-1){ if (cbs_.on_close) cbs_.on_close(sfd); ::close(sfd);} rem_poll(fd); ::close(fd); continue; }
        bool is_listener=false; { std::lock_guard<std::mutex> lk(mtx_); is_listener = listeners_.find(fd) != listeners_.end(); }
        if (is_listener && (re & POLLIN)) { while (true) { sockaddr_storage ss{}; socklen_t slen=sizeof(ss); socket_t client=::accept(fd,(sockaddr*)&ss,&slen); if (client<0){ if(errno==EAGAIN||errno==EWOULDBLOCK) break; else { IO_LOG_ERR("accept(listen fd=%d) failed errno=%d (%s)", fd, errno, std::strerror(errno)); break; } } if (max_conn_>0 && cur_conn_.load()>=max_conn_){::close(client); continue;} int fl=fcntl(client,F_GETFL,0); if(fl<0) fl=0; fcntl(client,F_SETFL,fl|O_NONBLOCK); cur_conn_++; IO_LOG_DBG("accept: client fd=%d", (int)client); if (cbs_.on_accept) cbs_.on_accept(client);} continue; }
        if (re & POLLOUT) { std::unique_lock<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if (it!=sockets_.end() && it->second.connecting) { int err=0; socklen_t len=sizeof(err); int gs = ::getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len); if(gs<0){ err=errno; IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", fd, err, std::strerror(err)); } else { IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", fd, err, std::strerror(err)); } it->second.connecting=false; if (err==0) { lk.unlock(); IO_LOG_DBG("connect: established fd=%d", fd); if (cbs_.on_accept) cbs_.on_accept(fd);} else { lk.unlock(); if (cbs_.on_close) cbs_.on_close(fd); ::close(fd); std::lock_guard<std::mutex> lk2(mtx_); sockets_.erase(fd); continue; } } }
        if (re & POLLIN) { std::unique_lock<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if (it==sockets_.end()) { lk.unlock(); continue; } auto st=it->second; bool paused=st.paused; lk.unlock(); if(paused) continue; if(!st.buf||st.buf_size==0||!st.read_cb) continue; ssize_t rn=::recv(fd,st.buf,st.buf_size,0); if(rn>0){ st.read_cb(fd,st.buf,(size_t)rn);} else if(rn==0 || (re & (POLLHUP|POLLERR))){ if(cbs_.on_close) cbs_.on_close(fd); std::lock_guard<std::mutex> lk2(mtx_); sockets_.erase(fd); if(cur_conn_>0) cur_conn_--; auto itT=timers_.find(fd); if(itT!=timers_.end()){ int tfd=itT->second; rem_poll(tfd); ::close(tfd); timers_.erase(itT);} timeouts_ms_.erase(fd); } }
        if (re & POLLOUT) { std::unique_lock<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if(it!=sockets_.end() && !it->second.out_queue.empty()) { auto& st=it->second; auto* p=st.out_queue.data(); size_t sz=st.out_queue.size(); lk.unlock(); ssize_t wn=::send(fd,p,sz,0); lk.lock(); if(wn>0){ if(cbs_.on_write) cbs_.on_write(fd,(size_t)wn); st.out_queue.erase(st.out_queue.begin(), st.out_queue.begin()+wn);} if(st.out_queue.empty()){ short mask = POLLRDHUP; if(!st.paused) mask |= POLLIN; add_poll(fd,mask); st.want_write=false; } } }
      }
    }
  }
};

INetEngine* create_engine_devpoll() { return new DevPollEngine(); }

} // namespace io

#endif
