# YAPF for C++

`yapf.hpp` is a header-only C++17 wrapper: RAII, exceptions, `std::vector`.
Add `../../yapf.c` to your project and include the header.

```cpp
#include "yapf.hpp"

yapf::Image img = yapf::Image::load("texture.yapf");     // or Image::decode(bytes)
upload(img.width(), img.height(), img.channels(), img.pixels());

std::vector<uint8_t> file = yapf::Image(256, 256, 4, rgba).encode();
```

`yapf.c` compiles as C or as C++, so you can simply add it to a C++ project
(Visual Studio, CMake, …).

Example: `g++ -O2 -std=c++17 example.cpp -x c++ ../../yapf.c -o example -pthread && ./example`
