#include "io/net.hpp"
#if defined(IO_ENGINE_EVENTPORTS)
#include <port.h>
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
#include <atomic>
#include <cstdio>
#include <signal.h>
#include <time.h>

#ifndef NDEBUG
#  define IO_LOG_ERR(fmt, ...) std::fprintf(stderr, "[io/eventports][ERR] " fmt "\n", ##__VA_ARGS__)
#  define IO_LOG_DBG(fmt, ...) std::fprintf(stderr, "[io/eventports][DBG] " fmt "\n", ##__VA_ARGS__)
#else
#  define IO_LOG_ERR(fmt, ...) ((void)0)
#  define IO_LOG_DBG(fmt, ...) ((void)0)
#endif

namespace io {

class EventPortsEngine : public INetEngine {
public:
  EventPortsEngine() = default; ~EventPortsEngine() override { destroy(); }
  bool init(const NetCallbacks& cbs) override {
    cbs_ = cbs; port_ = ::port_create(); if (port_ < 0) { IO_LOG_ERR("port_create failed errno=%d (%s)", errno, std::strerror(errno)); return false; }
    // user event pipe
    if (pipe(user_pipe_) == 0) {
      int f0 = fcntl(user_pipe_[0], F_GETFL, 0); fcntl(user_pipe_[0], F_SETFL, f0 | O_NONBLOCK);
      int f1 = fcntl(user_pipe_[1], F_GETFL, 0); fcntl(user_pipe_[1], F_SETFL, f1 | O_NONBLOCK);
      if (::port_associate(port_, PORT_SOURCE_FD, user_pipe_[0], POLLIN, nullptr) != 0) {
        IO_LOG_ERR("port_associate(user_pipe) failed errno=%d (%s)", errno, std::strerror(errno));
      }
    }
    running_ = true; loop_ = std::thread(&EventPortsEngine::event_loop, this); return true;
  }
  void destroy() override {
    if (port_ != -1) { running_ = false; if (user_pipe_[1] != -1) { uint32_t v=0; (void)::write(user_pipe_[1], &v, sizeof(v)); }
      if (loop_.joinable()) loop_.join(); if (user_pipe_[0] != -1) ::close(user_pipe_[0]); if (user_pipe_[1] != -1) ::close(user_pipe_[1]); ::close(port_); port_=-1; }
  }
  bool add_socket(socket_t fd, char* buffer, size_t buffer_size, ReadCallback cb) override {
    int flags = fcntl(fd, F_GETFL, 0); if (flags < 0) flags = 0; fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    { std::lock_guard<std::mutex> lk(mtx_); sockets_[fd] = SockState{buffer, buffer_size, std::move(cb), {}, false, false, false}; }
    if (::port_associate(port_, PORT_SOURCE_FD, fd, POLLIN | POLLRDNORM | POLLRDHUP, nullptr) != 0) { IO_LOG_ERR("port_associate(fd=%d, POLLIN|...) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno)); return false; }
    return true;
  }
  bool delete_socket(socket_t fd) override {
    (void)::port_dissociate(port_, PORT_SOURCE_FD, fd);
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = timers_.find(fd); if (it != timers_.end()) { timer_delete(it->second); timers_.erase(it); }
    sockets_.erase(fd); timeouts_ms_.erase(fd);
    return true;
  }
  bool connect(socket_t fd, const char* host, uint16_t port, bool async) override {
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port); if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) return false;
    int flags = fcntl(fd, F_GETFL, 0); if (flags < 0) flags = 0; if (!async) { if (flags & O_NONBLOCK) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK); }
    int r = ::connect(fd, (sockaddr*)&addr, sizeof(addr)); int err = (r==0)?0:errno; int cur = fcntl(fd, F_GETFL, 0); if (cur<0) cur=0; fcntl(fd, F_SETFL, cur | O_NONBLOCK);
    if (r == 0) { if (::port_associate(port_, PORT_SOURCE_FD, fd, POLLIN | POLLRDNORM | POLLRDHUP, nullptr) != 0) return false; if (cbs_.on_accept) cbs_.on_accept(fd); return true; }
    if (async && (err == EINPROGRESS)) { (void)::port_associate(port_, PORT_SOURCE_FD, fd, POLLOUT | POLLIN | POLLRDHUP, nullptr); std::lock_guard<std::mutex> lk(mtx_); sockets_[fd].connecting = true; return true; }
    IO_LOG_ERR("connect(fd=%d) failed, res=%d, errno=%d (%s)", (int)fd, r, err, std::strerror(err));
    return false;
  }
  bool disconnect(socket_t fd) override { if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); IO_LOG_DBG("close: fd=%d", (int)fd); if (cbs_.on_close) cbs_.on_close(fd);} return true; }
  bool accept(socket_t listen_socket, bool async, uint32_t max_connections) override {
    if (async) { int flags = fcntl(listen_socket, F_GETFL, 0); fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK); }
    max_conn_ = max_connections; { std::lock_guard<std::mutex> lk(mtx_); listeners_.insert(listen_socket); }
    if (::port_associate(port_, PORT_SOURCE_FD, listen_socket, POLLIN, nullptr) != 0) {
      IO_LOG_ERR("port_associate(listen=%d, POLLIN) failed errno=%d (%s)", (int)listen_socket, errno, std::strerror(errno)); return false;
    }
    return true;
  }
  bool write(socket_t fd, const char* data, size_t data_size) override {
    std::lock_guard<std::mutex> lk(mtx_); auto it = sockets_.find(fd); if (it == sockets_.end()) return false; auto& st = it->second; st.out_queue.insert(st.out_queue.end(), data, data + data_size);
    if (!st.want_write) { st.want_write = true; (void)::port_associate(port_, PORT_SOURCE_FD, fd, POLLIN | POLLOUT | POLLRDHUP, nullptr); }
    return true;
  }
  bool pause_read(socket_t socket) override {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sockets_.find(socket); if (it == sockets_.end()) return false;
    it->second.paused = true;
    short mask = POLLRDHUP; if (it->second.want_write) mask |= POLLOUT; // drop POLLIN
    if (::port_associate(port_, PORT_SOURCE_FD, socket, mask, nullptr) != 0) {
      IO_LOG_ERR("port_associate(pause fd=%d) failed errno=%d (%s)", (int)socket, errno, std::strerror(errno));
      return false;
    }
    return true;
  }
  bool resume_read(socket_t socket) override {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sockets_.find(socket); if (it == sockets_.end()) return false;
    it->second.paused = false;
    short mask = POLLIN | POLLRDHUP; if (it->second.want_write) mask |= POLLOUT;
    if (::port_associate(port_, PORT_SOURCE_FD, socket, mask, nullptr) != 0) {
      IO_LOG_ERR("port_associate(resume fd=%d) failed errno=%d (%s)", (int)socket, errno, std::strerror(errno));
      return false;
    }
    return true;
  }
  bool post(uint32_t val) override { if (user_pipe_[1] == -1) return false; ssize_t n = ::write(user_pipe_[1], &val, sizeof(val)); return n == (ssize_t)sizeof(val); }
  bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override {
    // Solaris 11.4: native timers via timer_create + SIGEV_PORT, posting to our event port (PORT_SOURCE_TIMER)
    if (timeout_ms == 0) {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = timers_.find(socket);
      if (it != timers_.end()) { timer_delete(it->second); timers_.erase(it); }
      timeouts_ms_.erase(socket);
      return true;
    }
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (sockets_.find(socket) == sockets_.end()) return false;
    }
    // Create timer if not present
    timer_t tid{};
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = timers_.find(socket);
      if (it == timers_.end()) {
        struct sigevent sev{};
        sev.sigev_notify = SIGEV_PORT;
        // Pass socket id in user field and target port via notify attributes
        // Note: On Solaris, for SIGEV_PORT the sigev_value.sival_ptr points to a port_notify_t, which specifies the port and user data.
        struct port_notify pn{}; // alias type may be port_notify_t depending on headers
        std::memset(&pn, 0, sizeof(pn));
        pn.portnfy_port = port_;
        pn.portnfy_user = (void*)(intptr_t)socket;
        sev.sigev_value.sival_ptr = &pn;
        // timer_create is expected to copy port_notify contents internally
        if (timer_create(CLOCK_MONOTONIC, &sev, &tid) != 0) { IO_LOG_ERR("timer_create failed errno=%d", errno); return false; }
        timers_[socket] = tid;
        timeouts_ms_[socket] = timeout_ms;
      } else {
        tid = it->second;
        timeouts_ms_[socket] = timeout_ms;
      }
    }
    // Arm/rearm one-shot timeout
    itimerspec its{}; its.it_value.tv_sec = timeout_ms / 1000; its.it_value.tv_nsec = (timeout_ms % 1000) * 1000000L; its.it_interval = {0,0};
    if (timer_settime(tid, 0, &its, nullptr) != 0) { IO_LOG_ERR("timer_settime failed errno=%d (%s)", errno, std::strerror(errno)); return false; }
    return true;
  }
