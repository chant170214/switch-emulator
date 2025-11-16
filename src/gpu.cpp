#include "gpu.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sstream>

namespace {
inline float unpack_depth(std::uint32_t color) {
    return static_cast<float>((color >> 24) & 0xFF) / 255.0f;
}
}

GPU::GPU(std::size_t width, std::size_t height) : width_(width), height_(height) {
    targets_.resize(2);
    for (auto& target : targets_) {
        target.color.resize(width_ * height_, 0);
        target.depth.resize(width_ * height_, 1.0f);
    }
}

void GPU::reset() {
    for (auto& target : targets_) {
        std::fill(target.color.begin(), target.color.end(), 0);
        std::fill(target.depth.begin(), target.depth.end(), 1.0f);
    }
    blend_mode_ = BlendMode::None;
    depth_test_enabled_ = false;
    debug_hook_ = nullptr;
}

std::vector<std::uint32_t>& GPU::active_color() {
    return targets_[active_target_].color;
}

const std::vector<std::uint32_t>& GPU::active_color() const {
    return targets_[active_target_].color;
}

std::vector<float>& GPU::active_depth() {
    return targets_[active_target_].depth;
}

void GPU::draw_pixel(int x, int y, std::uint32_t rgba) {
    write_pixel(x, y, rgba, unpack_depth(rgba));
}

void GPU::fill(std::uint32_t rgba) {
    auto& color = active_color();
    std::fill(color.begin(), color.end(), rgba);
}

void GPU::submit_rectangle(int x, int y, int w, int h, std::uint32_t rgba) {
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            write_pixel(x + col, y + row, rgba, unpack_depth(rgba));
        }
    }
}

void GPU::draw_line(int x0, int y0, int x1, int y1, std::uint32_t rgba) {
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        write_pixel(x0, y0, rgba, unpack_depth(rgba));
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static float edge_function(float ax, float ay, float bx, float by, float cx, float cy) {
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

void GPU::draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2, std::uint32_t rgba, bool fill) {
    if (!fill) {
        draw_line(x0, y0, x1, y1, rgba);
        draw_line(x1, y1, x2, y2, rgba);
        draw_line(x2, y2, x0, y0, rgba);
        return;
    }

    const int min_x = std::max(0, std::min({x0, x1, x2}));
    const int max_x = std::min(static_cast<int>(width_) - 1, std::max({x0, x1, x2}));
    const int min_y = std::max(0, std::min({y0, y1, y2}));
    const int max_y = std::min(static_cast<int>(height_) - 1, std::max({y0, y1, y2}));

    const float area = edge_function(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1),
                                     static_cast<float>(y1), static_cast<float>(x2), static_cast<float>(y2));
    if (std::abs(area) < std::numeric_limits<float>::epsilon()) {
        return;
    }

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float w0 = edge_function(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(x2),
                                           static_cast<float>(y2), static_cast<float>(x), static_cast<float>(y));
            const float w1 = edge_function(static_cast<float>(x2), static_cast<float>(y2), static_cast<float>(x0),
                                           static_cast<float>(y0), static_cast<float>(x), static_cast<float>(y));
            const float w2 = edge_function(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1),
                                           static_cast<float>(y1), static_cast<float>(x), static_cast<float>(y));

            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                const float depth = unpack_depth(rgba);
                write_pixel(x, y, rgba, depth);
            }
        }
    }
}

void GPU::draw_sprite(const SpriteDescriptor& sprite) {
    const auto texture_it = textures_.find(sprite.texture_id);
    if (texture_it == textures_.end()) {
        throw std::runtime_error("GPU::draw_sprite unknown texture id");
    }
    const auto& tex = texture_it->second;
    if (sprite.width <= 0 || sprite.height <= 0) {
        return;
    }
    const std::size_t tex_width = static_cast<std::size_t>(sprite.width);
    const std::size_t tex_height = static_cast<std::size_t>(sprite.height);
    if (tex.size() < tex_width * tex_height) {
        throw std::runtime_error("GPU::draw_sprite texture dimensions mismatch");
    }

    for (int y = 0; y < sprite.height; ++y) {
        const int src_y = sprite.flip_y ? (sprite.height - 1 - y) : y;
        for (int x = 0; x < sprite.width; ++x) {
            const int src_x = sprite.flip_x ? (sprite.width - 1 - x) : x;
            const std::uint32_t pixel = tex[static_cast<std::size_t>(src_y) * tex_width + static_cast<std::size_t>(src_x)];
            write_pixel(sprite.x + x, sprite.y + y, pixel, unpack_depth(pixel));
        }
    }
}

