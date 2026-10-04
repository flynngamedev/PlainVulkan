// window.cpp -- Pv::Init / Pv::CreateWindow / window management. This is
// where GLFW gets initialized and, once a window+surface exist, where the
// full Vulkan bootstrap (vk_core.cpp's vkInitAfterWindow) is triggered.
#include "pv/pv_internal.h"
#include "pv/pv_net.h"
#include "pv/pv_runtime.h"

#include <cstdlib>

namespace pv {
namespace rt {

Value Init() {
    // GUI-subsystem games have no stdout. Attach to the parent terminal so
    // `pv run` can show [PlainVulkan] logs; only allocate a new console when
    // PV_CONSOLE=1 (so double-clicking a .exe still doesn't flash one).
    const char* pvConsole = std::getenv("PV_CONSOLE");
    bool alloc = pvConsole && pvConsole[0] == '1';
    ensureConsole(alloc);

    if (!glfwInit()) {
        logLine("ERROR", "glfwInit() failed -- is a display available? (DISPLAY/WAYLAND_DISPLAY unset?)");
        return Value(false);
    }
    if (!glfwVulkanSupported()) {
        logLine("ERROR", "Vulkan is not available on this system (no loader/driver found)");
        return Value(false);
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // we drive Vulkan ourselves, not GLFW's GL context
    return Value(true);
}

Value Shutdown() {
    // Before the graphics teardown: the network thread is joined here so it
    // cannot still be queuing callbacks (which run script code, which can
    // touch the scene) while the renderer is being destroyed underneath it.
    net::shutdownTransport();
    shutdownAudio();
    vkShutdown();
    return Value();
}

static void framebufferResizeCallback(GLFWwindow*, int, int) { vkRequestSwapchainRecreate(); }

static void scrollCallback(GLFWwindow*, double, double yoffset) { engine().scrollAccum += yoffset; }

static void keyCallback(GLFWwindow*, int key, int, int action, int) {
    if (action == GLFW_PRESS) engine().lastKeyPressed = key;
}

static void charCallback(GLFWwindow*, unsigned int codepoint) {
    if (codepoint < 128) engine().uiCharsTypedThisFrame += static_cast<char>(codepoint);
}

Value CreateWindow(const Value& width, const Value& height, const Value& title) {
    auto& e = engine();
    e.windowWidth = static_cast<int>(width.asInt());
    e.windowHeight = static_cast<int>(height.asInt());
    e.windowedW = e.windowWidth;
    e.windowedH = e.windowHeight;
    e.window = glfwCreateWindow(e.windowWidth, e.windowHeight, title.asString().c_str(), nullptr, nullptr);
    if (!e.window) {
        logLine("ERROR", "glfwCreateWindow failed");
        return Value(false);
    }
    glfwSetFramebufferSizeCallback(e.window, framebufferResizeCallback);
    glfwSetScrollCallback(e.window, scrollCallback);
    glfwSetKeyCallback(e.window, keyCallback);
    glfwSetCharCallback(e.window, charCallback);
    try {
        vkInitAfterWindow(e.vsyncEnabled);
    } catch (const std::exception& ex) {
        logLine("ERROR", std::string("Vulkan initialization failed: ") + ex.what());
        return Value(false);
    }
    logLine("INFO", "window created (" + std::to_string(e.windowWidth) + "x" + std::to_string(e.windowHeight) + ")");
    return Value(true);
}

Value CloseWindow() {
    if (engine().window) glfwSetWindowShouldClose(engine().window, GLFW_TRUE);
    return Value();
}

Value SetWindowTitle(const Value& title) {
    if (engine().window) glfwSetWindowTitle(engine().window, title.asString().c_str());
    return Value();
}

Value SetWindowSize(const Value& width, const Value& height) {
    auto& e = engine();
    e.windowWidth = static_cast<int>(width.asInt());
    e.windowHeight = static_cast<int>(height.asInt());
    if (e.window) glfwSetWindowSize(e.window, e.windowWidth, e.windowHeight);
    return Value();
}

Value IsWindowOpen() {
    auto& e = engine();
    if (!e.window) return Value(false);
    glfwPollEvents();
    return Value(!glfwWindowShouldClose(e.window));
}

Value SetVSync(const Value& enabled) {
    auto& e = engine();
    bool want = enabled.truthy();
    if (want != e.vsyncEnabled) {
        e.vsyncEnabled = want;
        vkRequestSwapchainRecreate();
    }
    return Value();
}

Value SetFullscreen(const Value& enabled) {
    auto& e = engine();
    if (!e.window) return Value();
    bool want = enabled.truthy();
    if (want == e.isFullscreen) return Value();
    if (want) {
        glfwGetWindowPos(e.window, &e.windowedX, &e.windowedY);
        glfwGetWindowSize(e.window, &e.windowedW, &e.windowedH);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(e.window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(e.window, nullptr, e.windowedX, e.windowedY, e.windowedW, e.windowedH, 0);
    }
    e.isFullscreen = want;
    return Value();
}

} // namespace rt
} // namespace pv
