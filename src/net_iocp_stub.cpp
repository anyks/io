// Minimal IOCP stub for MinGW builds: provides factory symbol but no implementation.
// This allows Windows+MinGW smoke builds to succeed while the real IOCP backend is WIP.

#include "io/net.hpp"

namespace io {

// On MinGW we expose the factory symbol but return nullptr to indicate unavailability.
INetEngine *create_engine_iocp() {
    return nullptr;
}

} // namespace io
