#include <gtest/gtest.h>
#include "io/io.hpp"
#include <cstdio>

TEST(IoTest, SplitLines) {
    auto v = io::split_lines("a\nb\n");
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "b");
    EXPECT_EQ(v[2], "");
}

TEST(IoTest, WriteReadRoundtrip) {
    const std::string path = "test_tmp.txt";
    const std::string content = "alpha\nbeta\n";
    io::write_file(path, content);
    const auto read = io::read_file(path);
    EXPECT_EQ(read, content);
    std::remove(path.c_str());
}
