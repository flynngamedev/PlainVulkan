// draw2d.cpp -- Pv::DrawTriangle / DrawRect / DrawCircle / DrawLine /
// DrawSprite(Ex) / DrawText(Ex). Every call here appends a handful of
// vertices to the current frame's ring-buffer VBO and issues its draw
// immediately (see draw2DVerts in vk_core.cpp) -- that's what keeps
// draw order correct when a script interleaves flat shapes, sprites, and
// (in draw3d.cpp) 3D meshes within one frame.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

#include <cmath>
#include <vector>

namespace pv {

static void unpackColor(int64_t hex, float out[4]) {
    out[0] = static_cast<float>((hex >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((hex >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(hex & 0xFF) / 255.0f;
    out[3] = ((hex >> 24) & 0xFF) != 0 ? static_cast<float>((hex >> 24) & 0xFF) / 255.0f : 1.0f;
}

static Vertex2D V(float x, float y, const float c[4]) {
    Vertex2D v{};
    v.pos[0] = x;
    v.pos[1] = y;
    v.color[0] = c[0];
    v.color[1] = c[1];
    v.color[2] = c[2];
    v.color[3] = c[3];
    v.uv[0] = v.uv[1] = 0;
    return v;
}

namespace rt {

Value DrawTriangle(const Value& x, const Value& y, const Value& color) {
    float c[4];
    unpackColor(color.asInt(), c);
    float cx = static_cast<float>(x.asFloat()), cy = static_cast<float>(y.asFloat());
    // The reference only gives a single anchor point + color for a
    // triangle (no explicit size), so this draws a small fixed-size
    // upward-pointing triangle centered on (x, y) -- a documented
    // simplification (see runtime/README.md).
    const float r = 20.0f;
    Vertex2D verts[3] = {V(cx, cy - r, c), V(cx + r, cy + r, c), V(cx - r, cy + r, c)};
    draw2DVerts(verts, 3, false, VK_NULL_HANDLE);
    return Value();
}

Value DrawRect(const Value& x, const Value& y, const Value& w, const Value& h, const Value& color) {
    float c[4];
    unpackColor(color.asInt(), c);
    float X = static_cast<float>(x.asFloat()), Y = static_cast<float>(y.asFloat());
    float W = static_cast<float>(w.asFloat()), H = static_cast<float>(h.asFloat());
    Vertex2D verts[6] = {V(X, Y, c),         V(X + W, Y, c),     V(X + W, Y + H, c),
                          V(X + W, Y + H, c), V(X, Y + H, c),     V(X, Y, c)};
    draw2DVerts(verts, 6, false, VK_NULL_HANDLE);
    return Value();
}

Value DrawCircle(const Value& x, const Value& y, const Value& radius, const Value& color) {
    float c[4];
    unpackColor(color.asInt(), c);
    float X = static_cast<float>(x.asFloat()), Y = static_cast<float>(y.asFloat());
    float R = static_cast<float>(radius.asFloat());
    const int kSegments = 24;
    std::vector<Vertex2D> verts;
    verts.reserve(kSegments * 3);
    for (int i = 0; i < kSegments; i++) {
        float a0 = (2.0f * 3.14159265f * i) / kSegments;
        float a1 = (2.0f * 3.14159265f * (i + 1)) / kSegments;
        verts.push_back(V(X, Y, c));
        verts.push_back(V(X + cosf(a0) * R, Y + sinf(a0) * R, c));
        verts.push_back(V(X + cosf(a1) * R, Y + sinf(a1) * R, c));
    }
    draw2DVerts(verts.data(), static_cast<uint32_t>(verts.size()), false, VK_NULL_HANDLE);
    return Value();
}

Value DrawLine(const Value& x1, const Value& y1, const Value& x2, const Value& y2, const Value& color) {
    float c[4];
    unpackColor(color.asInt(), c);
    float X1 = static_cast<float>(x1.asFloat()), Y1 = static_cast<float>(y1.asFloat());
    float X2 = static_cast<float>(x2.asFloat()), Y2 = static_cast<float>(y2.asFloat());
    float dx = X2 - X1, dy = Y2 - Y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-5f) return Value();
    float nx = -dy / len * 1.0f; // half-width 1px normal
    float ny = dx / len * 1.0f;
    Vertex2D verts[6] = {
        V(X1 + nx, Y1 + ny, c), V(X2 + nx, Y2 + ny, c), V(X2 - nx, Y2 - ny, c),
        V(X2 - nx, Y2 - ny, c), V(X1 - nx, Y1 - ny, c), V(X1 + nx, Y1 + ny, c),
    };
    draw2DVerts(verts, 6, false, VK_NULL_HANDLE);
    return Value();
}

static void drawTexturedQuad(float x, float y, float w, float h, uint64_t textureHandle, const float tint[4]) {
    GpuTexture* tex = engine().textures.get(textureHandle);
    VkDescriptorSet set = tex ? tex->descriptorSet : VK_NULL_HANDLE;
    Vertex2D verts[6]{};
    struct P {
        float px, py, u, v;
    };
    P ps[6] = {{x, y, 0, 0},         {x + w, y, 1, 0},     {x + w, y + h, 1, 1},
               {x + w, y + h, 1, 1}, {x, y + h, 0, 1},     {x, y, 0, 0}};
    for (int i = 0; i < 6; i++) {
        verts[i].pos[0] = ps[i].px;
        verts[i].pos[1] = ps[i].py;
        verts[i].color[0] = tint[0];
        verts[i].color[1] = tint[1];
        verts[i].color[2] = tint[2];
        verts[i].color[3] = tint[3];
        verts[i].uv[0] = ps[i].u;
        verts[i].uv[1] = ps[i].v;
    }
    draw2DVerts(verts, 6, true, set);
}

Value DrawSprite(const Value& texture, const Value& x, const Value& y) {
    GpuTexture* tex = engine().textures.get(static_cast<uint64_t>(texture.asInt()));
    float w = tex ? static_cast<float>(tex->width) : 32.0f;
    float h = tex ? static_cast<float>(tex->height) : 32.0f;
    const float white[4] = {1, 1, 1, 1};
    drawTexturedQuad(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), w, h,
                      static_cast<uint64_t>(texture.asInt()), white);
    return Value();
}

Value DrawSpriteEx(const Value& texture, const Value& x, const Value& y, const Value& w, const Value& h) {
    const float white[4] = {1, 1, 1, 1};
    drawTexturedQuad(static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                      static_cast<float>(w.asFloat()), static_cast<float>(h.asFloat()),
                      static_cast<uint64_t>(texture.asInt()), white);
    return Value();
}

// Renders `text` using the compact built-in 3x5 bitmap font (font_data.cpp).
static void drawBitmapText(const std::string& text, float x, float y, float pixelSize, const float c[4]) {
    const float cellW = 4.0f * pixelSize; // 3 px glyph + 1 px gap
    float cursorX = x;
    std::vector<Vertex2D> verts;
    for (char ch : text) {
        if (ch == '\n') {
            cursorX = x;
            y += 6.0f * pixelSize;
            continue;
        }
        const Glyph3x5& g = lookupGlyph(ch);
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                if (g.rows[row][col] != '#') continue;
                float px = cursorX + col * pixelSize;
                float py = y + row * pixelSize;
                verts.push_back(V(px, py, c));
                verts.push_back(V(px + pixelSize, py, c));
                verts.push_back(V(px + pixelSize, py + pixelSize, c));
                verts.push_back(V(px + pixelSize, py + pixelSize, c));
                verts.push_back(V(px, py + pixelSize, c));
                verts.push_back(V(px, py, c));
            }
        }
        cursorX += cellW;
    }
    if (!verts.empty()) draw2DVerts(verts.data(), static_cast<uint32_t>(verts.size()), false, VK_NULL_HANDLE);
}

Value DrawText(const Value& text, const Value& x, const Value& y, const Value& color) {
    float c[4];
    unpackColor(color.asInt(), c);
    drawBitmapText(text.asString(), static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), 3.0f, c);
    return Value();
}

Value DrawTextEx(const Value& text, const Value& x, const Value& y, const Value& font, const Value& size,
                  const Value& color) {
    (void)font; // custom font *files* aren't implemented -- see runtime/README.md; size still works
    float c[4];
    unpackColor(color.asInt(), c);
    float pixelSize = static_cast<float>(size.asFloat()) / 7.0f;
    if (pixelSize <= 0) pixelSize = 3.0f;
    drawBitmapText(text.asString(), static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()), pixelSize, c);
    return Value();
}

} // namespace rt
} // namespace pv
