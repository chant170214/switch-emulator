#include "gui.h"

#include <cstdlib>
#include <iostream>
#include <memory>

#ifdef SWITCH_EMU_HAS_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstring>

struct GuiViewer::Impl {
    Impl(std::size_t width, std::size_t height) : width(width), height(height) {
        display = XOpenDisplay(nullptr);
        if (!display) {
            std::cerr << "Failed to open X11 display. GUI preview disabled.\n";
            return;
        }
        screen = DefaultScreen(display);
        window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, static_cast<unsigned int>(width),
                                     static_cast<unsigned int>(height), 1, BlackPixel(display, screen),
                                     WhitePixel(display, screen));
        XStoreName(display, window, "Switch Emulator Framebuffer");
        XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
        gc = XCreateGC(display, window, 0, nullptr);
        wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display, window, &wm_delete, 1);
        XMapWindow(display, window);
        XFlush(display);
    }

    ~Impl() {
        if (display && gc) {
            XFreeGC(display, gc);
        }
        if (display && window) {
            XDestroyWindow(display, window);
        }
        if (display) {
            XCloseDisplay(display);
        }
    }

    bool present(const std::vector<std::uint32_t>& framebuffer) {
        if (!display || framebuffer.size() != width * height) {
            return false;
        }
        const std::size_t pixel_bytes = width * height * sizeof(std::uint32_t);
        unsigned char* buffer = static_cast<unsigned char*>(std::malloc(pixel_bytes));
        if (!buffer) {
            std::cerr << "Failed to allocate image buffer for GUI preview.\n";
            return false;
        }
        std::uint32_t* dst = reinterpret_cast<std::uint32_t*>(buffer);
        for (std::size_t i = 0; i < width * height; ++i) {
            const std::uint32_t pixel = framebuffer[i];
            const std::uint32_t r = (pixel >> 16) & 0xFFu;
            const std::uint32_t g = (pixel >> 8) & 0xFFu;
            const std::uint32_t b = pixel & 0xFFu;
            dst[i] = (r << 16) | (g << 8) | b;
        }

        XImage* image = XCreateImage(display, DefaultVisual(display, screen), 24, ZPixmap, 0,
                                     reinterpret_cast<char*>(buffer), static_cast<unsigned int>(width),
                                     static_cast<unsigned int>(height), 32, 0);
        if (!image) {
            std::cerr << "Failed to create XImage for GUI preview.\n";
            std::free(buffer);
            return false;
        }

        bool keep_running = true;
        while (keep_running) {
            while (XPending(display)) {
                XEvent event;
                XNextEvent(display, &event);
                switch (event.type) {
                case Expose:
                    XPutImage(display, window, gc, image, 0, 0, 0, 0, static_cast<unsigned int>(width),
                              static_cast<unsigned int>(height));
                    break;
                case ConfigureNotify:
                    break;
                case ClientMessage:
                    if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete) {
                        keep_running = false;
                    }
                    break;
                case KeyPress:
                case ButtonPress:
                    keep_running = false;
                    break;
                default:
                    break;
                }
            }
            if (keep_running) {
                XPutImage(display, window, gc, image, 0, 0, 0, 0, static_cast<unsigned int>(width),
                          static_cast<unsigned int>(height));
                XFlush(display);
            }
        }

        XDestroyImage(image);
        return true;
    }

    Display* display = nullptr;
    ::Window window = 0;
    GC gc = nullptr;
    Atom wm_delete = None;
    int screen = 0;
    std::size_t width = 0;
    std::size_t height = 0;

    bool ready() const { return display != nullptr; }
};

#else

struct GuiViewer::Impl {
    Impl(std::size_t, std::size_t) {}
    bool present(const std::vector<std::uint32_t>&) { return false; }
    bool ready() const { return false; }
};

#endif

GuiViewer::GuiViewer(std::size_t width, std::size_t height) : impl_(std::make_unique<Impl>(width, height)) {}

GuiViewer::~GuiViewer() = default;

GuiViewer::GuiViewer(GuiViewer&&) noexcept = default;
GuiViewer& GuiViewer::operator=(GuiViewer&&) noexcept = default;

bool GuiViewer::available() const noexcept { return impl_ && impl_->ready(); }

bool GuiViewer::present(const std::vector<std::uint32_t>& framebuffer) {
#ifdef SWITCH_EMU_HAS_X11
    if (!available()) {
        std::cerr << "GUI preview unavailable: missing X11 display.\n";
        return false;
    }
    return impl_->present(framebuffer);
#else
    (void)framebuffer;
    std::cerr << "GUI preview unavailable: built without X11 support.\n";
    return false;
#endif
}
