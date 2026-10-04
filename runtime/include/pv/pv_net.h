// pv_net.h -- internal plumbing shared between network.cpp (the transport:
// sockets, framing, threads) and network_scene.cpp (the three commands that
// map a received state packet onto scene entities).
//
// Deliberately does NOT include pv_internal.h. That header pulls in
// <vulkan/vulkan.h> and <GLFW/glfw3.h>, and nothing about the networking
// layer needs a graphics context -- keeping the dependency out means the
// whole networking half of the runtime compiles (and can be unit-tested)
// without a Vulkan SDK present, which is also what lets it share a wire
// format with the PlainS server, a program that has no renderer at all.
#pragma once

#include "pv/pv_json.h"
#include "pv/pv_value.h"

#include <cstdint>
#include <string>

namespace pv {
namespace net {

// --- wire format --------------------------------------------------------
//
// Every message on the wire is one frame:
//
//   [0..4)   uint32, big-endian: byte length of everything that follows
//   [4]      uint8: frame kind (below)
//   [5..)    payload, `length - 1` bytes
//
// A 4-byte length prefix (rather than a delimiter such as '\n') is what
// makes SendRaw able to carry arbitrary binary payloads: there is no byte
// sequence a payload could contain that would be mistaken for a boundary,
// so no escaping is needed anywhere in the stack.
//
// The kind byte is what lets one connection carry script traffic and the
// latency probe together without the probe ever surfacing to a script as a
// spurious Pv::Receive() result.
enum FrameKind : uint8_t {
    FRAME_JSON = 0, // payload is UTF-8 JSON; surfaces to scripts as a Value
    FRAME_RAW = 1,  // payload is opaque bytes; surfaces as a Value string
    FRAME_PING = 2, // payload is uint64 big-endian microsecond stamp
    FRAME_PONG = 3, // payload is the stamp from the PING, echoed verbatim
    FRAME_HANDSHAKE = 4, // cleartext X25519 public key + session token (see pv_crypto.h)
    FRAME_AEAD = 5,      // XChaCha20-Poly1305 wrapper around an inner frame
};

// Frames larger than this are refused rather than allocated for. Without a
// cap, a single corrupt or hostile length prefix (0xFFFFFFFF) would ask the
// receiver for a 4GB allocation before any payload validation could run.
constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

// --- Value <-> JSON -----------------------------------------------------
// Handles serialize as their integer id: an entity id has to survive the
// round trip to a server process that has no pv::Handle type, and the id is
// the only part of a Handle that is meaningful outside this process anyway.
json::Json valueToJson(const Value& v);
Value jsonToValue(const json::Json& j);

// Runs any queued OnConnect/OnDisconnect/OnReceive/OnNetworkError handlers
// on the *calling* thread. Handlers are queued by the background network
// thread and never invoked from it, so script code -- which is not thread
// safe and freely touches scene state -- only ever runs on the main thread.
// Called from Pv::BeginFrame and from every blocking/polling Pv:: network
// command, so a normal game loop dispatches handlers once per frame.
void pumpCallbacks();

// Closes sockets and joins the network thread. Idempotent.
void shutdownTransport();

} // namespace net
} // namespace pv
