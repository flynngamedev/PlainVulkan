// frame.cpp -- Pv::BeginFrame / Pv::EndFrame / Pv::Clear / timing. Owns the
// acquire -> record -> submit -> present cycle; see vk_core.cpp for the
// pipelines and buffers this records commands against.
#include "pv/pv_internal.h"
#include "pv/pv_net.h"
#include "pv/pv_runtime.h"

#include <algorithm>
#include <array>

namespace pv {
namespace rt {

Value BeginFrame() {
    // Deliver any network callbacks the background thread queued since the
    // last frame. Done here, at a single well-defined point in the frame,
    // so a handler that moves an entity always runs before this frame reads
    // transforms -- rather than at whatever arbitrary moment the packet
    // happened to arrive.
    net::pumpCallbacks();

    auto& e = engine();
    if (!e.initialized) return Value();

    vkRecreateSwapchainIfNeeded();

    vkWaitForFences(e.device, 1, &e.frameSync[e.currentFrame].inFlight, VK_TRUE, UINT64_MAX);

    // Initialized rather than left indeterminate: vkAcquireNextImageKHR only
    // writes *pImageIndex when it succeeds (or returns VK_SUBOPTIMAL_KHR).
    // On any other failure -- VK_ERROR_SURFACE_LOST_KHR when a display is
    // hot-unplugged, VK_ERROR_DEVICE_LOST after a driver reset -- the old
    // code went on to index swapchainFramebuffers[] with an uninitialized
    // value, reading out of bounds and usually crashing.
    uint32_t imageIndex = 0;
    VkResult acquireResult =
        vkAcquireNextImageKHR(e.device, e.swapchain, UINT64_MAX, e.frameSync[e.currentFrame].imageAvailable,
                               VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        vkRequestSwapchainRecreate();
        vkRecreateSwapchainIfNeeded();
        acquireResult =
            vkAcquireNextImageKHR(e.device, e.swapchain, UINT64_MAX, e.frameSync[e.currentFrame].imageAvailable,
                                   VK_NULL_HANDLE, &imageIndex);
    }
    // VK_SUBOPTIMAL_KHR still produced a usable image; anything else did not.
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        logLine("ERROR", "vkAcquireNextImageKHR failed (VkResult " +
                             std::to_string(static_cast<int>(acquireResult)) +
                             ") -- skipping this frame. The swapchain will be rebuilt on the next one.");
        vkRequestSwapchainRecreate();
        // frameActive stays false, so the matching EndFrame() is a no-op and
        // nothing tries to record into a command buffer that was never begun.
        return Value();
    }
    if (imageIndex >= e.swapchainFramebuffers.size()) {
        logLine("ERROR", "swapchain returned image index " + std::to_string(imageIndex) +
                             " but only " + std::to_string(e.swapchainFramebuffers.size()) +
                             " framebuffers exist -- skipping this frame.");
        vkRequestSwapchainRecreate();
        return Value();
    }
    // Wait until this swapchain image is free (it may still be owned by the
    // other frame-in-flight). Must happen *before* we reset the current
    // frame's fence, or waiting on an aliased fence deadlocks.
    if (imageIndex < e.imagesInFlight.size() && e.imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(e.device, 1, &e.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    if (imageIndex < e.imagesInFlight.size()) {
        e.imagesInFlight[imageIndex] = e.frameSync[e.currentFrame].inFlight;
    }
    e.currentImageIndex = imageIndex;
    vkResetFences(e.device, 1, &e.frameSync[e.currentFrame].inFlight);

    VkCommandBuffer cmd = e.commandBuffers[e.currentFrame];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);

    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{e.clearR, e.clearG, e.clearB, e.clearA}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = e.renderPass;
    rpBegin.framebuffer = e.sceneFramebuffer ? e.sceneFramebuffer : e.swapchainFramebuffers[imageIndex];
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = e.swapchainExtent;
    rpBegin.clearValueCount = static_cast<uint32_t>(clears.size());
    rpBegin.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0, 0, static_cast<float>(e.swapchainExtent.width), static_cast<float>(e.swapchainExtent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, e.swapchainExtent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    e.activeCmd = cmd;
    e.frameActive = true;
    e.vbo2DCursor = 0;
    // Particles share the same per-frame ring-buffer discipline as the 2D
    // stream: reset the write cursor once the frame's command buffer has
    // been reset, never mid-frame.
    e.vboParticleCursor = 0;
    e.warnedParticleOverflow = false;
    e.frameCounter++;

    double mx, my;
    glfwGetCursorPos(e.window, &mx, &my);
    e.mouseDeltaX = mx - e.mouseLastX;
    e.mouseDeltaY = my - e.mouseLastY;
    e.mouseLastX = mx;
    e.mouseLastY = my;

    updateLightsUbo();

    auto now = std::chrono::steady_clock::now();
    e.deltaTime = std::chrono::duration<float>(now - e.lastFrameTime).count();
    e.lastFrameTime = now;
    e.fpsAccum += e.deltaTime;
    e.fpsFrames += 1;
    if (e.fpsAccum >= 0.5f) {
        e.fps = static_cast<float>(e.fpsFrames) / e.fpsAccum;
        e.fpsAccum = 0;
        e.fpsFrames = 0;
    }

    return Value();
}

Value EndFrame() {
    auto& e = engine();
    if (!e.initialized || !e.frameActive) return Value();

    vkCmdEndRenderPass(e.activeCmd);
    recordShadowAndPost(e.activeCmd, e.currentImageIndex);
    vkEndCommandBuffer(e.activeCmd);

    VkSemaphore waitSems[] = {e.frameSync[e.currentFrame].imageAvailable};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSems[] = {e.frameSync[e.currentFrame].renderFinished};

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = waitSems;
    submit.pWaitDstStageMask = waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &e.activeCmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = signalSems;
    vkQueueSubmit(e.graphicsQueue, 1, &submit, e.frameSync[e.currentFrame].inFlight);

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = signalSems;
    present.swapchainCount = 1;
    present.pSwapchains = &e.swapchain;
    present.pImageIndices = &e.currentImageIndex;
    VkResult presentResult = vkQueuePresentKHR(e.presentQueue, &present);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        vkRequestSwapchainRecreate();
    }

    e.currentFrame = (e.currentFrame + 1) % Engine::MAX_FRAMES_IN_FLIGHT;
    e.frameActive = false;
    e.activeCmd = VK_NULL_HANDLE;
    // Cleared here (end of frame) rather than in BeginFrame: GLFW's char
    // callback fires during glfwPollEvents() inside IsWindowOpen(), which
    // runs *before* BeginFrame in the typical `while (IsWindowOpen())
    // { BeginFrame(); ...; EndFrame(); }` loop -- clearing at the start of
    // BeginFrame would wipe out this frame's keystrokes before any
    // Pv::UIInputText call got to read them.
    e.uiCharsTypedThisFrame.clear();
    return Value();
}

Value Clear(const Value& colorHex) {
    auto& e = engine();
    int64_t hex = colorHex.asInt();
    e.clearR = static_cast<float>((hex >> 16) & 0xFF) / 255.0f;
    e.clearG = static_cast<float>((hex >> 8) & 0xFF) / 255.0f;
    e.clearB = static_cast<float>(hex & 0xFF) / 255.0f;
    e.clearA = 1.0f;
    // BeginFrame already started the render pass with the *previous* clear
    // color. Scripts call Clear() inside the frame, so apply it now or the
    // first frame (and any mid-frame clear) never shows the requested color.
    if (e.frameActive && e.activeCmd) {
        VkClearAttachment attachments[2]{};
        attachments[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachments[0].colorAttachment = 0;
        attachments[0].clearValue.color.float32[0] = e.clearR;
        attachments[0].clearValue.color.float32[1] = e.clearG;
        attachments[0].clearValue.color.float32[2] = e.clearB;
        attachments[0].clearValue.color.float32[3] = e.clearA;
        attachments[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (e.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || e.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
            attachments[1].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        attachments[1].clearValue.depthStencil = {1.0f, 0};
        VkClearRect rect{};
        rect.rect.extent = e.swapchainExtent;
        rect.layerCount = 1;
        vkCmdClearAttachments(e.activeCmd, 2, attachments, 1, &rect);
    }
    return Value();
}

Value SetClearColor(const Value& r, const Value& g, const Value& b, const Value& a) {
    auto& e = engine();
    e.clearR = static_cast<float>(r.asFloat());
    e.clearG = static_cast<float>(g.asFloat());
    e.clearB = static_cast<float>(b.asFloat());
    e.clearA = static_cast<float>(a.asFloat());
    return Value();
}

Value GetDeltaTime() { return Value(static_cast<double>(engine().deltaTime)); }
Value GetFPS() { return Value(static_cast<double>(engine().fps)); }

} // namespace rt
} // namespace pv
