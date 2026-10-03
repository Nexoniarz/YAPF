// yapf.hpp — header-only C++17 wrapper for YAPF.  Copyright 2026 Nexoniarz — Apache 2.0.
//
//   #include "yapf.hpp"          // and compile/link ../../yapf.c
//   yapf::Image img = yapf::Image::load("texture.yapf");
//   img.width(); img.height(); img.channels(); img.pixels();   // span of bytes
//   std::vector<uint8_t> file = img.encode();
//   yapf::Image mine(256, 256, 4, my_rgba_bytes);  mine.save("out.yapf");
#pragma once

extern "C" {
#include "../../yapf.h"
}

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace yapf {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Image {
public:
    /* Decode a file or a memory buffer; threads = 0 uses every core, 1 the calling thread. */
    static Image load(const std::string &path, int threads = 1) {
        yapf_image_t *p = yapf_load_mt(path.c_str(), threads);
        if (!p) throw Error("YAPF: cannot load " + path);
        return Image(p);
    }
    static Image decode(const void *data, size_t size, int threads = 1) {
        yapf_image_t *p = yapf_load_memory_mt(data, size, threads);
        if (!p) throw Error("YAPF: invalid or corrupt data");
        return Image(p);
    }
    static Image decode(const std::vector<uint8_t> &bytes, int threads = 1) {
        return decode(bytes.data(), bytes.size(), threads);
    }

    /* A new image from your pixels (copied).  channels: 1 gray, 2 gray+A, 3 RGB, 4 RGBA. */
    Image(uint32_t width, uint32_t height, uint8_t channels, const uint8_t *pixels,
          uint8_t flags = YAPF_FLAG_SRGB)
        : own_(pixels, pixels + size_t(width) * height * channels) {
        std::memset(&view_, 0, sizeof(view_));
        view_.width = width; view_.height = height; view_.channels = channels;
        view_.flags = flags; view_.mip_levels = 1;
        view_.gpu_format = channels == 4 ? YAPF_GPU_SRGB8_A8 : channels == 3 ? YAPF_GPU_SRGB8
                         : channels == 2 ? YAPF_GPU_RG8 : YAPF_GPU_R8;
        view_.pixels = own_.data();
    }

    Image(Image &&o) noexcept : img_(std::exchange(o.img_, nullptr)), own_(std::move(o.own_)), view_(o.view_) {
        if (!img_) view_.pixels = own_.data();
    }
    Image &operator=(Image &&o) noexcept {
        if (this != &o) { yapf_free(img_); img_ = std::exchange(o.img_, nullptr); own_ = std::move(o.own_);
                          view_ = o.view_; if (!img_) view_.pixels = own_.data(); }
        return *this;
    }
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;
    ~Image() { yapf_free(img_); }

    uint32_t width()     const { return raw().width; }
    uint32_t height()    const { return raw().height; }
    uint8_t  channels()  const { return raw().channels; }
    uint8_t  flags()     const { return raw().flags; }
    uint8_t  mipLevels() const { return raw().mip_levels; }
    const uint8_t *pixels() const { return raw().pixels; }
    size_t   size()      const { return size_t(width()) * height() * channels(); }
    const uint8_t *mip(int level) const { return raw().mips ? raw().mips[level] : raw().pixels; }

    std::vector<uint8_t> encode() const {
        void *data = nullptr;
        size_t n = 0;
        if (yapf_encode(&raw(), &data, &n) != YAPF_OK) throw Error("YAPF: encoding failed");
        std::vector<uint8_t> out(static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + n);
        yapf_free_buffer(data);
        return out;
    }
    void save(const std::string &path) const {
        if (yapf_save(path.c_str(), &raw()) != YAPF_OK) throw Error("YAPF: cannot write " + path);
    }

    const yapf_image_t &raw() const { return img_ ? *img_ : view_; }

private:
    explicit Image(yapf_image_t *p) : img_(p) { std::memset(&view_, 0, sizeof(view_)); }
    yapf_image_t        *img_ = nullptr;   // decoded by the library
    std::vector<uint8_t> own_;             // or pixels we own
    yapf_image_t         view_{};
};

} // namespace yapf
