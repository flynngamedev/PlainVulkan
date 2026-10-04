// vk_core.cpp -- Vulkan instance/device/swapchain bootstrap, the render
// pass and pipelines, and the low-level helpers (buffer creation, one-shot
// command buffers, immediate 2D vertex submission) that the rest of the
// runtime is built on. See pv_internal.h for the shared Engine struct and
// runtime/README.md for the overall architecture/tier notes.
#include "pv/pv_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

// Per-OS bits used only by exeDirectory()/ensureConsole() below. Everything
// else in this file is platform-neutral because pv_platform.h (pulled in by
// pv_internal.h) has already handled the windows.h macro hazards.
#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <climits>
#  include <unistd.h>
#else
#  include <unistd.h>
#  include <limits.h>
#endif

namespace pv {

Engine& engine() {
    static Engine e;
    return e;
}

bool requireInitialized(const char* fnName) {
    if (engine().initialized) return true;
    logLine("ERROR", std::string(fnName) +
                          ": called before Pv::CreateWindow succeeded (or after it failed) -- "
                          "check Pv::CreateWindow's return value before using any other Pv:: rendering "
                          "call. Returning a null/invalid result instead of touching the GPU.");
    return false;
}

// Math (Mat4/Quat/Vec3) used to live here. It moved to pv_math.cpp when
// quaternions, matrix inversion, and TRS decomposition were added for the
// animation, physics, and editor work -- see pv_math.h for the conventions.

// ======================================================================
// Small utilities
// ======================================================================
void logLine(const char* level, const std::string& msg) {
    std::ostream& os = (std::string(level) == "ERROR") ? std::cerr : std::cout;
    os << "[PlainVulkan][" << level << "] " << msg << std::endl;
}

#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need, nullptr, nullptr);
    return out;
}
#endif

void ensureConsole(bool allocIfNoParent) {
#if defined(_WIN32)
    // A WIN32 (GUI subsystem) binary starts with no stdout at all, so every
    // Pv::Log would vanish. AttachConsole picks up a parent terminal when
    // the game was launched from one (`pv run` from PowerShell); AllocConsole
    // makes a fresh one only when asked (PV_CONSOLE=1, or the editor).
    static bool done = false;
    if (done) return;
    done = true;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!allocIfNoParent || !AllocConsole()) return;
    }
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    std::ios::sync_with_stdio(true);
    // UTF-8 console output, so log messages containing non-ASCII asset
    // paths render correctly instead of as mojibake.
    SetConsoleOutputCP(CP_UTF8);
#else
    (void)allocIfNoParent;
#endif
}

std::string exeDirectory() {
#if defined(_WIN32)
    // GetModuleFileNameW rather than the "A" variant: a user profile path
    // can contain characters outside the active ANSI code page, and the A
    // variant would mangle them into an unopenable path.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) break;
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        // Truncated (ERROR_INSUFFICIENT_BUFFER): grow and retry. Long paths
        // can exceed MAX_PATH when the registry long-path opt-in is set.
        if (buf.size() > 32768) break;
        buf.resize(buf.size() * 2);
    }
    std::string p = normalizeSlashes(wideToUtf8(buf));
    auto pos = p.find_last_of('/');
    if (pos != std::string::npos) return p.substr(0, pos);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // first call just reports the needed size
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        buf.resize(std::strlen(buf.c_str()));
        auto pos = buf.find_last_of('/');
        if (pos != std::string::npos) return buf.substr(0, pos);
    }
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1) {
        buf[len] = '\0';
        std::string p(buf);
        auto pos = p.find_last_of('/');
        if (pos != std::string::npos) return p.substr(0, pos);
    }
#endif
    return "."; // portable fallback: relies on cwd (documented in runtime/README.md)
}

std::vector<uint32_t> readSpirvFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("could not open SPIR-V file: " + path);
    }
    size_t size = static_cast<size_t>(f.tellg());
    std::vector<uint32_t> buf(size / sizeof(uint32_t));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    return buf;
}

static VkShaderModule loadShaderModule(const std::string& filename) {
    auto code = readSpirvFile(exeDirectory() + "/shaders/" + filename);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule mod;
    if (vkCreateShaderModule(engine().device, &ci, nullptr, &mod) != VK_SUCCESS) {
        throw std::runtime_error("failed to create shader module: " + filename);
    }
    return mod;
}

// ======================================================================
// Buffers / memory
// ======================================================================
uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(engine().physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    throw std::runtime_error("no suitable Vulkan memory type found");
}

void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                   VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(engine().device, &bi, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("vkCreateBuffer failed");
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(engine().device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    if (vkAllocateMemory(engine().device, &ai, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("vkAllocateMemory failed");
    }
    vkBindBufferMemory(engine().device, buffer, memory, 0);
}

VkCommandBuffer beginOneShotCommands() {
    if (engine().commandPool == VK_NULL_HANDLE) {
        throw std::runtime_error("beginOneShotCommands: command pool not created yet");
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandPool = engine().commandPool;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(engine().device, &ai, &cmd) != VK_SUCCESS || cmd == VK_NULL_HANDLE) {
        throw std::runtime_error("vkAllocateCommandBuffers failed");
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}
void endOneShotCommands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(engine().graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(engine().graphicsQueue);
    vkFreeCommandBuffers(engine().device, engine().commandPool, 1, &cmd);
}

void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    VkCommandBuffer cmd = beginOneShotCommands();
    VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    endOneShotCommands(cmd);
}

// ======================================================================
// Instance / device
// ======================================================================
static const bool kEnableValidation =
#if defined(NDEBUG)
    false;
#else
    true;
#endif

static bool checkValidationLayerSupport() {
    uint32_t count;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (auto& l : layers) {
        if (std::string(l.layerName) == "VK_LAYER_KHRONOS_validation") return true;
    }
    return false;
}

static void createInstance() {
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "PlainVulkan Game";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "PlainVulkan";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);

    bool useValidation = kEnableValidation && checkValidationLayerSupport();

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    static const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    if (useValidation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &validationLayer;
    }
    if (vkCreateInstance(&ci, nullptr, &engine().instance) != VK_SUCCESS) {
        throw std::runtime_error("vkCreateInstance failed");
    }
}

struct QueueFamilies {
    std::optional<uint32_t> graphics, present;
    bool complete() const { return graphics.has_value() && present.has_value(); }
};

static QueueFamilies findQueueFamilies(VkPhysicalDevice dev) {
    QueueFamilies qf;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, props.data());
    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) qf.graphics = i;
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, engine().surface, &presentSupport);
        if (presentSupport) qf.present = i;
        if (qf.complete()) break;
    }
    return qf;
}

