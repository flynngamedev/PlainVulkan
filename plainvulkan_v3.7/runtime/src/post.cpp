// post.cpp -- HDR offscreen, cascaded shadows, bloom, TAA history, LUT, composite.
#include "pv/pv_internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace pv {

static uint32_t memType(uint32_t bits, VkMemoryPropertyFlags props) {
    return findMemoryType(bits, props);
}

static void destroyGpuImage(GpuImage& img) {
    auto& e = engine();
    if (img.view) vkDestroyImageView(e.device, img.view, nullptr);
    if (img.image) vkDestroyImage(e.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(e.device, img.memory, nullptr);
    img = {};
}

static GpuImage makeColor(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    auto& e = engine();
    GpuImage out;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.format = format;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = usage;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    vkCreateImage(e.device, &ci, nullptr, &out.image);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(e.device, out.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(e.device, &ai, nullptr, &out.memory);
    vkBindImageMemory(e.device, out.image, out.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(e.device, &vi, nullptr, &out.view);
    return out;
}

static void writeCombined(VkDescriptorSet set, uint32_t binding, VkImageView view, VkSampler samp,
                          VkImageLayout layout) {
    auto& e = engine();
    VkDescriptorImageInfo ii{samp, view, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(e.device, 1, &w, 0, nullptr);
}

static VkPipeline makeFullscreen(VkShaderModule vert, VkShaderModule frag, VkPipelineLayout layout,
                                 VkRenderPass rp) {
    auto& e = engine();
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyns;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rast;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = layout;
    pci.renderPass = rp;
    pci.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(e.device, VK_NULL_HANDLE, 1, &pci, nullptr, &p);
    return p;
}

static std::vector<uint32_t> spirv(const char* name) {
    return readSpirvFile(exeDirectory() + "/shaders/" + name);
}

static VkShaderModule makeMod(const std::vector<uint32_t>& words) {
    auto& e = engine();
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4;
    ci.pCode = words.data();
    VkShaderModule m;
    vkCreateShaderModule(e.device, &ci, nullptr, &m);
    return m;
}

void recreatePostExtentResources() {
    auto& e = engine();
    if (!e.device || e.swapchainExtent.width == 0) return;
    if (e.sceneFramebuffer) vkDestroyFramebuffer(e.device, e.sceneFramebuffer, nullptr);
    e.sceneFramebuffer = VK_NULL_HANDLE;
    if (e.bloomFramebuffer) vkDestroyFramebuffer(e.device, e.bloomFramebuffer, nullptr);
    e.bloomFramebuffer = VK_NULL_HANDLE;
    destroyGpuImage(e.hdrColor);
    destroyGpuImage(e.bloomColor);
    destroyGpuImage(e.historyColor);

    uint32_t w = e.swapchainExtent.width;
    uint32_t h = e.swapchainExtent.height;
    VkImageUsageFlags colorUse = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    e.hdrColor = makeColor(VK_FORMAT_R16G16B16A16_SFLOAT, w, h, colorUse);
    e.bloomColor = makeColor(VK_FORMAT_R16G16B16A16_SFLOAT, w, h, colorUse);
    e.historyColor = makeColor(VK_FORMAT_R16G16B16A16_SFLOAT, w, h, colorUse);

    if (e.renderPass && e.depthView) {
        VkImageView atts[] = {e.hdrColor.view, e.depthView};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = e.renderPass;
        fi.attachmentCount = 2;
        fi.pAttachments = atts;
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        vkCreateFramebuffer(e.device, &fi, nullptr, &e.sceneFramebuffer);
    }
    if (e.bloomRenderPass) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = e.bloomRenderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &e.bloomColor.view;
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        vkCreateFramebuffer(e.device, &fi, nullptr, &e.bloomFramebuffer);
    }
    if (e.postDescriptorSet && e.linearSampler) {
        writeCombined(e.bloomDescriptorSet, 0, e.hdrColor.view, e.linearSampler,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        writeCombined(e.postDescriptorSet, 0, e.hdrColor.view, e.linearSampler,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        writeCombined(e.postDescriptorSet, 1, e.depthView, e.linearSampler,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        writeCombined(e.postDescriptorSet, 2, e.bloomColor.view, e.linearSampler,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        writeCombined(e.postDescriptorSet, 3, e.historyColor.view, e.linearSampler,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        writeCombined(e.postDescriptorSet, 4, e.lutColor.view, e.linearSampler,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void destroyPostResources() {
    auto& e = engine();
    if (!e.device) return;
    if (e.sceneFramebuffer) vkDestroyFramebuffer(e.device, e.sceneFramebuffer, nullptr);
    if (e.bloomFramebuffer) vkDestroyFramebuffer(e.device, e.bloomFramebuffer, nullptr);
    e.sceneFramebuffer = e.bloomFramebuffer = VK_NULL_HANDLE;
    destroyGpuImage(e.hdrColor);
    destroyGpuImage(e.bloomColor);
    destroyGpuImage(e.historyColor);
    destroyGpuImage(e.lutColor);
    destroyGpuImage(e.shadowArray);
    for (int i = 0; i < 2; ++i) {
        if (e.shadowFramebuffers[i]) vkDestroyFramebuffer(e.device, e.shadowFramebuffers[i], nullptr);
        e.shadowFramebuffers[i] = VK_NULL_HANDLE;
        if (e.shadowLayerViews[i]) vkDestroyImageView(e.device, e.shadowLayerViews[i], nullptr);
        e.shadowLayerViews[i] = VK_NULL_HANDLE;
    }
    if (e.pipelineBloom) vkDestroyPipeline(e.device, e.pipelineBloom, nullptr);
    if (e.pipelinePost) vkDestroyPipeline(e.device, e.pipelinePost, nullptr);
    if (e.pipelineShadow) vkDestroyPipeline(e.device, e.pipelineShadow, nullptr);
    if (e.pipelineBloomLayout) vkDestroyPipelineLayout(e.device, e.pipelineBloomLayout, nullptr);
    if (e.pipelinePostLayout) vkDestroyPipelineLayout(e.device, e.pipelinePostLayout, nullptr);
    if (e.pipelineShadowLayout) vkDestroyPipelineLayout(e.device, e.pipelineShadowLayout, nullptr);
    if (e.bloomRenderPass) vkDestroyRenderPass(e.device, e.bloomRenderPass, nullptr);
    if (e.shadowRenderPass) vkDestroyRenderPass(e.device, e.shadowRenderPass, nullptr);
    if (e.swapchainRenderPass) vkDestroyRenderPass(e.device, e.swapchainRenderPass, nullptr);
    if (e.postSetLayout) vkDestroyDescriptorSetLayout(e.device, e.postSetLayout, nullptr);
    e.pipelineBloom = e.pipelinePost = e.pipelineShadow = VK_NULL_HANDLE;
    e.pipelineBloomLayout = e.pipelinePostLayout = e.pipelineShadowLayout = VK_NULL_HANDLE;
    e.bloomRenderPass = e.shadowRenderPass = e.swapchainRenderPass = VK_NULL_HANDLE;
    e.postSetLayout = VK_NULL_HANDLE;
}

static void createIdentityLut() {
    auto& e = engine();
    const uint32_t w = 256, h = 16;
    std::vector<uint8_t> px(w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t slice = x / 16;
            uint32_t r = x % 16;
            uint32_t g = y;
            uint32_t b = slice;
            size_t i = (static_cast<size_t>(y) * w + x) * 4;
            px[i + 0] = static_cast<uint8_t>(r * 17);
            px[i + 1] = static_cast<uint8_t>(g * 17);
            px[i + 2] = static_cast<uint8_t>(b * 17);
            px[i + 3] = 255;
        }
    }
    e.lutColor = makeColor(VK_FORMAT_R8G8B8A8_UNORM, w, h,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkBuffer staging;
    VkDeviceMemory stagingMem;
    createBuffer(px.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMem);
    void* mapped = nullptr;
    vkMapMemory(e.device, stagingMem, 0, px.size(), 0, &mapped);
    memcpy(mapped, px.data(), px.size());
    vkUnmapMemory(e.device, stagingMem);
    VkCommandBuffer cmd = beginOneShotCommands();
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = e.lutColor.image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toDst);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, e.lutColor.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    toDst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toDst.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toDst);
    endOneShotCommands(cmd);
    vkDestroyBuffer(e.device, staging, nullptr);
    vkFreeMemory(e.device, stagingMem, nullptr);
}

void createPostResources() {
    auto& e = engine();

    VkAttachmentDescription hdrColorAtt{};
    hdrColorAtt.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    hdrColorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
    hdrColorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdrColorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    hdrColorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    hdrColorAtt.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference bloomRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription bloomSub{};
    bloomSub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    bloomSub.colorAttachmentCount = 1;
    bloomSub.pColorAttachments = &bloomRef;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &hdrColorAtt;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &bloomSub;
    vkCreateRenderPass(e.device, &rpci, nullptr, &e.bloomRenderPass);

    VkAttachmentDescription depth{};
    depth.format = e.depthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription shSub{};
    shSub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    shSub.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo shci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    shci.attachmentCount = 1;
    shci.pAttachments = &depth;
    shci.subpassCount = 1;
    shci.pSubpasses = &shSub;
    vkCreateRenderPass(e.device, &shci, nullptr, &e.shadowRenderPass);

    uint32_t res = static_cast<uint32_t>(std::max(256, e.shadowResolution));
    VkImageCreateInfo sci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    sci.imageType = VK_IMAGE_TYPE_2D;
    sci.extent = {res, res, 1};
    sci.mipLevels = 1;
    sci.arrayLayers = 2;
    sci.format = e.depthFormat;
    sci.tiling = VK_IMAGE_TILING_OPTIMAL;
    sci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    sci.samples = VK_SAMPLE_COUNT_1_BIT;
    vkCreateImage(e.device, &sci, nullptr, &e.shadowArray.image);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(e.device, e.shadowArray.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(e.device, &ai, nullptr, &e.shadowArray.memory);
    vkBindImageMemory(e.device, e.shadowArray.image, e.shadowArray.memory, 0);
    VkImageViewCreateInfo svi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    svi.image = e.shadowArray.image;
    svi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    svi.format = e.depthFormat;
    svi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2};
    vkCreateImageView(e.device, &svi, nullptr, &e.shadowArray.view);

    for (int i = 0; i < 2; ++i) {
        VkImageViewCreateInfo lvi = svi;
        lvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        lvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, static_cast<uint32_t>(i), 1};
        vkCreateImageView(e.device, &lvi, nullptr, &e.shadowLayerViews[i]);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = e.shadowRenderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &e.shadowLayerViews[i];
        fi.width = res;
        fi.height = res;
        fi.layers = 1;
        vkCreateFramebuffer(e.device, &fi, nullptr, &e.shadowFramebuffers[i]);
    }

    createIdentityLut();

    VkDescriptorSetLayoutBinding binds[5]{};
    for (int i = 0; i < 5; ++i) {
        binds[i].binding = static_cast<uint32_t>(i);
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 5;
    sl.pBindings = binds;
    vkCreateDescriptorSetLayout(e.device, &sl, nullptr, &e.postSetLayout);

    VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds.descriptorPool = e.descriptorPool;
    ds.descriptorSetCount = 1;
    ds.pSetLayouts = &e.postSetLayout;
    vkAllocateDescriptorSets(e.device, &ds, &e.postDescriptorSet);
    VkDescriptorSetLayout bloomLayout = e.samplerSetLayout;
    ds.pSetLayouts = &bloomLayout;
    vkAllocateDescriptorSets(e.device, &ds, &e.bloomDescriptorSet);

    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4};
    std::array<VkDescriptorSetLayout, 2> postLayouts{e.postSetLayout, e.lightsSetLayout};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2;
    pl.pSetLayouts = postLayouts.data();
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(e.device, &pl, nullptr, &e.pipelinePostLayout);

    VkPipelineLayoutCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    bl.setLayoutCount = 1;
    bl.pSetLayouts = &e.samplerSetLayout;
    vkCreatePipelineLayout(e.device, &bl, nullptr, &e.pipelineBloomLayout);

    VkPushConstantRange spc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 32};
    VkPipelineLayoutCreateInfo shl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    shl.pushConstantRangeCount = 1;
    shl.pPushConstantRanges = &spc;
    vkCreatePipelineLayout(e.device, &shl, nullptr, &e.pipelineShadowLayout);

    auto fsVert = makeMod(spirv("fullscreen.vert.spv"));
    auto postFrag = makeMod(spirv("post.frag.spv"));
    auto bloomFrag = makeMod(spirv("bloom.frag.spv"));
    e.pipelinePost = makeFullscreen(fsVert, postFrag, e.pipelinePostLayout, e.swapchainRenderPass);
    e.pipelineBloom = makeFullscreen(fsVert, bloomFrag, e.pipelineBloomLayout, e.bloomRenderPass);
    vkDestroyShaderModule(e.device, fsVert, nullptr);
    vkDestroyShaderModule(e.device, postFrag, nullptr);
    vkDestroyShaderModule(e.device, bloomFrag, nullptr);

    auto shVert = makeMod(spirv("shadow.vert.spv"));
    auto shFrag = makeMod(spirv("shadow.frag.spv"));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shVert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shFrag;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, sizeof(Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 3> attrs{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, pos)},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, normal)},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex3D, uv)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_FRONT_BIT;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.depthBiasEnable = VK_TRUE;
    rast.depthBiasConstantFactor = 1.25f;
    rast.depthBiasSlopeFactor = 1.75f;
    rast.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dsState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsState.depthTestEnable = VK_TRUE;
    dsState.depthWriteEnable = VK_TRUE;
    dsState.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyns;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rast;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &dsState;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = e.pipelineShadowLayout;
    pci.renderPass = e.shadowRenderPass;
    vkCreateGraphicsPipelines(e.device, VK_NULL_HANDLE, 1, &pci, nullptr, &e.pipelineShadow);
    vkDestroyShaderModule(e.device, shVert, nullptr);
    vkDestroyShaderModule(e.device, shFrag, nullptr);

    VkDescriptorImageInfo shInfo{e.shadowSampler, e.shadowArray.view,
                                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = e.lightsDescriptorSet;
    w.dstBinding = 1;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &shInfo;
    vkUpdateDescriptorSets(e.device, 1, &w, 0, nullptr);

    recreatePostExtentResources();
}

static void memcpyMat(float* dst, const Mat4& m) { memcpy(dst, m.m, sizeof(float) * 16); }

void recordShadowAndPost(VkCommandBuffer cmd, uint32_t swapImageIndex) {
    auto& e = engine();
    uint32_t res = static_cast<uint32_t>(std::max(256, e.shadowResolution));
    if (e.shadowsEnabled && e.pipelineShadow && e.shadowDraws.size() > 0) {
        for (int c = 0; c < 2; ++c) {
            VkClearValue clear{};
            clear.depthStencil = {1.0f, 0};
            VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rp.renderPass = e.shadowRenderPass;
            rp.framebuffer = e.shadowFramebuffers[c];
            rp.renderArea.extent = {res, res};
            rp.clearValueCount = 1;
            rp.pClearValues = &clear;
            vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vp{0, 0, static_cast<float>(res), static_cast<float>(res), 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {res, res}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e.pipelineShadow);
            struct Push {
                float lightVP[16];
                float model[16];
            } push{};
            memcpyMat(push.lightVP, e.lightViewProj[c]);
            for (const ShadowDrawItem& item : e.shadowDraws) {
                GpuMesh* mesh = e.meshes.get(item.meshHandle);
                if (!mesh) continue;
                memcpyMat(push.model, item.model);
                vkCmdPushConstants(cmd, e.pipelineShadowLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, &off);
                if (mesh->indexBuffer) {
                    vkCmdBindIndexBuffer(cmd, mesh->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
                } else {
                    vkCmdDraw(cmd, mesh->vertexCount, 1, 0, 0);
                }
            }
            vkCmdEndRenderPass(cmd);
        }
    }

    if (e.pipelineBloom && e.bloomFramebuffer) {
        VkClearValue clear{};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = e.bloomRenderPass;
        rp.framebuffer = e.bloomFramebuffer;
        rp.renderArea.extent = e.swapchainExtent;
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp{0, 0, static_cast<float>(e.swapchainExtent.width),
                      static_cast<float>(e.swapchainExtent.height), 0, 1};
        VkRect2D sc{{0, 0}, e.swapchainExtent};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e.pipelineBloom);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e.pipelineBloomLayout, 0, 1,
                                &e.bloomDescriptorSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    if (e.pipelinePost && swapImageIndex < e.swapchainFramebuffers.size()) {
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = e.swapchainRenderPass;
        rp.framebuffer = e.swapchainFramebuffers[swapImageIndex];
        rp.renderArea.extent = e.swapchainExtent;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp{0, 0, static_cast<float>(e.swapchainExtent.width),
                      static_cast<float>(e.swapchainExtent.height), 0, 1};
        VkRect2D sc{{0, 0}, e.swapchainExtent};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e.pipelinePost);
        VkDescriptorSet sets[2] = {e.postDescriptorSet, e.lightsDescriptorSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e.pipelinePostLayout, 0, 2, sets, 0, nullptr);
        float params[4] = {static_cast<float>(e.frameCounter) * 0.016f, e.taaBlend,
                           1.0f / static_cast<float>(std::max(1u, e.swapchainExtent.width)),
                           1.0f / static_cast<float>(std::max(1u, e.swapchainExtent.height))};
        vkCmdPushConstants(cmd, e.pipelinePostLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), params);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    if (e.hdrColor.image && e.historyColor.image) {
        VkImageMemoryBarrier b[2]{};
        for (int i = 0; i < 2; ++i) {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        b[0].image = e.hdrColor.image;
        b[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[1].image = e.historyColor.image;
        b[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 2, b);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {e.swapchainExtent.width, e.swapchainExtent.height, 1};
        vkCmdCopyImage(cmd, e.hdrColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, e.historyColor.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, b);
    }

    e.shadowDraws.clear();
}

} // namespace pv
