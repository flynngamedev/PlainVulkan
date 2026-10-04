// pv_internal.h -- shared engine state used across the runtime's .cpp
// files. NOT part of the public API surface (pv_runtime.h is); this is
// the internals those functions are implemented in terms of.
//
// Scope note (see runtime/README.md for the full tier breakdown): this
// engine is deliberately a *simple, single-window, forward-rendering*
// Vulkan backend -- enough to genuinely run PlainVulkan's 2D/3D drawing,
// input, camera, and basic-lit mesh commands, not a AAA renderer. Things
// like shadow mapping, skyboxes, and render-to-texture are declared in
// pv_runtime.h (so calling them never fails to link) but are stubbed with
// a LogWarning rather than faked.
#pragma once

// pv_platform.h must come first: on Windows it sets NOMINMAX/NOGDI before
// GLFW pulls in windows.h, without which windows.h's min/max macros break
// every std::min in the runtime and its CreateWindow macro would rename
// Pv::CreateWindow out from under us.
#include "pv/pv_platform.h"

// On Windows, <windows.h> has to come first, before glfw3.h -- for two
// separate reasons, both of which were live defects:
//
//  1. APIENTRY. glfw3.h defines APIENTRY as __stdcall when nothing has
//     defined it yet, and (as of GLFW 3.4) never undefines it again. Any
//     TU that later reaches <windows.h> gets minwindef.h's
//     `#define APIENTRY WINAPI` on top of it -- MSVC C4005, and an error
//     under /WX. runtime/src/vk_core.cpp includes <windows.h> directly and
//     hit exactly this.
//
//  2. pv_win32_undef.h below is what stops windows.h's CreateWindow /
//     DrawText / PlaySound / DeleteFile A/W macros from renaming the
//     identically-named Pv:: commands. It can only undo macros that are
//     already defined. The comment here used to claim glfw3.h and Vulkan
//     had "now pulled in <windows.h>" -- but vulkan.h only includes it
//     under VK_USE_PLATFORM_WIN32_KHR, which this build does not define,
//     and GLFW_INCLUDE_NONE keeps glfw3.h from reaching it either. So
//     nothing had included windows.h, the undefs undid nothing, and a TU
//     that pulled in windows.h afterwards got the macros back with no
//     protection at all.
//
// Including it explicitly here fixes both: APIENTRY is windows.h's from
// the start so GLFW leaves it alone, the undefs below have something real
// to undo, and every later `#include <windows.h>` is a no-op include-guard
// hit that cannot reintroduce the macros.
#if defined(_WIN32)
#  include <windows.h>
#endif

// GLFW_INCLUDE_NONE stops glfw3.h from pulling in <GL/gl.h>. GLFW does that
// by default, which would make this Vulkan-only renderer fail to build on
// any machine without the OpenGL development headers installed -- a real
// problem on minimal Linux containers and on Windows toolchains that don't
// ship the GL headers. Vulkan is included explicitly instead, which is
// exactly what GLFW_INCLUDE_VULKAN would have done anyway.
#define GLFW_INCLUDE_NONE
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

// Undo the Win32 A/W macros (CreateWindow/DrawText/PlaySound/DeleteFile)
// before any pv::rt declaration, so they don't mangle command names or
// their call sites downstream.
#include "pv/pv_win32_undef.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pv/pv_anim.h"
#include "pv/pv_handle.h"
#include "pv/pv_math.h"
#include "pv/pv_particles.h"
#include "pv/pv_physics.h"
#include "pv/pv_scene.h"
#include "pv/pv_value.h"

