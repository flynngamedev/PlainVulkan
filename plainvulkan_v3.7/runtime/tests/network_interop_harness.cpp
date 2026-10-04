// interop_harness.cpp -- links the REAL PlainVulkan client networking code
// (runtime/src/network.cpp + network_scene.cpp, unmodified) against stubs
// for the handful of scene/logging commands they call, and drives it
// against a live PlainS server.
//
// The point is to verify the two wire formats actually agree. Both sides
// were written to the same spec, but a spec agreement that is never
// executed is exactly the kind that turns out to be wrong.
#include "pv/pv_runtime.h"
#include "pv/pv_value.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>

// --- stubs for the scene/logging commands the networking code calls -------
// A minimal in-memory entity table, enough for DeserializeState to have
// somewhere to put what it decodes.
namespace {

struct StubEntity {
    std::string name;
    double px = 0, py = 0, pz = 0;
    double rx = 0, ry = 0, rz = 0;
    bool active = true;
};

std::map<uint64_t, StubEntity>& entities() {
    static std::map<uint64_t, StubEntity> m;
    return m;
}
uint64_t g_nextEntity = 1;

} // namespace

namespace pv {
namespace rt {

Value Log(const Value& msg) {
    std::printf("  [log] %s\n", msg.asString().c_str());
    return Value();
}
Value LogWarning(const Value& msg) {
    std::printf("  [warn] %s\n", msg.asString().c_str());
    return Value();
}
Value LogError(const Value& msg) {
    std::printf("  [error] %s\n", msg.asString().c_str());
    return Value();
}

Value CreateEntity(const Value& name) {
    uint64_t id = g_nextEntity++;
    entities()[id].name = name.asString();
    return Value(static_cast<int64_t>(id));
}

Value FindEntity(const Value& name) {
    for (auto& kv : entities()) {
        if (kv.second.name == name.asString()) return Value(static_cast<int64_t>(kv.first));
    }
    return Value();
}

Value SetEntityPosition(const Value& e, const Value& x, const Value& y, const Value& z) {
    auto it = entities().find(static_cast<uint64_t>(e.asInt()));
    if (it == entities().end()) return Value(false);
    it->second.px = x.asFloat();
    it->second.py = y.asFloat();
    it->second.pz = z.asFloat();
    return Value(true);
}

Value GetEntityPosition(const Value& e) {
    auto it = entities().find(static_cast<uint64_t>(e.asInt()));
    if (it == entities().end()) return make_array({Value(0.0), Value(0.0), Value(0.0)});
    return make_array({Value(it->second.px), Value(it->second.py), Value(it->second.pz)});
}

Value SetEntityRotation(const Value& e, const Value& p, const Value& y, const Value& r) {
    auto it = entities().find(static_cast<uint64_t>(e.asInt()));
    if (it == entities().end()) return Value(false);
    it->second.rx = p.asFloat();
    it->second.ry = y.asFloat();
    it->second.rz = r.asFloat();
    return Value(true);
}

Value GetEntityRotation(const Value& e) {
    auto it = entities().find(static_cast<uint64_t>(e.asInt()));
    if (it == entities().end()) return make_array({Value(0.0), Value(0.0), Value(0.0)});
    return make_array({Value(it->second.rx), Value(it->second.ry), Value(it->second.rz)});
}

Value SetEntityActive(const Value& e, const Value& active) {
    auto it = entities().find(static_cast<uint64_t>(e.asInt()));
    if (it == entities().end()) return Value(false);
    it->second.active = active.truthy();
    return Value(true);
}

} // namespace rt
} // namespace pv

// --- the callbacks, as a .pv script's top-level functions would compile ---
static int g_connected = 0;
static int g_received = 0;
static std::string g_lastDisconnectReason;
// The id the server assigned us, taken from the welcome packet. It is NOT
// always 1: the server hands out ids in connection order and never reuses
// them, so anything that touched the port first (a health check, a previous
// harness run against a still-running server) shifts it. Hardcoding 1 here
// made this harness report a wire-format failure when the only thing that
// had actually happened was that someone else connected first.
static int64_t g_serverId = 0;

pv::Value OnConnected() {
    g_connected++;
    std::printf("  OnConnect fired\n");
    return pv::Value();
}

