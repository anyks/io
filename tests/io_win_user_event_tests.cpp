#include <gtest/gtest.h>
#include "io/net.hpp"
#include <atomic>
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

TEST(WinIocp, UserEventSmoke) {
    io::NetCallbacks cbs{};
    std::atomic<bool> got{false};
    cbs.on_user = [&](uint32_t v){ if (v == 42) got.store(true, std::memory_order_relaxed); };

    std::unique_ptr<io::INetEngine> eng(io::create_engine());
    ASSERT_TRUE(eng != nullptr);
    ASSERT_TRUE(eng->init(cbs));

    std::thread t([&](){ eng->start(100); });

    // Post a user event and wait briefly
    ASSERT_TRUE(eng->post(42));

    for (int i = 0; i < 50 && !got.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(10ms);
    }

    eng->stop();
    if (t.joinable()) t.join();
    eng->destroy();

    ASSERT_TRUE(got.load(std::memory_order_relaxed));
}