namespace pv {

// Math types (Vec3, Vec4, Quat, Mat4, Transform) now live in pv_math.h --
// they grew past "just enough for view/projection" once keyframe animation
// needed quaternions and the editor needed matrix inversion for picking.

// HandleTable moved to pv_handle.h so the scene/animation/particle/physics
// headers can declare their own tables without including Vulkan.

// ------------------------------------------------------------------
// Resource records
// ------------------------------------------------------------------
struct Vertex3D {
    float pos[3];
    float normal[3];
    float uv[2];
};

struct ShadowDrawItem {
    uint64_t meshHandle = 0;
    Mat4 model;
};

struct SceneUbo {
    float ambient[4];
    float posKind[4][4];
    float colorIntensity[4][4];
    float lightCountPad[4];
    float camPos[4];
    float jitterNearFar[4];
    float view[16];
    float proj[16];
    float viewProj[16];
    float invProj[16];
    float lightVP[2][16];
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct GpuMesh {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
};

struct GpuTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE; // one combined-image-sampler set per texture
    VkDescriptorSet pbrSet = VK_NULL_HANDLE;
};

struct MaterialRecord {
    uint64_t albedoTexture = 0;
    uint64_t normalTexture = 0;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float colorR = 1, colorG = 1, colorB = 1, colorA = 1;
    uint64_t emissiveTexture = 0;
    float emissiveIntensity = 0.0f;
    uint64_t heightTexture = 0;
    float heightScale = 0.0f;
    VkDescriptorSet materialSet = VK_NULL_HANDLE;
};

// Defined in pv_scene.h so the authoring-side LightComponent and this
// runtime record agree on one enum without pv_scene.h having to include
// Vulkan.
using LightKind = LightKindTag;
struct LightRecord {
    LightKind kind;
    Vec3 position;
    Vec3 direction;
    float r = 1, g = 1, b = 1;
    float intensity = 1;
    float radius = 10;  // point/spot
    float angle = 0.5f; // spot, radians
};

struct SoundRecord {
    std::string path;
    bool isMusic = false;
    void* maSound = nullptr; // type-erased `ma_sound*` -- see audio.cpp (keeps miniaudio.h out of this shared header)
};

// AnimRecord / AnimClip moved to pv_anim.h (they gained real per-node
// TRS channels and quaternion rotation, replacing the yaw-only float).
// ColliderRecord / RigidBodyRecord moved to pv_physics.h, where they are
// now thin script-facing handles onto whatever backend is active (PhysX
// by default) rather than the simulation state itself.

// ------------------------------------------------------------------
// The Engine: one instance, function-local static singleton (avoids
// static-initialization-order problems across translation units).
// ------------------------------------------------------------------
struct Vertex2D {
    float pos[2];
    float color[4];
    float uv[2]; // unused (0,0) for flat-color draws; meaningful for sprites
};

struct Glyph3x5 {
    const char* rows[5];
};
const Glyph3x5& lookupGlyph(char c);

struct FrameSync {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
};

struct Engine {
    // -- window / instance / device --
    GLFWwindow* window = nullptr;
    int windowWidth = 1280, windowHeight = 720;
    int windowedX = 50, windowedY = 50, windowedW = 1280, windowedH = 720; // for fullscreen toggle restore
    bool isFullscreen = false;
    bool vsyncEnabled = true;
    bool shouldClose = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = 0, presentFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    // -- swapchain --
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> swapchainFramebuffers;
    // One fence pointer per swapchain image so we never record into an
    // image the previous frame-in-flight still owns (classic "2 FIF, 2
    // images" race that shows up as a white window then a GPU TDR).
    std::vector<VkFence> imagesInFlight;
    VkRenderPass renderPass = VK_NULL_HANDLE;

    // -- depth buffer (for 3D) --
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    // -- commands / sync --
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    std::array<FrameSync, MAX_FRAMES_IN_FLIGHT> frameSync;
    size_t currentFrame = 0;
    uint32_t currentImageIndex = 0;
    VkCommandBuffer activeCmd = VK_NULL_HANDLE;
    bool frameActive = false;
    bool swapchainOutOfDate = false;

