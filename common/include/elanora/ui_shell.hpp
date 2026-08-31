#pragma once
//
// GLFW + OpenGL3 + Dear ImGui + ImPlot bootstrap, shared by all four apps.
//
// Usage:
//     UiShell shell("ELANORA Collector", 1440, 900);
//     if (!shell.ok()) return 1;
//     while (shell.begin_frame()) {
//         ... ImGui calls ...
//         shell.end_frame();
//     }
//
// begin_frame() returns false when the user closes the window, so the loop
// condition and the close check are the same thing.

#include <string>

struct GLFWwindow;

namespace elanora {

class UiShell {
public:
    UiShell(const char* title, int width, int height);
    ~UiShell();

    UiShell(const UiShell&)            = delete;
    UiShell& operator=(const UiShell&) = delete;

    // False if GLFW, the GL context, or ImGui failed to initialise. Check this
    // before entering the loop; error() explains what went wrong.
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    // Polls events and starts an ImGui frame. Returns false once the window
    // should close.
    bool begin_frame();

    // Renders the frame and swaps buffers.
    void end_frame();

    GLFWwindow* window() const { return window_; }

    // Background clear colour, matching the theme's window background so there
    // is no flash of a different colour during resize.
    void set_clear_color(float r, float g, float b);

private:
    GLFWwindow* window_ = nullptr;
    bool        ok_     = false;
    std::string error_;
    float       clear_[3]{0.06f, 0.07f, 0.09f};
};

}  // namespace elanora