static bool deviceHasSwapchainSupport(VkPhysicalDevice dev) {
    uint32_t count;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> avail(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, avail.data());
    for (auto& e : avail) {
        if (std::string(e.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) return true;
    }
    return false;
}

static void pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(engine().instance, &count, nullptr);
    if (count == 0) throw std::runtime_error("no Vulkan-capable GPU found");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(engine().instance, &count, devices.data());

    for (auto dev : devices) {
        if (!deviceHasSwapchainSupport(dev)) continue;
        auto qf = findQueueFamilies(dev);
        if (!qf.complete()) continue;
        uint32_t formatCount, presentModeCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(dev, engine().surface, &formatCount, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev, engine().surface, &presentModeCount, nullptr);
        if (formatCount == 0 || presentModeCount == 0) continue;
        engine().physicalDevice = dev;
        engine().graphicsFamily = *qf.graphics;
        engine().presentFamily = *qf.present;
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(dev, &p);
        logLine("INFO", std::string("selected GPU: ") + p.deviceName);
        return;
    }
    throw std::runtime_error("no suitable Vulkan-capable GPU found (needs graphics+present queues and swapchain support)");
}

static void createLogicalDevice() {
    std::set<uint32_t> uniqueFamilies = {engine().graphicsFamily, engine().presentFamily};
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    float priority = 1.0f;
    for (uint32_t fam : uniqueFamilies) {
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = fam;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queueInfos.push_back(qi);
    }

    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceFeatures avail{};
    vkGetPhysicalDeviceFeatures(engine().physicalDevice, &avail);
    features.fillModeNonSolid = avail.fillModeNonSolid; // for SetWireframe
    features.samplerAnisotropy = avail.samplerAnisotropy;

    static const char* swapchainExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    ci.pQueueCreateInfos = queueInfos.data();
    ci.pEnabledFeatures = &features;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = &swapchainExt;

    if (vkCreateDevice(engine().physicalDevice, &ci, nullptr, &engine().device) != VK_SUCCESS) {
        throw std::runtime_error("vkCreateDevice failed");
    }
    vkGetDeviceQueue(engine().device, engine().graphicsFamily, 0, &engine().graphicsQueue);
    vkGetDeviceQueue(engine().device, engine().presentFamily, 0, &engine().presentQueue);
}

// ======================================================================
// Swapchain / render pass / depth / framebuffers
// ======================================================================
static VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    if (formats.empty()) {
        throw std::runtime_error("surface has no color formats");
    }
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) return f;
    }
    return formats[0];
}
static VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync) {
    if (!vsync) {
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
        }
    }
    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR; // always supported
}

static VkFormat findDepthFormat() {
    const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                   VK_FORMAT_D24_UNORM_S8_UINT};
    for (VkFormat f : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(engine().physicalDevice, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return f;
    }
    return VK_FORMAT_D32_SFLOAT;
}