    // -- 2D pipeline (flat-colored primitives) --
    VkPipelineLayout pipeline2DLayout = VK_NULL_HANDLE;
    VkPipeline pipeline2D = VK_NULL_HANDLE;
    // Each frame-in-flight gets its own persistently-mapped, ring-buffer
    // style vertex buffer: every Pv::DrawXxx call appends its vertices at
    // the current write cursor and issues its draw call immediately (see
    // draw2d.cpp / draw3d.cpp), which is what keeps draw order correct
    // when a script interleaves flat shapes, sprites, and 3D meshes.
    VkBuffer vbo2D[MAX_FRAMES_IN_FLIGHT] = {};
    VkDeviceMemory vbo2DMemory[MAX_FRAMES_IN_FLIGHT] = {};
    void* vbo2DMapped[MAX_FRAMES_IN_FLIGHT] = {};
    static constexpr size_t VBO2D_CAPACITY = 1 << 16; // vertices
    uint32_t vbo2DCursor = 0;

    // -- textured 2D (sprites) pipeline --
    VkPipelineLayout pipelineSpriteLayout = VK_NULL_HANDLE;
    VkPipeline pipelineSprite = VK_NULL_HANDLE;
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE; // single combined image sampler @ binding 0
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkImage blankImage = VK_NULL_HANDLE; // 1x1 white texture, used as a safe default
    VkDeviceMemory blankImageMemory = VK_NULL_HANDLE;
    VkImageView blankImageView = VK_NULL_HANDLE;
    VkSampler blankSampler = VK_NULL_HANDLE;
    VkDescriptorSet blankDescriptorSet = VK_NULL_HANDLE;

    // -- 3D pipeline --
    VkPipelineLayout pipeline3DLayout = VK_NULL_HANDLE;
    VkPipeline pipeline3D = VK_NULL_HANDLE;
    VkPipeline pipeline3DWireframe = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightsSetLayout = VK_NULL_HANDLE; // lights UBO @ binding 0, set=0
    VkDescriptorSet lightsDescriptorSet = VK_NULL_HANDLE;
    VkBuffer lightsUbo = VK_NULL_HANDLE;
    VkDeviceMemory lightsUboMemory = VK_NULL_HANDLE;
    void* lightsUboMapped = nullptr;
    static constexpr int MAX_LIGHTS = 4;
    bool wireframe = false;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet blankMaterialSet = VK_NULL_HANDLE;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkImage blankNormalImage = VK_NULL_HANDLE;
    VkDeviceMemory blankNormalMemory = VK_NULL_HANDLE;
    VkImageView blankNormalView = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;

    GpuImage hdrColor;
    GpuImage bloomColor;
    GpuImage historyColor;
    GpuImage lutColor;
    VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;
    VkRenderPass swapchainRenderPass = VK_NULL_HANDLE;
    VkRenderPass shadowRenderPass = VK_NULL_HANDLE;
    VkRenderPass bloomRenderPass = VK_NULL_HANDLE;
    VkPipeline pipelineShadow = VK_NULL_HANDLE;
    VkPipelineLayout pipelineShadowLayout = VK_NULL_HANDLE;
    VkPipeline pipelineBloom = VK_NULL_HANDLE;
    VkPipeline pipelinePost = VK_NULL_HANDLE;
    VkPipelineLayout pipelinePostLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineBloomLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet postDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet bloomDescriptorSet = VK_NULL_HANDLE;
    VkFramebuffer bloomFramebuffer = VK_NULL_HANDLE;
    GpuImage shadowArray;
    VkImageView shadowLayerViews[2] = {};
    VkFramebuffer shadowFramebuffers[2] = {};
    std::vector<ShadowDrawItem> shadowDraws;
    uint64_t colorLutHandle = 0;
    float taaBlend = 0.85f;
    int taaFrame = 0;
    Mat4 lightViewProj[2];
    float cascadeSplit = 12.0f;

    // -- particle pipeline --
    // Two pipeline variants sharing one layout: alpha-blended (sorted,
    // depth-tested, depth-write off) and additive (unsorted, same depth
    // setup). Depth *write* is disabled for both -- transparent geometry
    // that writes depth occludes the particles drawn behind it, which is
    // the classic "the far half of my smoke plume disappeared" artifact.
    VkPipelineLayout pipelineParticleLayout = VK_NULL_HANDLE;
    VkPipeline pipelineParticleAlpha = VK_NULL_HANDLE;
    VkPipeline pipelineParticleAdditive = VK_NULL_HANDLE;
    VkBuffer vboParticle[MAX_FRAMES_IN_FLIGHT] = {};
    VkDeviceMemory vboParticleMemory[MAX_FRAMES_IN_FLIGHT] = {};
    void* vboParticleMapped[MAX_FRAMES_IN_FLIGHT] = {};
    static constexpr size_t VBO_PARTICLE_CAPACITY = 1 << 17; // vertices (~21k quads/frame)
    uint32_t vboParticleCursor = 0;
    bool warnedParticleOverflow = false;

