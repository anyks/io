#include <iostream>
#include "io/io.hpp"

int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string path = argv[1];
            const auto text = io::read_file(path);
            auto lines = io::split_lines(text);
            std::cout << "Lines: " << lines.size() << "\n";
        } else {
            const std::string sample = "hello\nworld\n";
            auto lines = io::split_lines(sample);
            for (auto l : lines) std::cout << l << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
