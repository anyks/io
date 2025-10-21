// Echo client example: send 100 framed messages to server and verify echoed data
#include "io/net.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static inline void be32_write(uint32_t v, char *out) {
#if defined(_WIN32) || defined(_WIN64)
	uint32_t n = htonl(v);
#else
	uint32_t n = htonl(v);
#endif
	std::memcpy(out, &n, sizeof(n));
}

static inline uint32_t be32_read(const char *in) {
	uint32_t n = 0;
	std::memcpy(&n, in, sizeof(n));
#if defined(_WIN32) || defined(_WIN64)
	return ntohl(n);
#else
	return ntohl(n);
#endif
}

int main() {
	using namespace io;

#if defined(_WIN32) || defined(_WIN64)
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		std::cerr << "WSAStartup failed" << std::endl;
		return 1;
	}
#endif

	auto *engine = create_engine();
	if (!engine) {
		std::cerr << "No engine" << std::endl;
		return 1;
	}

	std::atomic<bool> connected{false};
	std::atomic<bool> done{false};
	std::atomic<int> exit_code{1};

	// Prepare 100 messages with varying sizes and deterministic content
	std::vector<std::string> messages;
	messages.reserve(100);
	for (int i = 1; i <= 100; ++i) {
		size_t sz = (static_cast<size_t>(i) * 113) % 2048 + (i % 13);
		if (sz == 0)
			sz = 1;
		std::string m;
		m.resize(sz);
		for (size_t j = 0; j < sz; ++j)
			m[j] = char('A' + ((i + int(j)) % 26));
		messages.emplace_back(std::move(m));
	}

	// Receive-side state
	std::vector<char> recv_accum;
	recv_accum.reserve(1 << 16);
	size_t frames_received = 0;

	NetCallbacks cbs{};
	cbs.on_accept = [&](socket_t s) {
		std::cout << "connected: 0x" << std::hex << (static_cast<std::uintptr_t>(s)) << std::dec << std::endl;
		connected.store(true);
	};
	cbs.on_close = [&](socket_t s) {
		std::cout << "closed: 0x" << std::hex << (static_cast<std::uintptr_t>(s)) << std::dec << std::endl;
	};

	if (!engine->init(cbs)) {
		std::cerr << "init failed" << std::endl;
		return 1;
	}

	socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd == kInvalidSocket) {
		std::cerr << "socket() failed" << std::endl;
		return 1;
	}

	// Register read buffer and frame parser
	static thread_local char read_buf[8192];
	engine->add_socket(fd, read_buf, sizeof(read_buf), [&](socket_t /*s*/, char *b, size_t n) {
		recv_accum.insert(recv_accum.end(), b, b + n);
		// Parse frames: [4-byte BE length][payload]
		size_t offset = 0;
		while (true) {
			if (recv_accum.size() - offset < 4)
				break;
			uint32_t len = be32_read(&recv_accum[offset]);
			if (recv_accum.size() - offset < 4u + len)
				break;
			// Full frame available
			const char *payload = &recv_accum[offset + 4];
			size_t idx = frames_received;
			if (idx < messages.size()) {
				const std::string &exp = messages[idx];
				bool ok = (exp.size() == len) && (std::memcmp(exp.data(), payload, len) == 0);
				if (!ok) {
					std::cerr << "Mismatch at frame #" << idx << ": expected " << exp.size() << " bytes" << std::endl;
					done.store(true);
					exit_code.store(2);
					break;
				}
			}
			frames_received++;
			offset += (4u + len);
			if (frames_received == messages.size()) {
				std::cout << "All " << frames_received << " frames verified" << std::endl;
				done.store(true);
				exit_code.store(0);
			}
		}
		if (offset > 0) {
			// erase consumed bytes
			recv_accum.erase(recv_accum.begin(), recv_accum.begin() + static_cast<std::ptrdiff_t>(offset));
		}
	});

	if (!engine->connect(fd, "127.0.0.1", 8080, true)) {
		std::cerr << "connect failed" << std::endl;
		return 1;
	}

	// Wait for connection notification before sending
	for (int i = 0; i < 200 && !connected.load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	if (!connected.load()) {
		std::cerr << "connect timeout" << std::endl;
		return 1;
	}

	// Send frames: [len][payload]
	for (const auto &m : messages) {
		char hdr[4];
		be32_write(static_cast<uint32_t>(m.size()), hdr);
		engine->write(fd, hdr, sizeof(hdr));
		if (!m.empty())
			engine->write(fd, m.data(), m.size());
	}

	// Wait for completion
	while (!done.load())
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

	engine->disconnect(fd);
	engine->destroy();
#if defined(_WIN32) || defined(_WIN64)
	WSACleanup();
#endif
	return exit_code.load();
}