static void createDepthResources() {
    auto& e = engine();
    e.depthFormat = findDepthFormat();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.extent = {e.swapchainExtent.width, e.swapchainExtent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.format = e.depthFormat;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(e.device, &ci, nullptr, &e.depthImage);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(e.device, e.depthImage, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(e.device, &ai, nullptr, &e.depthMemory);
    vkBindImageMemory(e.device, e.depthImage, e.depthMemory, 0);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = e.depthImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = e.depthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(e.device, &vi, nullptr, &e.depthView);
}

static void createRenderPass() {
    auto& e = engine();
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depth{};
    depth.format = e.depthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments{color, depth};
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = static_cast<uint32_t>(attachments.size());
    ci.pAttachments = attachments.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    if (vkCreateRenderPass(e.device, &ci, nullptr, &e.renderPass) != VK_SUCCESS) {
        throw std::runtime_error("vkCreateRenderPass failed");
    }
}

static void createSwapchain(bool vsync) {
    auto& e = engine();
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(e.physicalDevice, e.surface, &caps);
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(e.physicalDevice, e.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(e.physicalDevice, e.surface, &formatCount, formats.data());
    uint32_t presentCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(e.physicalDevice, e.surface, &presentCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(e.physicalDevice, e.surface, &presentCount, presentModes.data());

    VkSurfaceFormatKHR format = chooseSurfaceFormat(formats);
    VkPresentModeKHR presentMode = choosePresentMode(presentModes, vsync);

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(e.window, &fbW, &fbH);
    while (e.window && (fbW == 0 || fbH == 0)) {
        glfwWaitEvents();
        glfwGetFramebufferSize(e.window, &fbW, &fbH);
    }
    VkExtent2D extent;
    // Intel drivers sometimes report currentExtent {0,0} instead of UINT32_MAX
    // before the HWND is fully sized -- creating a 0x0 swapchain AVs there.
    if (caps.currentExtent.width != UINT32_MAX && caps.currentExtent.width != 0 &&
        caps.currentExtent.height != 0) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(static_cast<uint32_t>(std::max(fbW, 1)), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(std::max(fbH, 1)), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = e.surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t families[] = {e.graphicsFamily, e.presentFamily};
    if (e.graphicsFamily != e.presentFamily) {
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = families;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    {
        const VkCompositeAlphaFlagBitsKHR alphaModes[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (auto mode : alphaModes) {
            if (caps.supportedCompositeAlpha & mode) {
                ci.compositeAlpha = mode;
                break;
            }
        }
    }
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(e.device, &ci, nullptr, &e.swapchain) != VK_SUCCESS) {
        throw std::runtime_error("vkCreateSwapchainKHR failed");
    }
    e.swapchainFormat = format.format;
    e.swapchainExtent = extent;

    uint32_t actualCount;
    vkGetSwapchainImagesKHR(e.device, e.swapchain, &actualCount, nullptr);
    e.swapchainImages.resize(actualCount);
    vkGetSwapchainImagesKHR(e.device, e.swapchain, &actualCount, e.swapchainImages.data());

    e.swapchainImageViews.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = e.swapchainImages[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = e.swapchainFormat;
        vi.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(e.device, &vi, nullptr, &e.swapchainImageViews[i]);
    }

    createDepthResources();
    // Recreating the render pass on every resize made existing pipelines
    // target a destroyed VkRenderPass. Some drivers tolerate "compatible"
    // replacements; others present a blank swapchain and then TDR.
    if (e.renderPass == VK_NULL_HANDLE) createRenderPass();

    if (e.swapchainRenderPass == VK_NULL_HANDLE) {
        VkAttachmentDescription swapColor{};
        swapColor.format = e.swapchainFormat;
        swapColor.samples = VK_SAMPLE_COUNT_1_BIT;
        swapColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        swapColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        swapColor.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        swapColor.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference swapRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &swapRef;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments = &swapColor;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        vkCreateRenderPass(e.device, &rpci, nullptr, &e.swapchainRenderPass);
    }

    e.swapchainFramebuffers.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; i++) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = e.swapchainRenderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &e.swapchainImageViews[i];
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        vkCreateFramebuffer(e.device, &fi, nullptr, &e.swapchainFramebuffers[i]);
    }
    e.imagesInFlight.assign(actualCount, VK_NULL_HANDLE);
    if (e.swapchainRenderPass) recreatePostExtentResources();
}

static void cleanupSwapchain() {
    auto& e = engine();
    if (e.sceneFramebuffer) {
        vkDestroyFramebuffer(e.device, e.sceneFramebuffer, nullptr);
        e.sceneFramebuffer = VK_NULL_HANDLE;
    }
    if (e.bloomFramebuffer) {
        vkDestroyFramebuffer(e.device, e.bloomFramebuffer, nullptr);
        e.bloomFramebuffer = VK_NULL_HANDLE;
    }
    auto killImg = [&](GpuImage& img) {
        if (img.view) vkDestroyImageView(e.device, img.view, nullptr);
        if (img.image) vkDestroyImage(e.device, img.image, nullptr);
        if (img.memory) vkFreeMemory(e.device, img.memory, nullptr);
        img = {};
    };
    killImg(e.hdrColor);
    killImg(e.bloomColor);
    killImg(e.historyColor);
    for (auto fb : e.swapchainFramebuffers) vkDestroyFramebuffer(e.device, fb, nullptr);
    e.swapchainFramebuffers.clear();
    e.imagesInFlight.clear();
    if (e.depthView) vkDestroyImageView(e.device, e.depthView, nullptr);
    if (e.depthImage) vkDestroyImage(e.device, e.depthImage, nullptr);
    if (e.depthMemory) vkFreeMemory(e.device, e.depthMemory, nullptr);
    e.depthView = VK_NULL_HANDLE;
    e.depthImage = VK_NULL_HANDLE;
    e.depthMemory = VK_NULL_HANDLE;
    for (auto v : e.swapchainImageViews) vkDestroyImageView(e.device, v, nullptr);
    e.swapchainImageViews.clear();
    if (e.swapchain) vkDestroySwapchainKHR(e.device, e.swapchain, nullptr);
    e.swapchain = VK_NULL_HANDLE;
}

static void recreateSwapchain() {
    auto& e = engine();
    int w = 0, h = 0;
    glfwGetFramebufferSize(e.window, &w, &h);
    while (w == 0 || h == 0) { // minimized; wait
        glfwGetFramebufferSize(e.window, &w, &h);
        glfwWaitEvents();
    }
    vkDeviceWaitIdle(e.device);
    cleanupSwapchain();
    createSwapchain(e.vsyncEnabled);
    e.swapchainOutOfDate = false;
}

// ======================================================================
// Descriptor set layouts / pool / blank texture
// ======================================================================
static void createDescriptorInfrastructure() {
    auto& e = engine();

    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo samplerLayoutCi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    samplerLayoutCi.bindingCount = 1;
    samplerLayoutCi.pBindings = &samplerBinding;
    vkCreateDescriptorSetLayout(e.device, &samplerLayoutCi, nullptr, &e.samplerSetLayout);

    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding = 1;
    shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = 1;
    shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding uboBinds[] = {uboBinding, shadowBinding};
    VkDescriptorSetLayoutCreateInfo uboLayoutCi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    uboLayoutCi.bindingCount = 2;
    uboLayoutCi.pBindings = uboBinds;
    vkCreateDescriptorSetLayout(e.device, &uboLayoutCi, nullptr, &e.lightsSetLayout);

    VkDescriptorSetLayoutBinding matBinds[3]{};
    for (int i = 0; i < 3; ++i) {
        matBinds[i].binding = static_cast<uint32_t>(i);
        matBinds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        matBinds[i].descriptorCount = 1;
        matBinds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo matLayoutCi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    matLayoutCi.bindingCount = 3;
    matLayoutCi.pBindings = matBinds;
    vkCreateDescriptorSetLayout(e.device, &matLayoutCi, nullptr, &e.materialSetLayout);

    std::array<VkDescriptorPoolSize, 2> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
    };
    VkDescriptorPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCi.maxSets = 1024;
    poolCi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolCi.pPoolSizes = sizes.data();
    vkCreateDescriptorPool(e.device, &poolCi, nullptr, &e.descriptorPool);

    createBuffer(sizeof(SceneUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, e.lightsUbo,
                 e.lightsUboMemory);
    vkMapMemory(e.device, e.lightsUboMemory, 0, VK_WHOLE_SIZE, 0, &e.lightsUboMapped);

    VkSamplerCreateInfo lin{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    lin.magFilter = lin.minFilter = VK_FILTER_LINEAR;
    lin.addressModeU = lin.addressModeV = lin.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lin.maxLod = 0.0f;
    vkCreateSampler(e.device, &lin, nullptr, &e.linearSampler);

    VkSamplerCreateInfo shs{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    shs.magFilter = shs.minFilter = VK_FILTER_LINEAR;
    shs.addressModeU = shs.addressModeV = shs.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shs.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    shs.compareEnable = VK_TRUE;
    shs.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(e.device, &shs, nullptr, &e.shadowSampler);

    VkDescriptorSetAllocateInfo dsAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsAi.descriptorPool = e.descriptorPool;
    dsAi.descriptorSetCount = 1;
    dsAi.pSetLayouts = &e.lightsSetLayout;
    vkAllocateDescriptorSets(e.device, &dsAi, &e.lightsDescriptorSet);

    VkDescriptorBufferInfo bufInfo{e.lightsUbo, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = e.lightsDescriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(e.device, 1, &write, 0, nullptr);
}

VkDescriptorSet allocSamplerDescriptorSet(VkImageView view, VkSampler sampler) {
    auto& e = engine();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = e.descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &e.samplerSetLayout;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(e.device, &ai, &set) != VK_SUCCESS) {
        throw std::runtime_error("vkAllocateDescriptorSets failed (descriptor pool exhausted?)");
    }
    VkDescriptorImageInfo imgInfo{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(e.device, 1, &write, 0, nullptr);
    return set;
}

VkDescriptorSet allocMaterialDescriptorSet(VkImageView albedo, VkSampler albedoSamp, VkImageView normal,
                                           VkSampler normalSamp, VkImageView height, VkSampler heightSamp) {
    auto& e = engine();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = e.descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &e.materialSetLayout;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(e.device, &ai, &set) != VK_SUCCESS) {
        throw std::runtime_error("vkAllocateDescriptorSets failed (material set)");
    }
    VkDescriptorImageInfo imgs[3] = {
        {albedoSamp, albedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {normalSamp, normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {heightSamp, height, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imgs[i];
    }
    vkUpdateDescriptorSets(e.device, 3, writes, 0, nullptr);
    return set;
}

static void createBlankTexture() {
    auto& e = engine();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.extent = {1, 1, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(e.device, &ci, nullptr, &e.blankImage);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(e.device, e.blankImage, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(e.device, &ai, nullptr, &e.blankImageMemory);
    vkBindImageMemory(e.device, e.blankImage, e.blankImageMemory, 0);

    VkBuffer staging;
    VkDeviceMemory stagingMem;
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMem);
    void* mapped;
    vkMapMemory(e.device, stagingMem, 0, 4, 0, &mapped);
    memcpy(mapped, whitePixel, 4);
    vkUnmapMemory(e.device, stagingMem);

    VkCommandBuffer cmd = beginOneShotCommands();
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = e.blankImage;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyBufferToImage(cmd, staging, e.blankImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader = toDst;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &toShader);
    endOneShotCommands(cmd);
    vkDestroyBuffer(e.device, staging, nullptr);
    vkFreeMemory(e.device, stagingMem, nullptr);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = e.blankImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(e.device, &vi, nullptr, &e.blankImageView);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 0.0f;
    vkCreateSampler(e.device, &si, nullptr, &e.blankSampler);

    e.blankDescriptorSet = allocSamplerDescriptorSet(e.blankImageView, e.blankSampler);
    e.blankMaterialSet = allocMaterialDescriptorSet(e.blankImageView, e.blankSampler, e.blankImageView,
                                                    e.blankSampler, e.blankImageView, e.blankSampler);
}

// ======================================================================
// Pipelines
// ======================================================================
static VkPipelineShaderStageCreateInfo stageInfo(VkShaderStageFlagBits stage, VkShaderModule mod) {
    VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    si.stage = stage;
    si.module = mod;
    si.pName = "main";
    return si;
}

static void create2DPipelines() {
    auto& e = engine();
    VkShaderModule vert = loadShaderModule("prim2d.vert.spv");
    VkShaderModule fragFlat = loadShaderModule("prim2d.frag.spv");
    VkShaderModule fragSprite = loadShaderModule("sprite.frag.spv");

    VkVertexInputBindingDescription binding{0, sizeof(Vertex2D), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 3> attrs{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex2D, pos)},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex2D, color)},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex2D, uv)},
    };
    // Both pipeline2D and pipelineSprite share this exact vertex format
    // (and the same underlying VBO/binding) -- flat-color draws just leave
    // `uv` as {0,0}, which the flat fragment shader never reads.
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttach.blendEnable = VK_TRUE;
    blendAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttach.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttach.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttach;

    std::array<VkDynamicState, 2> dynStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dyn.pDynamicStates = dynStates.data();

    VkPushConstantRange pcRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2};

    VkPipelineLayoutCreateInfo flatLayoutCi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    flatLayoutCi.pushConstantRangeCount = 1;
    flatLayoutCi.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(e.device, &flatLayoutCi, nullptr, &e.pipeline2DLayout);

    VkPipelineLayoutCreateInfo spriteLayoutCi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    spriteLayoutCi.pushConstantRangeCount = 1;
    spriteLayoutCi.pPushConstantRanges = &pcRange;
    spriteLayoutCi.setLayoutCount = 1;
    spriteLayoutCi.pSetLayouts = &e.samplerSetLayout;
    vkCreatePipelineLayout(e.device, &spriteLayoutCi, nullptr, &e.pipelineSpriteLayout);

    VkPipelineDepthStencilStateCreateInfo noDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    noDepth.depthTestEnable = VK_FALSE;
    noDepth.depthWriteEnable = VK_FALSE;

    auto makePipeline = [&](VkShaderModule frag, VkPipelineLayout layout) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{stageInfo(VK_SHADER_STAGE_VERTEX_BIT, vert),
                                                                stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT, frag)};
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount = 2;
        pci.pStages = stages.data();
        pci.pVertexInputState = &vertexInput;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rast;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &noDepth;
        pci.pColorBlendState = &blend;
        pci.pDynamicState = &dyn;
        pci.layout = layout;
        pci.renderPass = e.renderPass;
        pci.subpass = 0;
        VkPipeline pipeline;
        if (vkCreateGraphicsPipelines(e.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create 2D pipeline");
        }
        return pipeline;
    };

    e.pipeline2D = makePipeline(fragFlat, e.pipeline2DLayout);
    e.pipelineSprite = makePipeline(fragSprite, e.pipelineSpriteLayout);

    vkDestroyShaderModule(e.device, vert, nullptr);
    vkDestroyShaderModule(e.device, fragFlat, nullptr);
    vkDestroyShaderModule(e.device, fragSprite, nullptr);
}

static void create3DPipelines() {
    auto& e = engine();
    VkShaderModule vert = loadShaderModule("mesh3d.vert.spv");
    VkShaderModule frag = loadShaderModule("mesh3d.frag.spv");

    VkVertexInputBindingDescription binding{0, sizeof(Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 3> attrs{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, pos)},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, normal)},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex3D, uv)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_BACK_BIT;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttach;

    std::array<VkDynamicState, 2> dynStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dyn.pDynamicStates = dynStates.data();

    VkPushConstantRange pcRange{static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
                                0, sizeof(float) * 24};
    std::array<VkDescriptorSetLayout, 2> setLayouts{e.lightsSetLayout, e.materialSetLayout};
    VkPipelineLayoutCreateInfo layoutCi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutCi.pushConstantRangeCount = 1;
    layoutCi.pPushConstantRanges = &pcRange;
    layoutCi.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutCi.pSetLayouts = setLayouts.data();
    vkCreatePipelineLayout(e.device, &layoutCi, nullptr, &e.pipeline3DLayout);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{stageInfo(VK_SHADER_STAGE_VERTEX_BIT, vert),
                                                            stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT, frag)};
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages.data();
    pci.pVertexInputState = &vertexInput;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rast;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &depth;
    pci.pColorBlendState = &blend;
    pci.pDynamicState = &dyn;
    pci.layout = e.pipeline3DLayout;
    pci.renderPass = e.renderPass;
    pci.subpass = 0;
    if (vkCreateGraphicsPipelines(e.device, VK_NULL_HANDLE, 1, &pci, nullptr, &e.pipeline3D) != VK_SUCCESS) {
        throw std::runtime_error("failed to create 3D pipeline");
    }

    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(e.physicalDevice, &feats);
    if (feats.fillModeNonSolid) {
        rast.polygonMode = VK_POLYGON_MODE_LINE;
        rast.cullMode = VK_CULL_MODE_NONE;
        if (vkCreateGraphicsPipelines(e.device, VK_NULL_HANDLE, 1, &pci, nullptr, &e.pipeline3DWireframe) !=
            VK_SUCCESS) {
            e.pipeline3DWireframe = e.pipeline3D;
        }
    } else {
        logLine("WARNING", "GPU doesn't support fillModeNonSolid; Pv::SetWireframe will have no visible effect");
        e.pipeline3DWireframe = e.pipeline3D;
    }

    vkDestroyShaderModule(e.device, vert, nullptr);
    vkDestroyShaderModule(e.device, frag, nullptr);
}

// ======================================================================
// Frame lifecycle helpers used by window.cpp/frame.cpp
// ======================================================================
static void createParticlePipelines() {
    auto& e = engine();
    VkShaderModule vert = loadShaderModule("particle.vert.spv");
    VkShaderModule frag = loadShaderModule("particle.frag.spv");

    VkVertexInputBindingDescription binding{0, sizeof(ParticleVertexLayout), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 3> attrs{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleVertexLayout, pos)},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleVertexLayout, color)},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ParticleVertexLayout, uv)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    // Particle quads are built CPU-side from a camera basis, so their
    // winding flips depending on which side of the camera they land. Culling
    // would make half of every emitter vanish.
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth *test* on so particles are correctly occluded by solid geometry;
    // depth *write* off so they never occlude each other or the particles
    // behind them. Writing depth from transparent geometry is the classic
    // cause of "the far half of my smoke plume disappeared".
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    std::array<VkDynamicState, 2> dynStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dyn.pDynamicStates = dynStates.data();

    // mat4 viewProj + vec4 params -- see particle.vert.
    VkPushConstantRange pcRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 20};
    VkPipelineLayoutCreateInfo layoutCi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutCi.pushConstantRangeCount = 1;
    layoutCi.pPushConstantRanges = &pcRange;
    layoutCi.setLayoutCount = 1;
    layoutCi.pSetLayouts = &e.samplerSetLayout;
    vkCreatePipelineLayout(e.device, &layoutCi, nullptr, &e.pipelineParticleLayout);

    auto makeVariant = [&](bool additive) {
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT;
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        // The only difference between the two variants: additive accumulates
        // into the framebuffer (order-independent, which is why particles.cpp
        // skips the depth sort for it), alpha replaces proportionally.
        ba.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &ba;

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{stageInfo(VK_SHADER_STAGE_VERTEX_BIT, vert),
                                                              stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT, frag)};
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount = 2;
        pci.pStages = stages.data();
        pci.pVertexInputState = &vertexInput;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rast;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &depth;
        pci.pColorBlendState = &blend;
        pci.pDynamicState = &dyn;
        pci.layout = e.pipelineParticleLayout;
        pci.renderPass = e.renderPass;
        pci.subpass = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(e.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create particle pipeline");
        }
        return pipeline;
    };

    e.pipelineParticleAlpha = makeVariant(false);
    e.pipelineParticleAdditive = makeVariant(true);

    vkDestroyShaderModule(e.device, vert, nullptr);
    vkDestroyShaderModule(e.device, frag, nullptr);
}

