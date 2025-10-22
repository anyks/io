// Cross-engine helpers
#include "io/net.hpp"
#include <mutex>
#include <atomic>
#if !defined(_WIN32)
#include <signal.h>
#endif

namespace io {

void suppress_sigpipe_once() {
#if !defined(_WIN32) && defined(SIGPIPE)
	static std::once_flag flag;
	std::call_once(flag, []() {
		::signal(SIGPIPE, SIG_IGN);
	});
#endif
}

// Diagnostics: broken pipe counter
static std::atomic<uint64_t> g_broken_pipe_count{0};

void record_broken_pipe() {
	g_broken_pipe_count.fetch_add(1, std::memory_order_relaxed);
}

uint64_t broken_pipe_count() {
	return g_broken_pipe_count.load(std::memory_order_relaxed);
}

void reset_broken_pipe_count() {
	g_broken_pipe_count.store(0, std::memory_order_relaxed);
}

} // namespace io
