#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class GuiViewer {
public:
    GuiViewer(std::size_t width, std::size_t height);
    ~GuiViewer();

    GuiViewer(const GuiViewer&) = delete;
    GuiViewer& operator=(const GuiViewer&) = delete;
    GuiViewer(GuiViewer&&) noexcept;
    GuiViewer& operator=(GuiViewer&&) noexcept;

    bool available() const noexcept;

    bool present(const std::vector<std::uint32_t>& framebuffer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
