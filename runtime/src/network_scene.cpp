// network_scene.cpp -- the four `Pv::` commands that sit between the
// transport (network.cpp) and the scene: turning script input and local
// entities into packets, and applying a server's authoritative state packet
// back onto local entities.
//
// These call the public `pv::rt::` scene commands rather than reaching into
// Engine/Scene directly. That keeps this file free of <vulkan/vulkan.h>
// (see the note in pv_net.h) and means it automatically inherits whatever
// invariants the scene commands maintain -- notably the world-matrix dirty
// marking that a direct write to Entity::local would silently skip.
//
// --- the state packet -----------------------------------------------------
// This is the format PlainS's PlS::BroadcastState / BroadcastStateSelective
// emits, and the one Pv::DeserializeState consumes. Both halves are
// documented together here because a mismatch between them is invisible
// until runtime:
//
//   {
//     "type": "state",
//     "tick": 1240,
//     "entities": {
//       "3": { "id": 3, "name": "Player_1", "state": "alive", "health": 100,
//              "position": [x, y, z],
//              "rotation": [pitch, yaw, roll],
//              "velocity": [vx, vy, vz] }
//     }
//   }
//
// `entities` is an object keyed by stringified server entity id rather than
// an array, because a selective broadcast sends a different subset to each
// player -- an array index would mean something different from one packet
// to the next, while the key stays stable.
#include "pv/pv_net.h"
#include "pv/pv_runtime.h"

#include <map>
#include <mutex>
#include <string>

namespace pv {
namespace rt {

namespace {

// Maps a server-side entity id to the local scene entity mirroring it.
// Held here rather than in the scene because it is meaningless without a
// connection and must be dropped wholesale when one ends.
std::map<int64_t, Value>& mirrorMap() {
    static std::map<int64_t, Value> m;
    return m;
}
std::mutex& mirrorMutex() {
    static std::mutex m;
    return m;
}

// Local mirrors are named `Net_<serverId>` so that a script (or the editor's
// scene inspector) can find a networked entity by name, and so a mirror
// survives being looked up again after a scene reload dropped the map.
std::string mirrorName(int64_t serverId) {
    return "Net_" + std::to_string(serverId);
}

// Returns the local entity mirroring `serverId`, creating it on first sight.
// Creating on demand is what makes remote players appear without any
// spawn-handling code in the script: the first state packet that mentions an
// id is also the packet that brings it into existence.
Value mirrorFor(int64_t serverId) {
    std::lock_guard<std::mutex> lock(mirrorMutex());
    auto& m = mirrorMap();
    auto it = m.find(serverId);
    if (it != m.end()) return it->second;

    std::string name = mirrorName(serverId);
    Value existing = FindEntity(Value(name));
    if (!existing.isNull() && existing.asInt() != 0) {
        m[serverId] = existing;
        return existing;
    }
    Value created = CreateEntity(Value(name));
    m[serverId] = created;
    return created;
}

// Reads element `i` of a Value that should be a 3-element array, tolerating
// a missing or short array. Packets come off the wire from another process;
// a malformed one should leave an entity where it was, not teleport it to
// the origin or crash.
double vecAt(const Value& v, size_t i, double fallback = 0.0) {
    Value e = index_get(v, Value(static_cast<int64_t>(i)));
    return e.isNumber() ? e.asFloat() : fallback;
}

bool hasVec3(const Value& v) { return v.isArray(); }

} // namespace

Value SerializeInput(const Value& action, const Value& x, const Value& y, const Value& z) {
    // Builds the input packet PlS::OnPlayerInput receives. Exists as a
    // command (rather than leaving scripts to build the object themselves)
    // because `.pv` has no object literal syntax -- this is the supported
    // way to produce a structured packet in a single expression.
    Value out = Value::MakeObject();
    auto& o = out.objectRef();
    o["action"] = action;
    o["x"] = Value(x.asFloat());
    o["y"] = Value(y.asFloat());
    o["z"] = Value(z.asFloat());
    return out;
}

Value SerializePlayerState(const Value& entity) {
    Value pos = GetEntityPosition(entity);
    Value rot = GetEntityRotation(entity);

    Value out = Value::MakeObject();
    auto& o = out.objectRef();
    o["id"] = Value(entity.asInt());
    o["position"] = pos;
    o["rotation"] = rot;
    return out;
}

Value DeserializeState(const Value& data) {
    // Accepts either a full state packet or, for convenience, the bare
    // `entities` map -- a script that has already pulled the field out of a
    // larger message shouldn't have to rebuild the wrapper to call this.
    Value entities = member_get(data, "entities");
    if (!entities.isObject()) {
        entities = data.isObject() ? data : Value();
    }
    if (!entities.isObject()) return Value(false);

    int64_t applied = 0;
    Value& mut = const_cast<Value&>(entities);
    for (auto& kv : mut.objectRef()) {
        int64_t serverId = 0;
        try {
            serverId = std::stoll(kv.first);
        } catch (...) {
            continue; // key isn't an entity id; skip rather than abort
        }
        const Value& e = kv.second;
        Value local = mirrorFor(serverId);
        if (local.isNull()) continue;

        Value pos = member_get(e, "position");
        if (hasVec3(pos)) {
            SetEntityPosition(local, Value(vecAt(pos, 0)), Value(vecAt(pos, 1)), Value(vecAt(pos, 2)));
        }
        Value rot = member_get(e, "rotation");
        if (hasVec3(rot)) {
            SetEntityRotation(local, Value(vecAt(rot, 0)), Value(vecAt(rot, 1)), Value(vecAt(rot, 2)));
        }
        // A server marking an entity dead deactivates the local mirror
        // rather than destroying it: the id can come back (respawn), and
        // destroying would invalidate any handle the script is holding.
        Value st = member_get(e, "state");
        if (st.type() == ValueType::String) {
            SetEntityActive(local, Value(st.asString() != "dead"));
        }
        ++applied;
    }
    return Value(applied > 0);
}

Value DeserializePlayerPosition(const Value& data, const Value& entity) {
    // Pulls a position out of either shape the server might have sent: a
    // single entity record (`{"position": [...]}`) or a bare `[x, y, z]`.
    Value pos = member_get(data, "position");
    if (!hasVec3(pos) && data.isArray()) pos = data;
    if (!hasVec3(pos)) return Value(false);

    SetEntityPosition(entity, Value(vecAt(pos, 0)), Value(vecAt(pos, 1)), Value(vecAt(pos, 2)));

    Value rot = member_get(data, "rotation");
    if (hasVec3(rot)) {
        SetEntityRotation(entity, Value(vecAt(rot, 0)), Value(vecAt(rot, 1)), Value(vecAt(rot, 2)));
    }
    return Value(true);
}

} // namespace rt
} // namespace pv