    // -- frame timing --
    std::chrono::steady_clock::time_point lastFrameTime;
    float deltaTime = 0.0f;
    float fps = 0.0f;
    float fpsAccum = 0.0f;
    int fpsFrames = 0;

    // -- clear color --
    float clearR = 0, clearG = 0, clearB = 0, clearA = 1;

    // -- camera --
    Vec3 camPos{0, 0, -5};
    Vec3 camTarget{0, 0, 0};
    bool camHasTarget = false;
    float camPitch = 0, camYaw = 0, camRoll = 0;
    float fovDegrees = 60.0f;
    float nearZ = 0.05f, farZ = 1000.0f;
    bool orthographic = false;

    // -- lighting --
    float ambientR = 0.1f, ambientG = 0.1f, ambientB = 0.1f, ambientIntensity = 1.0f;
    HandleTable<LightRecord> lights;
    bool shadowsEnabled = true;
    int shadowResolution = 1024;
    float fogR = 0, fogG = 0, fogB = 0, fogNear = 50, fogFar = 200;
    bool fogEnabled = false;

    // -- resources --
    HandleTable<GpuMesh> meshes;
    HandleTable<GpuTexture> textures;
    HandleTable<MaterialRecord> materials;
    HandleTable<SoundRecord> sounds;
    HandleTable<AnimRecord> anims;
    HandleTable<ColliderRecord> colliders;
    HandleTable<RigidBodyRecord> bodies;
    HandleTable<EmitterRecord> emitters;

    // -- physics --
    // The simulation itself lives behind a backend interface (PhysX by
    // default, the original hand-written solver as a fallback) -- see
    // pv_physics.h for why. `gravity` is mirrored here so Pv::SetGravity
    // can be called before Pv::InitPhysics and still take effect.
    Vec3 gravity{0, -9.8f, 0};
    bool physicsInitialized = false;
    std::unique_ptr<PhysicsBackend> physics;
    float physicsFixedHz = 60.0f;
    int physicsMaxSubsteps = 4;

    // -- scene --
    // One active scene at a time. A pointer rather than a value because
    // Scene is large and the editor swaps between scenes by moving the
    // pointer rather than copying.
    std::unique_ptr<Scene> scene;

    // -- input edge-detection state --
    struct EdgeState {
        bool prev = false, cur = false;
        uint64_t lastUpdatedFrame = 0;
    };
    std::unordered_map<int, EdgeState> keyEdge;
    std::unordered_map<int, EdgeState> mouseEdge;
    std::unordered_map<int, EdgeState> gamepadEdge; // key = id*1000 + button
    uint64_t frameCounter = 0;
    double mouseLastX = 0, mouseLastY = 0;
    double mouseDeltaX = 0, mouseDeltaY = 0;
    double scrollAccum = 0;
    int lastKeyPressed = -1;

    // -- UI immediate-mode state --
    bool uiActive = false;
    std::unordered_map<std::string, std::string> uiTextBuffers;
    std::string uiFocusedLabel;
    std::string uiCharsTypedThisFrame;

    // -- audio (type-erased so miniaudio.h stays out of this shared header;
    //    see audio.cpp) --
    void* audioEngine = nullptr;         // ma_engine*
    void* audioResourceManager = nullptr; // ma_resource_manager*, owns the custom OGG/Vorbis decoder
    bool audioInitialized = false;

    // -- shaders (Tier 4: tracked but not yet driving custom pipelines) --
    HandleTable<std::string> shaderLabels;

