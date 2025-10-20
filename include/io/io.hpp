#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace io {

// Simple API contract example
// - read_file: read text file into string
// - write_file: write text to file (overwrite)
// - split_lines: utility to split text by new lines

std::string read_file(const std::string& path);
void write_file(const std::string& path, std::string_view content);
std::vector<std::string_view> split_lines(std::string_view text);

} // namespace io