private:
  struct SockState { char* buf{nullptr}; size_t buf_size{0}; ReadCallback read_cb; std::vector<char> out_queue; bool want_write{false}; bool connecting{false}; bool paused{false}; };
  int port_{-1}; NetCallbacks cbs_{}; std::thread loop_{}; std::atomic<bool> running_{false}; int user_pipe_[2]{-1,-1};
  std::mutex mtx_; std::unordered_map<socket_t, SockState> sockets_; std::unordered_set<socket_t> listeners_;
  std::unordered_map<socket_t,uint32_t> timeouts_ms_; std::unordered_map<socket_t,timer_t> timers_;
  std::atomic<uint32_t> max_conn_{0}; std::atomic<uint32_t> cur_conn_{0};

  void event_loop() {
    while (running_) {
      port_event_t evs[64]; uint_t nget = 1; timespec ts{1,0}; int r = ::port_getn(port_, evs, 64, &nget, &ts);
      if (r != 0) { if (errno == EINTR) continue; IO_LOG_ERR("port_getn failed errno=%d (%s)", errno, std::strerror(errno)); continue; }
      if (nget == 0) continue;
      for (uint_t i=0;i<nget;++i) {
        auto& ev = evs[i];
        if (ev.portev_source == PORT_SOURCE_TIMER) {
          // Timer event posted via SIGEV_PORT; user field carries socket id
          socket_t sfd = static_cast<socket_t>((intptr_t)ev.portev_user);
          bool exists=false; { std::lock_guard<std::mutex> lk(mtx_); exists = sockets_.find(sfd)!=sockets_.end(); }
          if (exists) { if (cbs_.on_close) cbs_.on_close(sfd); ::close(sfd); std::lock_guard<std::mutex> lk2(mtx_); sockets_.erase(sfd); if (cur_conn_>0) cur_conn_--; timeouts_ms_.erase(sfd); auto itT=timers_.find(sfd); if(itT!=timers_.end()){ timer_delete(itT->second); timers_.erase(itT);} }
          continue;
        }
        if (ev.portev_source == PORT_SOURCE_FD) {
          int fd = static_cast<int>(reinterpret_cast<intptr_t>(ev.portev_object));
          if (fd == user_pipe_[0]) { uint32_t v; while (::read(user_pipe_[0], &v, sizeof(v)) == sizeof(v)) { if (cbs_.on_user) cbs_.on_user(v);} continue; }
          bool is_listener=false; { std::lock_guard<std::mutex> lk(mtx_); is_listener = listeners_.find(fd) != listeners_.end(); }
          if (is_listener && (ev.portev_events & POLLIN)) {
            while (true) { sockaddr_storage ss{}; socklen_t slen=sizeof(ss); socket_t client=::accept(fd,(sockaddr*)&ss,&slen); if (client<0){ if(errno==EAGAIN||errno==EWOULDBLOCK) break; else break;} if (max_conn_>0 && cur_conn_.load()>=max_conn_){::close(client); continue;} int fl=fcntl(client,F_GETFL,0); if(fl<0) fl=0; fcntl(client,F_SETFL,fl|O_NONBLOCK); cur_conn_++; IO_LOG_DBG("accept: client fd=%d", (int)client); if (cbs_.on_accept) cbs_.on_accept(client); }
            continue;
          }
          if (ev.portev_events & POLLOUT) {
            std::unique_lock<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if (it!=sockets_.end() && it->second.connecting) { int err=0; socklen_t len=sizeof(err); int gs = ::getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len); if(gs<0){ err=errno; IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", fd, err, std::strerror(err)); } else { IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", fd, err, std::strerror(err)); } it->second.connecting=false; if (err==0) { lk.unlock(); IO_LOG_DBG("connect: established fd=%d", fd); if (cbs_.on_accept) cbs_.on_accept(fd);} else { lk.unlock(); if (cbs_.on_close) cbs_.on_close(fd); ::close(fd); std::lock_guard<std::mutex> lk2(mtx_); sockets_.erase(fd); continue; } }
          }
          if (ev.portev_events & POLLIN) {
            std::unique_lock<std::mutex> lk(mtx_); auto it = sockets_.find(fd); if (it==sockets_.end()) { lk.unlock(); continue; } auto st = it->second; lk.unlock(); if(!st.buf||st.buf_size==0||!st.read_cb) continue; ssize_t rn=::recv(fd,st.buf,st.buf_size,0); if(rn>0){ st.read_cb(fd,st.buf,(size_t)rn);} else if(rn==0 || (ev.portev_events & (POLLHUP|POLLERR))){ if(cbs_.on_close) cbs_.on_close(fd); std::lock_guard<std::mutex> lk2(mtx_); sockets_.erase(fd); if(cur_conn_>0) cur_conn_--; auto itT=timers_.find(fd); if(itT!=timers_.end()){ int tfd=itT->second; ::port_dissociate(port_,PORT_SOURCE_FD,tfd); ::close(tfd); timers_.erase(itT);} timeouts_ms_.erase(fd); }
            // Rearm timer on successful read
            if (rn > 0) {
              timer_t tid{}; uint32_t ms=0; { std::lock_guard<std::mutex> lk2(mtx_); auto itT=timers_.find(fd); if(itT!=timers_.end()){ tid = itT->second; ms = timeouts_ms_[fd]; } }
              if (tid != (timer_t)0 && ms>0) { itimerspec its{}; its.it_value.tv_sec = ms/1000; its.it_value.tv_nsec = (ms%1000)*1000000L; timer_settime(tid, 0, &its, nullptr); }
            }
          }
          if (ev.portev_events & POLLOUT) {
            std::unique_lock<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if(it!=sockets_.end() && !it->second.out_queue.empty()) { auto& st=it->second; auto* p=st.out_queue.data(); size_t sz=st.out_queue.size(); lk.unlock(); ssize_t wn=::send(fd,p,sz,0); lk.lock(); if(wn>0){ if(cbs_.on_write) cbs_.on_write(fd,(size_t)wn); st.out_queue.erase(st.out_queue.begin(), st.out_queue.begin()+wn);} if(st.out_queue.empty()){ (void)::port_associate(port_,PORT_SOURCE_FD,fd,POLLIN|POLLRDHUP,nullptr); st.want_write=false; } }
          }
          // re-associate fd with new interest mask
          short mask = POLLRDHUP; { std::lock_guard<std::mutex> lk(mtx_); auto it=sockets_.find(fd); if (it!=sockets_.end()) { if (!it->second.paused) mask |= POLLIN; if (it->second.want_write) mask |= POLLOUT; } }
          if (::port_associate(port_, PORT_SOURCE_FD, fd, mask, nullptr) != 0) {
            IO_LOG_ERR("port_associate(rearm fd=%d, mask=0x%x) failed errno=%d (%s)", fd, mask, errno, std::strerror(errno));
          }
        }
      }
    }
  }
};

INetEngine* create_engine_eventports() { return new EventPortsEngine(); }

} // namespace io

#endif