void drawParticleVerts(const void* verts, uint32_t count, const Mat4& viewProj, uint64_t textureHandle,
                       bool additive) {
    auto& e = engine();
    if (!e.frameActive || count == 0) return;
    if (e.pipelineParticleAlpha == VK_NULL_HANDLE) return;

    // The ring buffer is per-frame-in-flight, so wrapping mid-frame would
    // overwrite vertices an already-recorded draw still points at. Dropping
    // the overflow (with one warning) is the safe failure: you lose some
    // particles for a frame instead of getting corrupted geometry.
    if (e.vboParticleCursor + count > Engine::VBO_PARTICLE_CAPACITY) {
        if (!e.warnedParticleOverflow) {
            e.warnedParticleOverflow = true;
            logLine("WARNING",
                    "particle vertex budget exhausted this frame (" +
                        std::to_string(Engine::VBO_PARTICLE_CAPACITY / 6) +
                        " quads); some particles were skipped. Lower emitter rates or maxParticles.");
        }
        return;
    }

    auto* dst = reinterpret_cast<ParticleVertexLayout*>(e.vboParticleMapped[e.currentFrame]) + e.vboParticleCursor;
    memcpy(dst, verts, sizeof(ParticleVertexLayout) * count);

    VkPipeline pipeline = additive ? e.pipelineParticleAdditive : e.pipelineParticleAlpha;
    vkCmdBindPipeline(e.activeCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(e.activeCmd, 0, 1, &e.vboParticle[e.currentFrame], &offset);

    struct {
        float viewProj[16];
        float params[4];
    } push{};
    memcpy(push.viewProj, viewProj.m, sizeof(push.viewProj));
    push.params[0] = 0.0f; // soft-particle fade distance, reserved
    vkCmdPushConstants(e.activeCmd, e.pipelineParticleLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    GpuTexture* tex = e.textures.get(textureHandle);
    VkDescriptorSet set = tex ? tex->descriptorSet : e.blankDescriptorSet;
    vkCmdBindDescriptorSets(e.activeCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e.pipelineParticleLayout, 0, 1, &set, 0,
                            nullptr);

    vkCmdDraw(e.activeCmd, count, 1, e.vboParticleCursor, 0);
    e.vboParticleCursor += count;
}

void draw2DVerts(const Vertex2D* verts, uint32_t count, bool textured, VkDescriptorSet texSet) {
    auto& e = engine();
    if (!e.frameActive || count == 0) return;
    if (e.vbo2DCursor + count > Engine::VBO2D_CAPACITY) {
        // Wrapping mid-frame would overwrite vertices an already-recorded
        // draw still points at (GPU use-after-overwrite). Drop the extra.
        return;
    }
    Vertex2D* dst = reinterpret_cast<Vertex2D*>(e.vbo2DMapped[e.currentFrame]) + e.vbo2DCursor;
    memcpy(dst, verts, sizeof(Vertex2D) * count);

    VkPipeline pipeline = textured ? e.pipelineSprite : e.pipeline2D;
    VkPipelineLayout layout = textured ? e.pipelineSpriteLayout : e.pipeline2DLayout;
    vkCmdBindPipeline(e.activeCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(e.activeCmd, 0, 1, &e.vbo2D[e.currentFrame], &offset);
    float screenSize[2] = {static_cast<float>(e.swapchainExtent.width), static_cast<float>(e.swapchainExtent.height)};
    vkCmdPushConstants(e.activeCmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(screenSize), screenSize);
    if (textured) {
        VkDescriptorSet set = texSet ? texSet : e.blankDescriptorSet;
        vkCmdBindDescriptorSets(e.activeCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    }
    vkCmdDraw(e.activeCmd, count, 1, e.vbo2DCursor, 0);
    e.vbo2DCursor += count;
}

Mat4 buildViewMatrix() {
    auto& e = engine();
    if (e.camHasTarget) {
        return Mat4::lookAt(e.camPos, e.camTarget, {0, 1, 0});
    }
    // No explicit LookAt target: derive a look direction from pitch/yaw
    // set via Pv::RotateCamera.
    Vec3 dir{cosf(e.camPitch) * sinf(e.camYaw), sinf(e.camPitch), cosf(e.camPitch) * cosf(e.camYaw)};
    return Mat4::lookAt(e.camPos, e.camPos + dir, {0, 1, 0});
}

Mat4 buildProjectionMatrix(bool forOrtho2D) {
    auto& e = engine();
    float aspect = e.swapchainExtent.height == 0
                       ? 1.0f
                       : static_cast<float>(e.swapchainExtent.width) / static_cast<float>(e.swapchainExtent.height);
    if (e.orthographic || forOrtho2D) {
        float halfH = 5.0f;
        float halfW = halfH * aspect;
        return Mat4::orthographic(-halfW, halfW, -halfH, halfH, e.nearZ, e.farZ);
    }
    return Mat4::perspective(e.fovDegrees * 3.14159265f / 180.0f, aspect, e.nearZ, e.farZ);
}

void updateLightsUbo() {
    auto& e = engine();
    SceneUbo data{};
    data.ambient[0] = e.ambientR;
    data.ambient[1] = e.ambientG;
    data.ambient[2] = e.ambientB;
    data.ambient[3] = e.ambientIntensity;
    int i = 0;
    Vec3 sunDir{0.35f, -1.0f, 0.25f};
    bool haveSun = false;
    for (auto& kv : e.lights) {
        if (i >= Engine::MAX_LIGHTS) break;
        LightRecord& light = kv.second;
        Vec3 p = (light.kind == LightKind::Directional) ? light.direction : light.position;
        data.posKind[i][0] = p.x;
        data.posKind[i][1] = p.y;
        data.posKind[i][2] = p.z;
        data.posKind[i][3] = (light.kind == LightKind::Directional) ? 1.0f : 0.0f;
        data.colorIntensity[i][0] = light.r;
        data.colorIntensity[i][1] = light.g;
        data.colorIntensity[i][2] = light.b;
        data.colorIntensity[i][3] = light.intensity;
        if (!haveSun && light.kind == LightKind::Directional) {
            sunDir = light.direction.normalized();
            if (sunDir.lengthSq() < 1e-6f) sunDir = Vec3(0.35f, -1.0f, 0.25f).normalized();
            haveSun = true;
        }
        i++;
    }
    data.lightCountPad[0] = static_cast<float>(i);
    data.lightCountPad[1] = e.shadowsEnabled ? 1.0f : 0.0f;
    data.lightCountPad[2] = e.cascadeSplit;
    data.camPos[0] = e.camPos.x;
    data.camPos[1] = e.camPos.y;
    data.camPos[2] = e.camPos.z;
    auto halton = [](int idx, int base) {
        float f = 1.0f, r = 0.0f;
        int n = idx + 1;
        while (n > 0) {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(n % base);
            n /= base;
        }
        return r;
    };
    float jx = (halton(e.taaFrame, 2) * 2.0f - 1.0f) / std::max(1u, e.swapchainExtent.width);
    float jy = (halton(e.taaFrame, 3) * 2.0f - 1.0f) / std::max(1u, e.swapchainExtent.height);
    data.jitterNearFar[0] = jx;
    data.jitterNearFar[1] = jy;
    data.jitterNearFar[2] = e.nearZ;
    data.jitterNearFar[3] = e.farZ;
    e.taaFrame = (e.taaFrame + 1) & 7;

    Mat4 view = buildViewMatrix();
    Mat4 proj = buildProjectionMatrix();
    Mat4 viewProj = proj * view;
    Mat4 invProj = proj.inverse();
    memcpy(data.view, view.m, sizeof(data.view));
    memcpy(data.proj, proj.m, sizeof(data.proj));
    memcpy(data.viewProj, viewProj.m, sizeof(data.viewProj));
    memcpy(data.invProj, invProj.m, sizeof(data.invProj));

    Vec3 up = std::fabs(sunDir.y) > 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    float radii[2] = {e.cascadeSplit, e.cascadeSplit * 4.5f};
    for (int c = 0; c < 2; ++c) {
        float r = radii[c];
        Vec3 eye = e.camPos - sunDir * (r * 2.0f);
        Mat4 lv = Mat4::lookAt(eye, e.camPos, up);
        Mat4 lp = Mat4::orthographic(-r, r, -r, r, 0.1f, r * 6.0f);
        e.lightViewProj[c] = lp * lv;
        memcpy(data.lightVP[c], e.lightViewProj[c].m, sizeof(data.lightVP[c]));
    }

    memcpy(e.lightsUboMapped, &data, sizeof(data));
}

// ======================================================================
// Public lifecycle entry points (called from window.cpp)
// ======================================================================
void vkInitAfterWindow(bool vsync) {
    auto& e = engine();
    createInstance();
    if (glfwCreateWindowSurface(e.instance, e.window, nullptr, &e.surface) != VK_SUCCESS) {
        throw std::runtime_error("glfwCreateWindowSurface failed");
    }
    pickPhysicalDevice();
    createLogicalDevice();

    // Must exist before createBlankTexture / mesh upload: those record into
    // one-shot command buffers. Creating the pool after them AVs on Intel.
    VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCi.queueFamilyIndex = e.graphicsFamily;
    if (vkCreateCommandPool(e.device, &poolCi, nullptr, &e.commandPool) != VK_SUCCESS) {
        throw std::runtime_error("vkCreateCommandPool failed");
    }

    createSwapchain(vsync);
    createDescriptorInfrastructure();
    createBlankTexture();
    create2DPipelines();
    create3DPipelines();
    createParticlePipelines();
    createPostResources();

    e.commandBuffers.resize(Engine::MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAi.commandPool = e.commandPool;
    cbAi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = Engine::MAX_FRAMES_IN_FLIGHT;
    vkAllocateCommandBuffers(e.device, &cbAi, e.commandBuffers.data());

    for (int i = 0; i < Engine::MAX_FRAMES_IN_FLIGHT; i++) {
        VkSemaphoreCreateInfo semCi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fenceCi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceCi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateSemaphore(e.device, &semCi, nullptr, &e.frameSync[i].imageAvailable);
        vkCreateSemaphore(e.device, &semCi, nullptr, &e.frameSync[i].renderFinished);
        vkCreateFence(e.device, &fenceCi, nullptr, &e.frameSync[i].inFlight);

        createBuffer(sizeof(Vertex2D) * Engine::VBO2D_CAPACITY, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, e.vbo2D[i],
                     e.vbo2DMemory[i]);
        vkMapMemory(e.device, e.vbo2DMemory[i], 0, sizeof(Vertex2D) * Engine::VBO2D_CAPACITY, 0, &e.vbo2DMapped[i]);

        const VkDeviceSize particleBytes = sizeof(ParticleVertexLayout) * Engine::VBO_PARTICLE_CAPACITY;
        createBuffer(particleBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, e.vboParticle[i],
                     e.vboParticleMemory[i]);
        vkMapMemory(e.device, e.vboParticleMemory[i], 0, particleBytes, 0, &e.vboParticleMapped[i]);
    }

    e.lastFrameTime = std::chrono::steady_clock::now();
    e.initialized = true;
}

void vkShutdown() {
    auto& e = engine();
    if (!e.initialized) return;
    vkDeviceWaitIdle(e.device);

    for (int i = 0; i < Engine::MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(e.device, e.frameSync[i].imageAvailable, nullptr);
        vkDestroySemaphore(e.device, e.frameSync[i].renderFinished, nullptr);
        vkDestroyFence(e.device, e.frameSync[i].inFlight, nullptr);
        vkUnmapMemory(e.device, e.vbo2DMemory[i]);
        vkDestroyBuffer(e.device, e.vbo2D[i], nullptr);
        vkFreeMemory(e.device, e.vbo2DMemory[i], nullptr);
        vkUnmapMemory(e.device, e.vboParticleMemory[i]);
        vkDestroyBuffer(e.device, e.vboParticle[i], nullptr);
        vkFreeMemory(e.device, e.vboParticleMemory[i], nullptr);
    }
    vkDestroyCommandPool(e.device, e.commandPool, nullptr);

    for (auto& kv : e.meshes) {
        GpuMesh& mesh = kv.second;
        vkDestroyBuffer(e.device, mesh.vertexBuffer, nullptr);
        vkFreeMemory(e.device, mesh.vertexMemory, nullptr);
        if (mesh.indexBuffer) vkDestroyBuffer(e.device, mesh.indexBuffer, nullptr);
        if (mesh.indexMemory) vkFreeMemory(e.device, mesh.indexMemory, nullptr);
    }
    for (auto& kv : e.textures) {
        GpuTexture& tex = kv.second;
        vkDestroySampler(e.device, tex.sampler, nullptr);
        vkDestroyImageView(e.device, tex.view, nullptr);
        vkDestroyImage(e.device, tex.image, nullptr);
        vkFreeMemory(e.device, tex.memory, nullptr);
    }

    vkDestroySampler(e.device, e.blankSampler, nullptr);
    vkDestroyImageView(e.device, e.blankImageView, nullptr);
    vkDestroyImage(e.device, e.blankImage, nullptr);
    vkFreeMemory(e.device, e.blankImageMemory, nullptr);
    vkUnmapMemory(e.device, e.lightsUboMemory);
    vkDestroyBuffer(e.device, e.lightsUbo, nullptr);
    vkFreeMemory(e.device, e.lightsUboMemory, nullptr);

    vkDestroyPipeline(e.device, e.pipeline2D, nullptr);
    vkDestroyPipeline(e.device, e.pipelineSprite, nullptr);
    vkDestroyPipeline(e.device, e.pipeline3D, nullptr);
    if (e.pipeline3DWireframe != e.pipeline3D) vkDestroyPipeline(e.device, e.pipeline3DWireframe, nullptr);
    vkDestroyPipelineLayout(e.device, e.pipeline2DLayout, nullptr);
    vkDestroyPipelineLayout(e.device, e.pipelineSpriteLayout, nullptr);
    vkDestroyPipelineLayout(e.device, e.pipeline3DLayout, nullptr);
    if (e.pipelineParticleAlpha) vkDestroyPipeline(e.device, e.pipelineParticleAlpha, nullptr);
    if (e.pipelineParticleAdditive) vkDestroyPipeline(e.device, e.pipelineParticleAdditive, nullptr);
    vkDestroyPipelineLayout(e.device, e.pipelineParticleLayout, nullptr);
    if (e.materialSetLayout) vkDestroyDescriptorSetLayout(e.device, e.materialSetLayout, nullptr);
    if (e.linearSampler) vkDestroySampler(e.device, e.linearSampler, nullptr);
    if (e.shadowSampler) vkDestroySampler(e.device, e.shadowSampler, nullptr);
    vkDestroyDescriptorSetLayout(e.device, e.samplerSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(e.device, e.lightsSetLayout, nullptr);
    vkDestroyDescriptorPool(e.device, e.descriptorPool, nullptr);

    cleanupSwapchain();
    destroyPostResources();
    if (e.renderPass) vkDestroyRenderPass(e.device, e.renderPass, nullptr);
    e.renderPass = VK_NULL_HANDLE;
    vkDestroyDevice(e.device, nullptr);
    if (e.debugMessenger) {
        auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(e.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (fn) fn(e.instance, e.debugMessenger, nullptr);
    }
    vkDestroySurfaceKHR(e.instance, e.surface, nullptr);
    vkDestroyInstance(e.instance, nullptr);
    if (e.window) glfwDestroyWindow(e.window);
    glfwTerminate();
    e.initialized = false;
}

void vkRequestSwapchainRecreate() { engine().swapchainOutOfDate = true; }
void vkRecreateSwapchainIfNeeded() {
    if (engine().swapchainOutOfDate) recreateSwapchain();
}

} // namespace pv