    bool initialized = false;
};

Engine& engine();

// Returns false (and logs once) if Vulkan isn't ready yet -- e.g. a
// script called Pv::CreateCube() before Pv::CreateWindow() succeeded, or
// continued after CreateWindow returned false without checking it. Every
// public Pv:: function that allocates a GPU resource checks this first,
// so the failure mode is a clear log message and a null-ish return value
// instead of a crash on a null VkDevice.
bool requireInitialized(const char* fnName);

// Shared helpers implemented in vk_core.cpp, used by the drawing /
// resource-management translation units.
uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                   VkBuffer& buffer, VkDeviceMemory& memory);
void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);
VkCommandBuffer beginOneShotCommands();
void endOneShotCommands(VkCommandBuffer cmd);

// Appends `count` vertices to the current frame's ring-buffer VBO and
// issues an immediate (bind pipeline + bind vbo + draw) command -- see the
// MAX_FRAMES_IN_FLIGHT vbo2D comment in Engine for why this is immediate
// rather than batched.
void draw2DVerts(const Vertex2D* verts, uint32_t count, bool textured, VkDescriptorSet texSet);

// Particle equivalent: appends world-space quad vertices to the current
// frame's particle ring buffer and issues one draw with the given
// view-projection matrix. `additive` selects the blend pipeline variant.
struct ParticleVertexLayout {
    float pos[3];
    float color[4];
    float uv[2];
};
void drawParticleVerts(const void* verts, uint32_t count, const Mat4& viewProj, uint64_t textureHandle,
                       bool additive);

// Draws a mesh with an explicit world matrix and optional material.
// Implemented in draw3d.cpp; used by the scene renderer, which already has
// a world matrix and shouldn't have to decompose it back into DrawMeshEx's
// position/euler/uniform-scale arguments.
void drawSceneMesh(uint64_t meshHandle, const Mat4& model, uint64_t materialHandle);

// Allocates (or reuses a cached) 1-sampler descriptor set bound to `view`
// via `sampler`. Used by texture loading and by the blank fallback texture.
VkDescriptorSet allocSamplerDescriptorSet(VkImageView view, VkSampler sampler);
VkDescriptorSet allocMaterialDescriptorSet(VkImageView albedo, VkSampler albedoSamp,
                                           VkImageView normal, VkSampler normalSamp,
                                           VkImageView height, VkSampler heightSamp);

void createPostResources();
void destroyPostResources();
void recreatePostExtentResources();
void recordShadowAndPost(VkCommandBuffer cmd, uint32_t swapImageIndex);

Mat4 buildViewMatrix();
Mat4 buildProjectionMatrix(bool forOrtho2D = false);
void updateLightsUbo();
std::vector<uint32_t> readSpirvFile(const std::string& path);
std::string exeDirectory();
void logLine(const char* level, const std::string& msg);

// Uploads vertex(+index) data to GPU buffers and returns a mesh handle.
// Shared by mesh.cpp (Pv::CreateMesh/LoadMesh/CreateCube/...) and
// draw3d.cpp (the lazily-created unit quad DrawBillboard reuses).
uint64_t uploadMeshData(const std::vector<Vertex3D>& verts, const std::vector<uint32_t>& indices);

// Same idea for textures: terrain_render.cpp bakes its splat map on the
// CPU and needs it on the GPU without duplicating the staging-buffer and
// layout-transition dance in texture.cpp.
uint64_t uploadTextureRGBA8(const uint8_t* pixels, uint32_t width, uint32_t height);

// Frees a mesh's device buffers. Deliberately does NOT call
// vkDeviceWaitIdle: terrain rebuilds destroy hundreds of chunk meshes in
// one go, and waiting per mesh turns a rebuild into a multi-second stall.
// Callers synchronize once around the batch instead.
void destroyMeshBuffers(GpuMesh& mesh);

// Lifecycle, implemented in vk_core.cpp, called from window.cpp.
void vkInitAfterWindow(bool vsync);
void vkShutdown();
void vkRequestSwapchainRecreate();
void vkRecreateSwapchainIfNeeded();

// Implemented in audio.cpp, called from window.cpp's Pv::Shutdown.
void shutdownAudio();

} // namespace pv
