// g++ -O2 -std=c++17 example.cpp ../../yapf.c -o example -pthread && ./example ../../other/YAPF.YAPF
#include "yapf.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "../../other/YAPF.YAPF";
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});

    yapf::Image img = yapf::Image::decode(bytes);
    double ms = 1e9;                                  // best of 10, as in a running program
    for (int i = 0; i < 10; i++) {
        auto t0 = std::chrono::steady_clock::now();
        img = yapf::Image::decode(bytes);
        ms = std::min(ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::printf("%s: %ux%u, %u channels, decoded in %.2f ms, file is %.1f%% of raw\n", path,
                img.width(), img.height(), img.channels(), ms, 100.0 * bytes.size() / img.size());

    std::vector<uint8_t> again = img.encode();
    std::printf("re-encoded: %zu bytes, identical to the file: %s\n", again.size(),
                again == bytes ? "yes" : "no");

    std::vector<uint8_t> gray(64 * 64);
    for (size_t i = 0; i < gray.size(); i++) gray[i] = uint8_t(i % 64 * 4);
    yapf::Image(64, 64, 1, gray.data()).save("gray.yapf");
    std::printf("wrote gray.yapf\n");
}
