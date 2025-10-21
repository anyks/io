#include "io/net.hpp"

namespace io {

// Forward declares for per-engine factory functions implemented in engine files
INetEngine *create_engine_kqueue();
INetEngine *create_engine_epoll();
INetEngine *create_engine_iouring();
INetEngine *create_engine_iocp();
INetEngine *create_engine_eventports();
INetEngine *create_engine_devpoll();

INetEngine *create_engine() {
#if defined(IO_ENGINE_KQUEUE)
	return create_engine_kqueue();
#elif defined(IO_ENGINE_IOURING)
	return create_engine_iouring();
#elif defined(IO_ENGINE_EPOLL)
	return create_engine_epoll();
#elif defined(IO_ENGINE_IOCP)
	return create_engine_iocp();
#elif defined(IO_ENGINE_EVENTPORTS)
	return create_engine_eventports();
#elif defined(IO_ENGINE_DEVPOLL)
	return create_engine_devpoll();
#else
	return nullptr;
#endif
}

} // namespace io
