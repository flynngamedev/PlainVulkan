// texture.cpp -- Pv::LoadTexture / CreateTexture / ... . Real PNG/JPG
// decoding via the vendored stb_image (see runtime/third_party/), real
// GPU upload. LoadCubemap and CreateRenderTexture are simplified (see the
// comments on each) -- documented in runtime/README.md's tier table.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include <cstring>

namespace pv {

static uint64_t uploadPixelsRGBA8(const uint8_t* pixels, uint32_t width, uint32_t height) {
    auto& e = engine();
    GpuTexture tex;
    tex.width = width;
    tex.height = height;

    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    VkBuffer staging;
    VkDeviceMemory stagingMem;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMem);
    void* mapped;
    vkMapMemory(e.device, stagingMem, 0, size, 0, &mapped);
    memcpy(mapped, pixels, static_cast<size_t>(size));
    vkUnmapMemory(e.device, stagingMem);

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(e.device, &ci, nullptr, &tex.image);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(e.device, tex.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(e.device, &ai, nullptr, &tex.memory);
    vkBindImageMemory(e.device, tex.image, tex.memory, 0);

    VkCommandBuffer cmd = beginOneShotCommands();
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = tex.image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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
    vi.image = tex.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(e.device, &vi, nullptr, &tex.view);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 0.0f;
    vkCreateSampler(e.device, &si, nullptr, &tex.sampler);

    tex.descriptorSet = allocSamplerDescriptorSet(tex.view, tex.sampler);
    return e.textures.add(tex);
}

// Public wrapper (declared in pv_internal.h), mirroring uploadMeshData in
// mesh.cpp. terrain_render.cpp bakes a splat map into RGBA8 on the CPU and
// needs it uploaded without reimplementing the staging buffer, the two
// layout transitions and the descriptor-set allocation above.
uint64_t uploadTextureRGBA8(const uint8_t* pixels, uint32_t width, uint32_t height) {
    return uploadPixelsRGBA8(pixels, width, height);
}

namespace rt {

Value LoadTexture(const Value& filepath) {
    if (!requireInitialized("Pv::LoadTexture")) return Value::MakeHandle(0, "texture");
    std::string path = filepath.asString();
    int w, h, channels;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) {
        logLine("ERROR", "Pv::LoadTexture failed to load '" + path + "': " + stbi_failure_reason());
        return Value::MakeHandle(0, "texture");
    }
    uint64_t id = uploadPixelsRGBA8(pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    stbi_image_free(pixels);
    return Value::MakeHandle(id, "texture");
}

Value UnloadTexture(const Value& texture) {
    auto& e = engine();
    uint64_t id = static_cast<uint64_t>(texture.asInt());
    if (GpuTexture* tex = e.textures.get(id)) {
        vkDeviceWaitIdle(e.device);
        vkDestroySampler(e.device, tex->sampler, nullptr);
        vkDestroyImageView(e.device, tex->view, nullptr);
        vkDestroyImage(e.device, tex->image, nullptr);
        vkFreeMemory(e.device, tex->memory, nullptr);
        e.textures.remove(id);
    }
    return Value();
}

Value CreateTexture(const Value& width, const Value& height, const Value& format) {
    if (!requireInitialized("Pv::CreateTexture")) return Value::MakeHandle(0, "texture");
    (void)format; // only RGBA8 is supported -- see runtime/README.md
    uint32_t w = static_cast<uint32_t>(width.asInt());
    uint32_t h = static_cast<uint32_t>(height.asInt());
    std::vector<uint8_t> blank(static_cast<size_t>(w) * h * 4, 0);
    uint64_t id = uploadPixelsRGBA8(blank.data(), w, h);
    return Value::MakeHandle(id, "texture");
}

Value SetTextureFilter(const Value& texture, const Value& filter) {
    (void)texture;
    (void)filter;
    logLine("WARNING", "Pv::SetTextureFilter: per-texture filter changes aren't implemented yet (textures use "
                        "linear filtering); tracked as a Tier 4 gap in runtime/README.md");
    return Value();
}

Value SetTextureWrap(const Value& texture, const Value& wrap) {
    (void)texture;
    (void)wrap;
    logLine("WARNING", "Pv::SetTextureWrap: per-texture wrap mode changes aren't implemented yet (textures use "
                        "repeat wrapping); tracked as a Tier 4 gap in runtime/README.md");
    return Value();
}

Value LoadCubemap(const Value& filepath) {
    // Simplification: loads the given image as a plain 2D texture so the
    // handle is valid and usable, rather than a true 6-face cube map.
    // Pv::DrawSkybox is a documented Tier 4 stub for the same reason.
    logLine("WARNING", "Pv::LoadCubemap: loading as a flat 2D texture, not a true cubemap (see runtime/README.md)");
    return LoadTexture(filepath);
}

Value CreateRenderTexture(const Value& width, const Value& height) {
    logLine("WARNING", "Pv::CreateRenderTexture: returns a real sampleable texture, but nothing renders into it "
                        "yet -- render-to-texture is a documented Tier 4 gap in runtime/README.md");
    return CreateTexture(width, height, Value("rgba8"));
}

} // namespace rt
} // namespace pv
