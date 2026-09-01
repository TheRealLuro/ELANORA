#include "elanora/ui_shell.hpp"

#include "elanora/theme.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>

namespace elanora {

namespace {

// GLFW reports errors through a global callback rather than return codes, so
// the most recent message is stashed here for UiShell to report.
std::string g_last_glfw_error;

void glfw_error_callback(int code, const char* description) {
    g_last_glfw_error = "GLFW error " + std::to_string(code) + ": " +
                        (description ? description : "(no description)");
}

}  // namespace

UiShell::UiShell(const char* title, int width, int height) {
    glfwSetErrorCallback(glfw_error_callback);

    if (!glfwInit()) {
        error_ = g_last_glfw_error.empty() ? "glfwInit failed" : g_last_glfw_error;
        return;
    }

    // GL 3.0 + GLSL 130 is the floor the ImGui OpenGL3 backend targets and is
    // available on essentially any machine that can run this at all.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window_) {
        error_ = g_last_glfw_error.empty() ? "glfwCreateWindow failed" : g_last_glfw_error;
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);  // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    apply_elanora_theme();

    // Before backend init so the atlas is built once. Without this the app
    // renders in ProggyClean, the 13px bitmap face ImGui ships with, which is
    // the single biggest reason an ImGui program reads as a debug overlay.
    load_fonts();

    if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
        error_ = "ImGui_ImplGlfw_InitForOpenGL failed";
        return;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        error_ = "ImGui_ImplOpenGL3_Init failed";
        return;
    }

    ok_ = true;
}

UiShell::~UiShell() {
    // Guarded because the constructor can fail partway through; tearing down
    // a backend that never initialised would crash on exit.
    if (ok_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }
    if (ImGui::GetCurrentContext()) {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

void UiShell::set_clear_color(float r, float g, float b) {
    clear_[0] = r;
    clear_[1] = g;
    clear_[2] = b;
}

bool UiShell::begin_frame() {
    if (!ok_ || glfwWindowShouldClose(window_)) return false;

    glfwPollEvents();

    // Windows minimises to a zero-size framebuffer. Idle briefly rather than
    // spinning at full rate -- but still begin the frame afterwards. Returning
    // early here without NewFrame() would leave the caller's ImGui calls and
    // end_frame()'s Render() running outside a frame, which crashes.
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w == 0 || h == 0) {
        glfwWaitEventsTimeout(0.1);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    return true;
}

void UiShell::end_frame() {
    ImGui::Render();

    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(clear_[0], clear_[1], clear_[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
}

}  // namespace elanora
