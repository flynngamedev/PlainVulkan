// ui.cpp -- Pv::BeginUI / UIButton / UIText / UIImage / UISlider /
// UICheckbox / UIInputText / UIProgressBar. A small real immediate-mode UI
// layer built entirely on top of draw2d.cpp's primitives (rects/text/
// sprites) and input.cpp's mouse queries -- genuinely interactive, just
// deliberately simple (no theming, no nested layout containers). See
// runtime/README.md.
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

namespace pv {

static bool pointInRect(double px, double py, float x, float y, float w, float h) {
    return px >= x && px <= x + w && py >= y && py <= y + h;
}

namespace rt {

Value BeginUI() {
    engine().uiActive = true;
    return Value();
}
Value EndUI() {
    engine().uiActive = false;
    return Value();
}

Value UIButton(const Value& label, const Value& x, const Value& y, const Value& w, const Value& h) {
    float X = static_cast<float>(x.asFloat()), Y = static_cast<float>(y.asFloat());
    float W = static_cast<float>(w.asFloat()), H = static_cast<float>(h.asFloat());
    double mx = GetMouseX().asFloat(), my = GetMouseY().asFloat();
    bool hovered = pointInRect(mx, my, X, Y, W, H);
    bool clicked = hovered && IsMousePressed(Value(0)).truthy();

    int64_t bg = hovered ? 0x5588CC : 0x336699;
    DrawRect(Value(static_cast<double>(X)), Value(static_cast<double>(Y)), Value(static_cast<double>(W)),
             Value(static_cast<double>(H)), Value(bg));
    DrawText(label, Value(static_cast<double>(X + 6)), Value(static_cast<double>(Y + H / 2 - 3)), Value(0xFFFFFF));
    return Value(clicked);
}

Value UIText(const Value& text, const Value& x, const Value& y, const Value& size, const Value& color) {
    return DrawTextEx(text, x, y, Value(""), size, color);
}

Value UIImage(const Value& texture, const Value& x, const Value& y, const Value& w, const Value& h) {
    return DrawSpriteEx(texture, x, y, w, h);
}

Value UISlider(const Value& label, const Value& x, const Value& y, const Value& min, const Value& max,
               const Value& value) {
    float X = static_cast<float>(x.asFloat()), Y = static_cast<float>(y.asFloat());
    float mn = static_cast<float>(min.asFloat()), mx_ = static_cast<float>(max.asFloat());
    float val = static_cast<float>(value.asFloat());
    const float W = 160, H = 18;

    double mx = GetMouseX().asFloat(), my = GetMouseY().asFloat();
    bool dragging = pointInRect(mx, my, X, Y, W, H) && IsMouseDown(Value(0)).truthy();
    if (dragging) {
        float t = static_cast<float>((mx - X) / W);
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        val = mn + t * (mx_ - mn);
    }

    DrawRect(Value(static_cast<double>(X)), Value(static_cast<double>(Y)), Value(static_cast<double>(W)),
             Value(static_cast<double>(H)), Value(0x222222));
    float t = (mx_ > mn) ? (val - mn) / (mx_ - mn) : 0.0f;
    DrawRect(Value(static_cast<double>(X)), Value(static_cast<double>(Y)), Value(static_cast<double>(W * t)),
             Value(static_cast<double>(H)), Value(0x66AAFF));
    DrawText(label, Value(static_cast<double>(X)), Value(static_cast<double>(Y - 12)), Value(0xFFFFFF));
    return Value(static_cast<double>(val));
}

Value UICheckbox(const Value& label, const Value& x, const Value& y, const Value& checked) {
    float X = static_cast<float>(x.asFloat()), Y = static_cast<float>(y.asFloat());
    const float S = 16;
    double mx = GetMouseX().asFloat(), my = GetMouseY().asFloat();
    bool clicked = pointInRect(mx, my, X, Y, S, S) && IsMousePressed(Value(0)).truthy();
    bool newState = clicked ? !checked.truthy() : checked.truthy();

    DrawRect(Value(static_cast<double>(X)), Value(static_cast<double>(Y)), Value(static_cast<double>(S)),
             Value(static_cast<double>(S)), Value(0x222222));
    if (newState) {
        DrawRect(Value(static_cast<double>(X + 3)), Value(static_cast<double>(Y + 3)), Value(static_cast<double>(S - 6)),
                 Value(static_cast<double>(S - 6)), Value(0x66FF88));
    }
    DrawText(label, Value(static_cast<double>(X + S + 6)), Value(static_cast<double>(Y + 2)), Value(0xFFFFFF));
    return Value(newState);
}

Value UIInputText(const Value& label, const Value& x, const Value& y, const Value& buffer) {
    auto& e = engine();
    float X = static_cast<float>(x.asFloat()), Y = static_cast<float>(y.asFloat());
    const float W = 160, H = 20;
    std::string key = label.asString();

    if (e.uiTextBuffers.find(key) == e.uiTextBuffers.end()) {
        e.uiTextBuffers[key] = buffer.asString();
    }
    std::string& text = e.uiTextBuffers[key];

    double mx = GetMouseX().asFloat(), my = GetMouseY().asFloat();
    if (pointInRect(mx, my, X, Y, W, H) && IsMousePressed(Value(0)).truthy()) {
        e.uiFocusedLabel = key;
    }
    if (e.uiFocusedLabel == key) {
        text += e.uiCharsTypedThisFrame;
        if (IsKeyPressed(Value(GLFW_KEY_BACKSPACE)).truthy() && !text.empty()) text.pop_back();
    }

    DrawRect(Value(static_cast<double>(X)), Value(static_cast<double>(Y)), Value(static_cast<double>(W)),
             Value(static_cast<double>(H)), Value(e.uiFocusedLabel == key ? 0x333355 : 0x222222));
    DrawText(Value(text), Value(static_cast<double>(X + 4)), Value(static_cast<double>(Y + 6)), Value(0xFFFFFF));
    DrawText(label, Value(static_cast<double>(X)), Value(static_cast<double>(Y - 12)), Value(0xAAAAAA));
    return Value(text);
}

Value UIProgressBar(const Value& x, const Value& y, const Value& w, const Value& h, const Value& value) {
    float X = static_cast<float>(x.asFloat()), Y = static_cast<float>(y.asFloat());
    float W = static_cast<float>(w.asFloat()), H = static_cast<float>(h.asFloat());
    float v = static_cast<float>(value.asFloat());
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    DrawRect(Value(static_cast<double>(X)), Value(static_cast<double>(Y)), Value(static_cast<double>(W)),
             Value(static_cast<double>(H)), Value(0x222222));
    DrawRect(Value(static_cast<double>(X)), Value(static_cast<double>(Y)), Value(static_cast<double>(W * v)),
             Value(static_cast<double>(H)), Value(0x44CC66));
    return Value();
}

} // namespace rt
} // namespace pv
