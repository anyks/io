#include "io/net.hpp"
#if defined(IO_ENGINE_EPOLL)
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <sys/timerfd.h>
#include <cstdio>

#ifndef NDEBUG
#  define IO_LOG_ERR(fmt, ...) std::fprintf(stderr, "[io/epoll][ERR] " fmt "\n", ##__VA_ARGS__)
#  define IO_LOG_DBG(fmt, ...) std::fprintf(stderr, "[io/epoll][DBG] " fmt "\n", ##__VA_ARGS__)
#else
#  define IO_LOG_ERR(fmt, ...) ((void)0)
#  define IO_LOG_DBG(fmt, ...) ((void)0)
#endif

namespace io {

class EpollEngine : public INetEngine {
public:
  EpollEngine() = default; ~EpollEngine() override { destroy(); }
  bool init(const NetCallbacks& cbs) override {
    cbs_ = cbs; ep_ = ::epoll_create1(0); if (ep_ < 0) return false;
    if (pipe(user_pipe_) == 0) {
      int f0 = fcntl(user_pipe_[0], F_GETFL, 0); fcntl(user_pipe_[0], F_SETFL, f0 | O_NONBLOCK);
      int f1 = fcntl(user_pipe_[1], F_GETFL, 0); fcntl(user_pipe_[1], F_SETFL, f1 | O_NONBLOCK);
      epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = user_pipe_[0]; ::epoll_ctl(ep_, EPOLL_CTL_ADD, user_pipe_[0], &ev);
    }
    running_ = true; loop_ = std::thread(&EpollEngine::event_loop, this); return true;
  }
  bool set_accept_depth(socket_t listen_socket, uint32_t depth) override { (void)listen_socket; (void)depth; return true; }
  bool set_accept_depth_ex(socket_t listen_socket, uint32_t depth, bool aggressive_cancel) override {
    (void)aggressive_cancel; return set_accept_depth(listen_socket, depth);
  }
  bool set_accept_autotune(socket_t listen_socket, const AcceptAutotuneConfig& cfg) override {
    (void)listen_socket; (void)cfg; return true;
  }
  void destroy() override {
    if (ep_ != -1) { running_ = false; if (user_pipe_[1] != -1) { uint32_t v=0; (void)::write(user_pipe_[1], &v, sizeof(v)); }
      if (loop_.joinable()) loop_.join(); if (user_pipe_[0] != -1) ::close(user_pipe_[0]); if (user_pipe_[1] != -1) ::close(user_pipe_[1]); ::close(ep_); ep_ = -1; }
  }
  bool add_socket(socket_t fd, char* buffer, size_t buffer_size, ReadCallback cb) override {
    int flags = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    { std::lock_guard<std::mutex> lk(mtx_); sockets_[fd] = SockState{buffer, buffer_size, std::move(cb), {}, false, false}; }
    epoll_event ev{}; ev.events = EPOLLIN | EPOLLRDHUP; ev.data.fd = fd; if (::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) != 0) { IO_LOG_ERR("epoll_ctl(ADD fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno)); return false; } return true;
  }
  bool delete_socket(socket_t fd) override {
    if (::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr) != 0) { IO_LOG_ERR("epoll_ctl(DEL fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno)); }
    std::lock_guard<std::mutex> lk(mtx_);
    auto itT = timers_.find(fd);
    if (itT != timers_.end()) {
      int tfd = itT->second; ::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr); ::close(tfd); owner_.erase(tfd); timers_.erase(itT); timeouts_ms_.erase(fd);
    }
    sockets_.erase(fd);
    return true;
  }
  bool connect(socket_t fd, const char* host, uint16_t port, bool async) override {
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port); if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) return false;
    int flags = fcntl(fd, F_GETFL, 0); if (flags < 0) flags = 0; if (!async) { if (flags & O_NONBLOCK) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK); }
    int r = ::connect(fd, (sockaddr*)&addr, sizeof(addr)); int err = (r==0)?0:errno; int cur = fcntl(fd, F_GETFL, 0); if (cur<0) cur=0; fcntl(fd, F_SETFL, cur | O_NONBLOCK);
    if (r == 0) {
      // Успешное соединение установлено сразу. Не трогаем sockets_[fd],
      // чтобы не потерять buffer/callback, заданные через add_socket.
      // Лишь убеждаемся, что сокет подписан на чтение в epoll.
      epoll_event ev{}; ev.events = EPOLLIN | EPOLLRDHUP; ev.data.fd = fd;
      // Попробуем MOD, если ещё не добавлен — fallback на ADD (ошибки игнорируем).
      (void)::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
      (void)::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev);
      if (cbs_.on_accept) cbs_.on_accept(fd);
      return true;
    }
    if (async && err == EINPROGRESS) { { std::lock_guard<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if(it==sockets_.end()){ SockState st; st.connecting=true; sockets_.emplace(fd, std::move(st)); } else { it->second.connecting=true; } }
      epoll_event ev{}; ev.events = EPOLLOUT | EPOLLIN | EPOLLRDHUP; ev.data.fd = fd; ::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev); return true; }
    // immediate failure path (not in-progress)
    IO_LOG_ERR("connect(fd=%d) failed, res=%d, errno=%d (%s)", (int)fd, r, err, std::strerror(err));
    return false;
  }
  bool disconnect(socket_t fd) override { if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); IO_LOG_DBG("close: fd=%d", (int)fd); if (cbs_.on_close) cbs_.on_close(fd);} return true; }
  bool accept(socket_t listen_socket, bool async, uint32_t max_connections) override {
    if (async) { int flags = fcntl(listen_socket, F_GETFL, 0); fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK); }
    max_conn_ = max_connections; { std::lock_guard<std::mutex> lk(mtx_); listeners_.insert(listen_socket); }
    epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = listen_socket; if (::epoll_ctl(ep_, EPOLL_CTL_ADD, listen_socket, &ev) != 0) { IO_LOG_ERR("epoll_ctl(ADD listen fd=%d) failed errno=%d (%s)", (int)listen_socket, errno, std::strerror(errno)); return false; } return true;
  }
  bool write(socket_t fd, const char* data, size_t data_size) override {
    std::lock_guard<std::mutex> lk(mtx_); auto it = sockets_.find(fd); if (it == sockets_.end()) return false; auto& st = it->second;
    st.out_queue.insert(st.out_queue.end(), data, data + data_size);
    if (!st.want_write) { epoll_event ev{}; ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP; ev.data.fd = fd; (void)::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev); st.want_write = true; }
    return true;
  }
  bool post(uint32_t val) override { if (user_pipe_[1] == -1) return false; ssize_t n = ::write(user_pipe_[1], &val, sizeof(val)); return n == (ssize_t)sizeof(val); }
  bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override {
    // Create or update a timerfd for this socket
    if (timeout_ms == 0) {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = timers_.find(socket);
      if (it != timers_.end()) {
        int tfd = it->second; ::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr); ::close(tfd); timers_.erase(it);
      }
      timeouts_ms_.erase(socket);
      return true;
    }
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (sockets_.find(socket) == sockets_.end()) return false;
    }
    int tfd = -1;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = timers_.find(socket);
      if (it == timers_.end()) {
        tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (tfd < 0) return false;
        timers_[socket] = tfd; owner_[tfd] = socket;
        epoll_event tev{}; tev.events = EPOLLIN; tev.data.fd = tfd; ::epoll_ctl(ep_, EPOLL_CTL_ADD, tfd, &tev);
      } else {
        tfd = it->second;
      }
      timeouts_ms_[socket] = timeout_ms;
    }
    itimerspec its{}; its.it_value.tv_sec = timeout_ms / 1000; its.it_value.tv_nsec = (timeout_ms % 1000) * 1000000L;
    // One-shot timer; we'll rearm on read events
    if (::timerfd_settime(tfd, 0, &its, nullptr) != 0) return false;
    return true;
  }

  bool pause_read(socket_t fd) override {
    epoll_event ev{}; ev.data.fd = fd; ev.events = EPOLLRDHUP;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = sockets_.find(fd);
      if (it != sockets_.end() && it->second.want_write) ev.events |= EPOLLOUT;
    }
    if (::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev) != 0) { IO_LOG_ERR("epoll_ctl(MOD pause fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno)); return false; }
    return true;
  }

  bool resume_read(socket_t fd) override {
    epoll_event ev{}; ev.data.fd = fd; ev.events = EPOLLIN | EPOLLRDHUP;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = sockets_.find(fd);
      if (it != sockets_.end() && it->second.want_write) ev.events |= EPOLLOUT;
    }
    if (::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev) != 0) { IO_LOG_ERR("epoll_ctl(MOD resume fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno)); return false; }
    return true;
  }

private:
  struct SockState { char* buf{nullptr}; size_t buf_size{0}; ReadCallback read_cb; std::vector<char> out_queue; bool want_write{false}; bool connecting{false}; };
  int ep_{-1}; NetCallbacks cbs_{}; std::thread loop_{}; std::atomic<bool> running_{false}; int user_pipe_[2]{-1,-1};
  std::mutex mtx_; std::unordered_map<socket_t, SockState> sockets_; std::unordered_set<socket_t> listeners_;
  // timerfd per socket for read idle timeout
  std::unordered_map<socket_t,int> timers_; // socket -> timerfd
  std::unordered_map<int,socket_t> owner_;  // timerfd -> socket
  std::unordered_map<socket_t,uint32_t> timeouts_ms_; // socket -> timeout ms
  std::atomic<uint32_t> max_conn_{0}; std::atomic<uint32_t> cur_conn_{0};

  void event_loop() {
    constexpr int MAXE=64; std::vector<epoll_event> evs(MAXE);
    while (running_) {
      int n = ::epoll_wait(ep_, evs.data(), MAXE, 1000); if (n < 0) { if (errno == EINTR) continue; IO_LOG_ERR("epoll_wait failed errno=%d (%s)", errno, std::strerror(errno)); continue; } if (n == 0) continue;
      for (int i=0;i<n;++i) {
        int fd = evs[i].data.fd; uint32_t events = evs[i].events;
        // timerfd fired?
        if (owner_.find(fd) != owner_.end() && (events & EPOLLIN)) {
          uint64_t exp; while (::read(fd, &exp, sizeof(exp)) == sizeof(exp)) {}
          socket_t sfd = (socket_t)-1; { std::lock_guard<std::mutex> lk(mtx_); auto it=owner_.find(fd); if(it!=owner_.end()) sfd = it->second; }
          if (sfd != -1) { if (cbs_.on_close) cbs_.on_close(sfd); ::close(sfd); std::lock_guard<std::mutex> lk(mtx_); sockets_.erase(sfd); if(cur_conn_>0) cur_conn_--; auto itT=timers_.find(sfd); if(itT!=timers_.end()){ ::epoll_ctl(ep_,EPOLL_CTL_DEL,fd,nullptr); ::close(fd); owner_.erase(fd); timers_.erase(itT);} timeouts_ms_.erase(sfd); }
          continue;
        }
        if ((events & EPOLLIN) && fd == user_pipe_[0]) { uint32_t v; while (::read(user_pipe_[0], &v, sizeof(v)) == sizeof(v)) { if (cbs_.on_user) cbs_.on_user(v);} continue; }
        bool is_listener=false; { std::lock_guard<std::mutex> lk(mtx_); is_listener = listeners_.find(fd)!=listeners_.end(); }
        if (is_listener && (events & EPOLLIN)) { while (true) { sockaddr_storage ss{}; socklen_t slen=sizeof(ss); socket_t client=::accept(fd,(sockaddr*)&ss,&slen); if (client<0){ if(errno==EAGAIN||errno==EWOULDBLOCK) break; else { IO_LOG_ERR("accept(listen fd=%d) failed errno=%d (%s)", fd, errno, std::strerror(errno)); break; } } if (max_conn_>0 && cur_conn_.load()>=max_conn_){::close(client); continue;} int fl=fcntl(client,F_GETFL,0); if(fl<0) fl=0; fcntl(client,F_SETFL,fl|O_NONBLOCK); epoll_event cev{}; cev.events=EPOLLIN|EPOLLRDHUP; cev.data.fd=client; if (::epoll_ctl(ep_,EPOLL_CTL_ADD,client,&cev)!=0){ IO_LOG_ERR("epoll_ctl(ADD client fd=%d) failed errno=%d (%s)", (int)client, errno, std::strerror(errno)); ::close(client); continue; } cur_conn_++; IO_LOG_DBG("accept: client fd=%d", (int)client); if (cbs_.on_accept) cbs_.on_accept(client);} continue; }
        if (events & EPOLLOUT) { std::unique_lock<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if (it!=sockets_.end() && it->second.connecting) { int err=0; socklen_t len=sizeof(err); int gs = ::getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len); if(gs<0){ err=errno; IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", fd, err, std::strerror(err)); } else { IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", fd, err, std::strerror(err)); } it->second.connecting=false; if (err==0) { lk.unlock(); IO_LOG_DBG("connect: established fd=%d", fd); if (cbs_.on_accept) cbs_.on_accept(fd);} else { lk.unlock(); if (cbs_.on_close) cbs_.on_close(fd); ::close(fd); std::lock_guard<std::mutex> lk2(mtx_); sockets_.erase(fd); continue; } } }
        if (events & EPOLLOUT) { std::unique_lock<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if(it!=sockets_.end() && !it->second.out_queue.empty()) { auto& st=it->second; auto* p=st.out_queue.data(); size_t sz=st.out_queue.size(); lk.unlock(); ssize_t wn=::send(fd,p,sz,0); lk.lock(); if(wn<0){ IO_LOG_ERR("send(fd=%d) failed errno=%d (%s)", fd, errno, std::strerror(errno)); } if(wn>0){ if(cbs_.on_write) cbs_.on_write(fd,(size_t)wn); st.out_queue.erase(st.out_queue.begin(), st.out_queue.begin()+wn);} if(st.out_queue.empty()){ epoll_event ev{}; ev.events=EPOLLIN|EPOLLRDHUP; ev.data.fd=fd; if(::epoll_ctl(ep_,EPOLL_CTL_MOD,fd,&ev)!=0){ IO_LOG_ERR("epoll_ctl(MOD fd=%d->IN) failed errno=%d (%s)", fd, errno, std::strerror(errno)); } st.want_write=false; } } }
        if (events & (EPOLLIN|EPOLLHUP|EPOLLRDHUP|EPOLLERR)) { std::unique_lock<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if (it==sockets_.end()) { lk.unlock(); continue; } auto st=it->second; lk.unlock(); if(!st.buf||st.buf_size==0||!st.read_cb) continue; ssize_t rn=::recv(fd,st.buf,st.buf_size,0); if(rn<0){ IO_LOG_ERR("recv(fd=%d) failed errno=%d (%s)", fd, errno, std::strerror(errno)); }
          if(rn>0){ st.read_cb(fd,st.buf,(size_t)rn); // rearm timer if exists
            int tfd=-1; uint32_t ms=0; { std::lock_guard<std::mutex> lk2(mtx_); auto itT=timers_.find(fd); if(itT!=timers_.end()) { tfd = itT->second; ms = timeouts_ms_[fd]; } }
            if (tfd!=-1 && ms>0) { itimerspec its{}; its.it_value.tv_sec = ms/1000; its.it_value.tv_nsec = (ms%1000)*1000000L; if(::timerfd_settime(tfd, 0, &its, nullptr)!=0){ IO_LOG_ERR("timerfd_settime(fd=%d) failed errno=%d (%s)", tfd, errno, std::strerror(errno)); } }
          } else if(rn==0 || (events & (EPOLLHUP|EPOLLRDHUP|EPOLLERR))){ if(cbs_.on_close) cbs_.on_close(fd); std::lock_guard<std::mutex> lk2(mtx_); sockets_.erase(fd); if(cur_conn_>0) cur_conn_--; auto itT=timers_.find(fd); if(itT!=timers_.end()){ int tfd=itT->second; ::epoll_ctl(ep_,EPOLL_CTL_DEL,tfd,nullptr); ::close(tfd); owner_.erase(tfd); timers_.erase(itT);} timeouts_ms_.erase(fd); }
      }
    }
  }
}
};

INetEngine* create_engine_epoll() { return new EpollEngine(); }

}; // namespace io

#endif

