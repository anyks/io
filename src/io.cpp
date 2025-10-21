#include "io/io.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace io {

std::string read_file(const std::string &path) {
	std::ifstream in(path, std::ios::in | std::ios::binary);
	if (!in.is_open()) {
		throw std::runtime_error("Failed to open file: " + path);
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

void write_file(const std::string &path, std::string_view content) {
	std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		throw std::runtime_error("Failed to open file for write: " + path);
	}
	out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::vector<std::string_view> split_lines(std::string_view text) {
	std::vector<std::string_view> result;
	size_t start = 0;
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\n') {
			result.emplace_back(text.substr(start, i - start));
			start = i + 1;
		}
	}
	// tail
	if (start <= text.size()) {
		result.emplace_back(text.substr(start));
	}
	return result;
}

} // namespace io