std::size_t GPU::upload_texture(std::size_t width, std::size_t height, const std::vector<std::uint32_t>& pixels) {
    if (width * height > pixels.size()) {
        throw std::runtime_error("GPU::upload_texture insufficient pixel data");
    }
    const std::size_t id = next_texture_id_++;
    textures_[id] = std::vector<std::uint32_t>(pixels.begin(), pixels.begin() + width * height);
    return id;
}

void GPU::set_render_target(std::size_t target_index) {
    if (target_index >= targets_.size()) {
        throw std::out_of_range("GPU::set_render_target index out of range");
    }
    active_target_ = target_index;
}

void GPU::swap_buffers() {
    if (targets_.size() < 2) {
        return;
    }
    std::swap(targets_[0], targets_[1]);
}

void GPU::set_blend_mode(const std::string& mode_name) {
    const std::string lower = [&]() {
        std::string tmp = mode_name;
        std::transform(tmp.begin(), tmp.end(), tmp.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return tmp;
    }();
    if (lower == "none") {
        blend_mode_ = BlendMode::None;
    } else if (lower == "alpha") {
        blend_mode_ = BlendMode::Alpha;
    } else if (lower == "add" || lower == "additive") {
        blend_mode_ = BlendMode::Additive;
    } else {
        throw std::runtime_error("GPU::set_blend_mode unknown mode: " + mode_name);
    }
}

void GPU::set_depth_test(bool enabled) {
    depth_test_enabled_ = enabled;
}

void GPU::clear_depth() {
    std::fill(active_depth().begin(), active_depth().end(), 1.0f);
}

void GPU::dma_blit(const std::vector<std::uint32_t>& src, std::size_t stride, int dst_x, int dst_y, std::size_t width,
                   std::size_t height) {
    if (stride < width) {
        throw std::runtime_error("GPU::dma_blit stride too small");
    }
    for (std::size_t row = 0; row < height; ++row) {
        for (std::size_t col = 0; col < width; ++col) {
            const std::uint32_t pixel = src[row * stride + col];
            write_pixel(dst_x + static_cast<int>(col), dst_y + static_cast<int>(row), pixel, unpack_depth(pixel));
        }
    }
}

void GPU::set_debug_hook(DebugHook hook) {
    debug_hook_ = std::move(hook);
}

const std::vector<std::uint32_t>& GPU::framebuffer() const noexcept {
    return targets_.front().color;
}

std::uint64_t GPU::checksum() const noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    for (std::uint32_t pixel : framebuffer()) {
        hash ^= pixel;
        hash *= prime;
    }
    return hash;
}

std::string GPU::dump_to_ppm(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("GPU::dump_to_ppm failed to open file");
    }

    out << "P3\n" << width_ << ' ' << height_ << "\n255\n";
    for (std::size_t y = 0; y < height_; ++y) {
        for (std::size_t x = 0; x < width_; ++x) {
            std::uint32_t pixel = framebuffer()[y * width_ + x];
            std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xFF);
            std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xFF);
            std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xFF);
            out << static_cast<int>(r) << ' ' << static_cast<int>(g) << ' ' << static_cast<int>(b);
            if (x + 1 < width_) {
                out << ' ';
            }
        }
        out << '\n';
    }

    return path;
}

