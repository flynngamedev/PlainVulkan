// input.cpp -- Pv::IsKeyDown / mouse / gamepad. All backed directly by
// GLFW polling, with a small per-frame edge-detection helper so
// IsKeyPressed/IsKeyReleased give a stable answer no matter how many
// times they're queried within the same frame (see the EdgeState comment
// below for why this needs the frame counter rather than just comparing
// against "last call").
#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

namespace pv {

// Advances `table[id]` at most once per frame: the first query for a given
// id in a new frame shifts cur -> prev and re-samples cur from `liveDown`;
// every subsequent query that frame sees the same (stable) prev/cur pair.
static Engine::EdgeState& edge(std::unordered_map<int, Engine::EdgeState>& table, int id, bool liveDown) {
    auto& st = table[id];
    if (st.lastUpdatedFrame != engine().frameCounter) {
        st.prev = st.cur;
        st.cur = liveDown;
        st.lastUpdatedFrame = engine().frameCounter;
    }
    return st;
}

namespace rt {

Value IsKeyDown(const Value& key) {
    auto& e = engine();
    if (!e.window) return Value(false);
    bool live = glfwGetKey(e.window, static_cast<int>(key.asInt())) == GLFW_PRESS;
    return Value(edge(e.keyEdge, static_cast<int>(key.asInt()), live).cur);
}
Value IsKeyUp(const Value& key) { return Value(!IsKeyDown(key).asBool()); }
Value IsKeyPressed(const Value& key) {
    auto& e = engine();
    if (!e.window) return Value(false);
    int k = static_cast<int>(key.asInt());
    bool live = glfwGetKey(e.window, k) == GLFW_PRESS;
    auto& st = edge(e.keyEdge, k, live);
    return Value(st.cur && !st.prev);
}
Value IsKeyReleased(const Value& key) {
    auto& e = engine();
    if (!e.window) return Value(false);
    int k = static_cast<int>(key.asInt());
    bool live = glfwGetKey(e.window, k) == GLFW_PRESS;
    auto& st = edge(e.keyEdge, k, live);
    return Value(!st.cur && st.prev);
}
Value GetKeyPressed() {
    int k = engine().lastKeyPressed;
    engine().lastKeyPressed = -1;
    return Value(k);
}

Value GetMouseX() { return Value(engine().mouseLastX); }
Value GetMouseY() { return Value(engine().mouseLastY); }
Value GetMouseDeltaX() { return Value(engine().mouseDeltaX); }
Value GetMouseDeltaY() { return Value(engine().mouseDeltaY); }

Value IsMouseDown(const Value& button) {
    auto& e = engine();
    if (!e.window) return Value(false);
    bool live = glfwGetMouseButton(e.window, static_cast<int>(button.asInt())) == GLFW_PRESS;
    return Value(edge(e.mouseEdge, static_cast<int>(button.asInt()), live).cur);
}
Value IsMousePressed(const Value& button) {
    auto& e = engine();
    if (!e.window) return Value(false);
    int b = static_cast<int>(button.asInt());
    bool live = glfwGetMouseButton(e.window, b) == GLFW_PRESS;
    auto& st = edge(e.mouseEdge, b, live);
    return Value(st.cur && !st.prev);
}
Value IsMouseReleased(const Value& button) {
    auto& e = engine();
    if (!e.window) return Value(false);
    int b = static_cast<int>(button.asInt());
    bool live = glfwGetMouseButton(e.window, b) == GLFW_PRESS;
    auto& st = edge(e.mouseEdge, b, live);
    return Value(!st.cur && st.prev);
}
Value GetMouseScroll() {
    double s = engine().scrollAccum;
    engine().scrollAccum = 0;
    return Value(s);
}
Value SetMouseVisible(const Value& visible) {
    if (engine().window) {
        glfwSetInputMode(engine().window, GLFW_CURSOR, visible.truthy() ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
    }
    return Value();
}
Value SetMouseLocked(const Value& locked) {
    if (engine().window) {
        glfwSetInputMode(engine().window, GLFW_CURSOR, locked.truthy() ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }
    return Value();
}
Value SetMousePosition(const Value& x, const Value& y) {
    if (engine().window) glfwSetCursorPos(engine().window, x.asFloat(), y.asFloat());
    return Value();
}

Value IsGamepadConnected(const Value& id) { return Value(static_cast<bool>(glfwJoystickIsGamepad(static_cast<int>(id.asInt())))); }

Value IsGamepadButtonDown(const Value& id, const Value& button) {
    GLFWgamepadstate state;
    if (!glfwGetGamepadState(static_cast<int>(id.asInt()), &state)) return Value(false);
    int b = static_cast<int>(button.asInt());
    if (b < 0 || b >= 15) return Value(false);
    bool live = state.buttons[b] == GLFW_PRESS;
    int combinedId = static_cast<int>(id.asInt()) * 1000 + b;
    return Value(edge(engine().gamepadEdge, combinedId, live).cur);
}
Value IsGamepadButtonPressed(const Value& id, const Value& button) {
    GLFWgamepadstate state;
    if (!glfwGetGamepadState(static_cast<int>(id.asInt()), &state)) return Value(false);
    int b = static_cast<int>(button.asInt());
    if (b < 0 || b >= 15) return Value(false);
    bool live = state.buttons[b] == GLFW_PRESS;
    int combinedId = static_cast<int>(id.asInt()) * 1000 + b;
    auto& st = edge(engine().gamepadEdge, combinedId, live);
    return Value(st.cur && !st.prev);
}
Value GetGamepadAxis(const Value& id, const Value& axis) {
    GLFWgamepadstate state;
    if (!glfwGetGamepadState(static_cast<int>(id.asInt()), &state)) return Value(0.0);
    int a = static_cast<int>(axis.asInt());
    if (a < 0 || a >= 6) return Value(0.0);
    return Value(static_cast<double>(state.axes[a]));
}
Value SetGamepadVibration(const Value& id, const Value& left, const Value& right) {
    (void)id;
    (void)left;
    (void)right;
    logLine("WARNING", "Pv::SetGamepadVibration: GLFW has no rumble API, so this is a no-op (Tier 4 gap, see "
                        "runtime/README.md)");
    return Value();
}

} // namespace rt
} // namespace pv
