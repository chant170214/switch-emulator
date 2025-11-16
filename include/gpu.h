#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct SpriteDescriptor {
    std::size_t texture_id = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool flip_x = false;
    bool flip_y = false;
};

class GPU {
public:
    GPU(std::size_t width = 128, std::size_t height = 72);

    void reset();

    void draw_pixel(int x, int y, std::uint32_t rgba);

    void fill(std::uint32_t rgba);

    void submit_rectangle(int x, int y, int w, int h, std::uint32_t rgba);

    void draw_line(int x0, int y0, int x1, int y1, std::uint32_t rgba);

    void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2, std::uint32_t rgba, bool fill = true);

    void draw_sprite(const SpriteDescriptor& sprite);

    std::size_t upload_texture(std::size_t width, std::size_t height, const std::vector<std::uint32_t>& pixels);

    void set_render_target(std::size_t target_index);

    void swap_buffers();

    void set_blend_mode(const std::string& mode_name);

    void set_depth_test(bool enabled);

    void clear_depth();

    void dma_blit(const std::vector<std::uint32_t>& src, std::size_t stride, int dst_x, int dst_y, std::size_t width,
                  std::size_t height);

    using DebugHook = std::function<void(int, int, std::uint32_t)>;
    void set_debug_hook(DebugHook hook);

    const std::vector<std::uint32_t>& framebuffer() const noexcept;

    std::size_t width() const noexcept { return width_; }
    std::size_t height() const noexcept { return height_; }

    std::uint64_t checksum() const noexcept;

    std::string dump_to_ppm(const std::string& path) const;

    std::string render_ascii_art(std::size_t max_columns = 80, bool colorized = true) const;

private:
    enum class BlendMode { None, Alpha, Additive };

    struct RenderTarget {
        std::vector<std::uint32_t> color;
        std::vector<float> depth;
    };

    std::size_t width_;
    std::size_t height_;
    std::vector<RenderTarget> targets_;
    std::size_t active_target_ = 0;
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> textures_;
    std::size_t next_texture_id_ = 1;
    BlendMode blend_mode_ = BlendMode::None;
    bool depth_test_enabled_ = false;
    DebugHook debug_hook_;

    std::vector<std::uint32_t>& active_color();
    const std::vector<std::uint32_t>& active_color() const;
    std::vector<float>& active_depth();
    std::uint32_t blend_pixels(std::uint32_t src, std::uint32_t dst) const;
    bool depth_pass(int x, int y, float depth);
    void write_pixel(int x, int y, std::uint32_t rgba, float depth = 0.0f);
};