std::string GPU::render_ascii_art(std::size_t max_columns, bool colorized) const {
    if (max_columns == 0) {
        max_columns = 1;
    }
    const auto& fb = framebuffer();
    if (fb.empty() || width_ == 0 || height_ == 0) {
        return {};
    }

    const std::size_t columns = std::min<std::size_t>(max_columns, width_);
    const double scale_x = static_cast<double>(width_) / static_cast<double>(columns);
    const double scale_y = scale_x * 2.0;  // compensate for terminal cell aspect ratio
    const std::size_t rows = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(static_cast<double>(height_) / scale_y)));

    const std::string gradient = " .:-=+*#%@";
    std::ostringstream oss;

    for (std::size_t row = 0; row < rows; ++row) {
        const std::size_t src_y0 = std::min<std::size_t>(height_ - 1,
                                                        static_cast<std::size_t>(row * scale_y));
        const std::size_t src_y1 = std::min<std::size_t>(
            height_, std::max<std::size_t>(src_y0 + 1,
                                           static_cast<std::size_t>(std::ceil((row + 1) * scale_y))));
        for (std::size_t col = 0; col < columns; ++col) {
            const std::size_t src_x0 = std::min<std::size_t>(width_ - 1,
                                                            static_cast<std::size_t>(col * scale_x));
            const std::size_t src_x1 = std::min<std::size_t>(
                width_, std::max<std::size_t>(src_x0 + 1,
                                              static_cast<std::size_t>(std::ceil((col + 1) * scale_x))));
            double sum_r = 0.0;
            double sum_g = 0.0;
            double sum_b = 0.0;
            std::size_t count = 0;
            for (std::size_t y = src_y0; y < src_y1; ++y) {
                for (std::size_t x = src_x0; x < src_x1; ++x) {
                    const std::uint32_t pixel = fb[y * width_ + x];
                    sum_r += static_cast<double>((pixel >> 16) & 0xFF);
                    sum_g += static_cast<double>((pixel >> 8) & 0xFF);
                    sum_b += static_cast<double>(pixel & 0xFF);
                    ++count;
                }
            }
            if (count == 0) {
                count = 1;
            }
            const double avg_r = sum_r / static_cast<double>(count);
            const double avg_g = sum_g / static_cast<double>(count);
            const double avg_b = sum_b / static_cast<double>(count);
            const double luminance = 0.2126 * avg_r + 0.7152 * avg_g + 0.0722 * avg_b;
            const double normalized = std::clamp(luminance / 255.0, 0.0, 1.0);
            const std::size_t grad_index = static_cast<std::size_t>(normalized * (gradient.size() - 1));
            const char glyph = gradient[grad_index];
            if (colorized) {
                oss << "\x1b[38;2;" << static_cast<int>(avg_r) << ';' << static_cast<int>(avg_g) << ';'
                    << static_cast<int>(avg_b) << 'm' << glyph;
            } else {
                oss << glyph;
            }
        }
        if (colorized) {
            oss << "\x1b[0m";
        }
        oss << '\n';
    }

    if (colorized) {
        oss << "\x1b[0m";
    }

    return oss.str();
}

std::uint32_t GPU::blend_pixels(std::uint32_t src, std::uint32_t dst) const {
    if (blend_mode_ == BlendMode::None) {
        return src;
    }
    auto extract = [](std::uint32_t color, int shift) {
        return static_cast<float>((color >> shift) & 0xFF);
    };
    float src_r = extract(src, 16);
    float src_g = extract(src, 8);
    float src_b = extract(src, 0);
    float src_a = extract(src, 24) / 255.0f;
    float dst_r = extract(dst, 16);
    float dst_g = extract(dst, 8);
    float dst_b = extract(dst, 0);

    float out_r = src_r;
    float out_g = src_g;
    float out_b = src_b;
    if (blend_mode_ == BlendMode::Alpha) {
        out_r = src_r * src_a + dst_r * (1.0f - src_a);
        out_g = src_g * src_a + dst_g * (1.0f - src_a);
        out_b = src_b * src_a + dst_b * (1.0f - src_a);
    } else if (blend_mode_ == BlendMode::Additive) {
        out_r = std::min(255.0f, src_r + dst_r);
        out_g = std::min(255.0f, src_g + dst_g);
        out_b = std::min(255.0f, src_b + dst_b);
    }

    const std::uint32_t r = static_cast<std::uint32_t>(out_r) & 0xFFu;
    const std::uint32_t g = static_cast<std::uint32_t>(out_g) & 0xFFu;
    const std::uint32_t b = static_cast<std::uint32_t>(out_b) & 0xFFu;
    const std::uint32_t a = (blend_mode_ == BlendMode::Alpha) ? static_cast<std::uint32_t>(src_a * 255.0f) : 0xFFu;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

bool GPU::depth_pass(int x, int y, float depth) {
    if (!depth_test_enabled_) {
        return true;
    }
    if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= width_ || static_cast<std::size_t>(y) >= height_) {
        return false;
    }
    auto& depth_buf = active_depth();
    const std::size_t index = static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x);
    if (depth < depth_buf[index]) {
        depth_buf[index] = depth;
        return true;
    }
    return false;
}

void GPU::write_pixel(int x, int y, std::uint32_t rgba, float depth) {
    if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= width_ || static_cast<std::size_t>(y) >= height_) {
        return;
    }
    if (!depth_pass(x, y, depth)) {
        return;
    }
    auto& color = active_color();
    const std::size_t index = static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x);
    std::uint32_t blended = blend_pixels(rgba, color[index]);
    color[index] = blended;
    if (debug_hook_) {
        debug_hook_(x, y, blended);
    }
}