pv::Value OnMessage(pv::Value data) {
    g_received++;
    pv::Value type = pv::member_get(data, "type");
    if (g_received <= 2) {
        std::printf("  OnReceive fired: type=%s\n", type.asString().c_str());
    }
    if (type.asString() == "welcome") {
        // "player_id" is the real contract (mp_server's OnConnect). The
        // other two are accepted so an older mock server, or one written
        // against the camelCase spelling, still drives this harness rather
        // than failing three checks that have nothing to do with the wire
        // format under test.
        pv::Value id = pv::member_get(data, "player_id");
        if (id.isNull()) id = pv::member_get(data, "playerId");
        if (id.isNull()) id = pv::member_get(data, "id");
        if (!id.isNull()) g_serverId = id.asInt();
    }
    pv::rt::DeserializeState(data);
    return pv::Value();
}

pv::Value OnLost(pv::Value reason) {
    g_lastDisconnectReason = reason.asString();
    std::printf("  OnDisconnect fired: %s\n", g_lastDisconnectReason.c_str());
    return pv::Value();
}

static int g_pass = 0, g_total = 0;
static void ok(bool cond, const std::string& msg) {
    ++g_total;
    if (cond) ++g_pass;
    std::printf("  %s  %s\n", cond ? "PASS " : "FAIL ", msg.c_str());
}

static void pumpFor(int ms) {
    // Pv::PollMessages dispatches queued callbacks, standing in for the
    // Pv::BeginFrame call a real game loop would make each frame.
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        pv::rt::PollMessages();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    using namespace pv;

    std::printf("[1] Pv::Connect to the PlainS server\n");
    Value conn = rt::Connect(Value(std::string("127.0.0.1")), Value(static_cast<int64_t>(8080)));
    ok(!conn.isNull(), "Connect returned a connection handle");
    ok(rt::IsConnected().truthy(), "Pv::IsConnected() is true");
    ok(rt::GetConnectionStatus().asString() == "connected",
       "Pv::GetConnectionStatus() == \"connected\"");

    rt::OnConnect(OnConnected);
    rt::OnReceive(OnMessage);
    rt::OnDisconnect(OnLost);
    pumpFor(300);
    ok(g_connected == 1, "OnConnect callback was dispatched exactly once");
    ok(g_received > 0, "OnReceive delivered the welcome + state packets");

    std::printf("[2] Pv::SerializeInput -> Pv::Send, server applies it\n");
    for (int i = 0; i < 10; ++i) {
        Value packet = rt::SerializeInput(Value(std::string("move")), Value(1.0), Value(0.0),
                                          Value(0.0));
        rt::Send(packet);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    pumpFor(500);

    // DeserializeState auto-creates a local mirror named Net_<serverId>,
    // where serverId is the id the welcome packet assigned us.
    ok(g_serverId > 0, "welcome packet carried a server-assigned player id (" +
                           std::to_string(g_serverId) + ")");
    const std::string mirrorName = "Net_" + std::to_string(g_serverId);
    Value mirror = rt::FindEntity(Value(mirrorName));
    ok(!mirror.isNull(),
       "DeserializeState auto-created local mirror entity " + mirrorName);
    Value pos = rt::GetEntityPosition(mirror);
    double x = index_get(pos, Value(static_cast<int64_t>(0))).asFloat();
    std::printf("  mirrored position x = %.3f\n", x);
    ok(x > 0.9 && x < 1.1, "server-computed position round-tripped into the local entity");

    std::printf("[3] latency probe over the shared PING/PONG frames\n");
    // The client pings once a second; wait for at least one round trip.
    pumpFor(1600);
    double latency = rt::GetLatency().asFloat();
    std::printf("  Pv::GetLatency() = %.3f ms\n", latency);
    ok(latency > 0.0 && latency < 1000.0, "GetLatency() reports a plausible round-trip time");
    ok(rt::GetPacketLoss().asFloat() == 0.0, "GetPacketLoss() is 0 on a healthy loopback link");
    ok(rt::GetBytesSent().asInt() > 0 && rt::GetBytesReceived().asInt() > 0,
       "byte counters advanced (sent=" + std::to_string(rt::GetBytesSent().asInt()) +
           " recv=" + std::to_string(rt::GetBytesReceived().asInt()) + ")");

    std::printf("[4] Pv::GetNetworkStats aggregates the above\n");
    Value stats = rt::GetNetworkStats();
    ok(stats.isObject() && !member_get(stats, "latency").isNull() &&
           member_get(stats, "status").asString() == "connected",
       "GetNetworkStats() returns a populated object");

    std::printf("[5] Pv::Disconnect fires OnDisconnect\n");
    rt::Disconnect();
    pumpFor(200);
    ok(!g_lastDisconnectReason.empty(), "OnDisconnect fired on a local disconnect");
    ok(!rt::IsConnected().truthy(), "IsConnected() is false after Disconnect()");

    std::printf("\n%d/%d checks passed\n", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
