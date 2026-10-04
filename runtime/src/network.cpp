// network.cpp -- the `Pv::` client networking commands: connection,
// send/receive, callbacks, latency instrumentation, and the optional
// peer-to-peer listener.
//
// Design notes
// ------------
// * One background thread owns every socket read. Scripts never block on
//   the network unless they explicitly ask to (Pv::Receive /
//   Pv::ReceiveTimeout); everything else is a queue inspection. This is
//   what keeps a 60fps render loop from being coupled to a peer's TCP
//   window.
//
// * Script callbacks are *queued* by that thread and invoked on the main
//   thread by net::pumpCallbacks() (called from Pv::BeginFrame and from
//   every Pv:: network command). Running a script handler on the network
//   thread would let it touch scene entities concurrently with the
//   renderer, which is exactly the kind of race that shows up as a
//   one-in-a-thousand crash rather than a test failure.
//
// * Callbacks are raw function pointers (`Value(*)()` / `Value(*)(Value)`),
//   not pv::Value, because a Value cannot hold a callable -- see the
//   Expr::Scope warning in the compiler's codegen.rs. A top-level `.pv`
//   function compiles to a real C++ function, so `Pv::OnReceive(OnMessage)`
//   passes its address directly and needs no codegen support at all. The
//   consequence, documented in the reference: a callback must be a
//   top-level function, since a function nested inside a block compiles to
//   a capturing std::function that has no function-pointer form.
//
// * TCP, not UDP. The API the reference describes (ordered messages, a
//   connection with a status, clean disconnect notification) is a stream
//   API, and games at this level of the stack are far better served by
//   correct-and-ordered than by lowest-possible-latency. TCP_NODELAY is set
//   on every socket, since Nagle's algorithm and a 60Hz input stream
//   interact badly (it holds small writes back waiting for an ACK).
//   Pv::GetPacketLoss is derived from unanswered latency probes rather than
//   claiming a datagram loss rate TCP would never expose -- see below.
#include "pv/pv_crypto.h"
#include "pv/pv_net.h"
#include "pv/pv_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET pv_socket_t;
#define PV_INVALID_SOCKET INVALID_SOCKET
#define pv_close_socket closesocket
#define pv_sock_errno WSAGetLastError()
#define PV_WOULDBLOCK WSAEWOULDBLOCK
#define PV_INPROGRESS WSAEWOULDBLOCK
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int pv_socket_t;
#define PV_INVALID_SOCKET (-1)
#define pv_close_socket ::close
#define pv_sock_errno errno
#define PV_WOULDBLOCK EWOULDBLOCK
#define PV_INPROGRESS EINPROGRESS
#endif

namespace pv {
namespace net {

namespace {

// --- platform shims -----------------------------------------------------

// Winsock needs an explicit per-process startup, and it refcounts, so the
// pairing with WSACleanup has to be exact. A function-local static gets
// both properties for free: initialized once, on first use, thread-safely.
struct SocketSubsystem {
    SocketSubsystem() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
    ~SocketSubsystem() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

void ensureSocketSubsystem() {
    static SocketSubsystem instance;
    (void)instance;
}

bool setNonBlocking(pv_socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void setNoDelay(pv_socket_t s) {
    int yes = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
}

uint64_t nowMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// --- framing ------------------------------------------------------------

void appendU32BE(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

uint32_t readU32BE(const char* p) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[3])));
}

void appendU64BE(std::string& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

uint64_t readU64BE(const std::string& s, size_t off) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && off + i < s.size(); ++i) {
        v = (v << 8) | static_cast<unsigned char>(s[off + i]);
    }
    return v;
}

std::string buildFrame(FrameKind kind, const std::string& payload) {
    std::string out;
    out.reserve(payload.size() + 5);
    appendU32BE(out, static_cast<uint32_t>(payload.size() + 1));
    out.push_back(static_cast<char>(kind));
    out += payload;
    return out;
}

// --- per-socket state ---------------------------------------------------

// One connection's receive-side accumulator. TCP is a stream, so a single
// recv() can deliver half a frame, three frames, or three-and-a-half; this
// buffers until at least one whole frame is present and hands out frames
// one at a time.
struct RxStream {
    std::string buf;

    // Returns true and fills kind/payload when a complete frame is ready.
    // `oversize` is set when the length prefix exceeds kMaxFrameBytes, which
    // the caller treats as a fatal protocol error for that connection: the
    // stream is desynchronized and cannot be resynchronized, since there is
    // no in-band way to find where the next frame starts.
    bool next(FrameKind& kind, std::string& payload, bool& oversize) {
        oversize = false;
        if (buf.size() < 4) return false;
        uint32_t len = readU32BE(buf.data());
        if (len == 0 || len > kMaxFrameBytes) {
            oversize = true;
            return false;
        }
        if (buf.size() < 4 + static_cast<size_t>(len)) return false;
        kind = static_cast<FrameKind>(static_cast<unsigned char>(buf[4]));
        payload.assign(buf, 5, static_cast<size_t>(len) - 1);
        buf.erase(0, 4 + static_cast<size_t>(len));
        return true;
    }
};

enum class PendingKind { Connect, Disconnect, Receive, Error };

struct PendingCallback {
    PendingKind kind;
    Value payload; // Receive: the message. Disconnect/Error: a string reason.
};

struct Peer {
    pv_socket_t fd = PV_INVALID_SOCKET;
    RxStream rx;
    std::string addr;
};

struct NetState {
    std::mutex mtx;
    std::condition_variable inboxCv;

    // --- outbound connection to a server ---
    pv_socket_t sock = PV_INVALID_SOCKET;
    RxStream rx;
    std::string status = "disconnected"; // connecting | connected | disconnected | error
    std::string lastError;
    std::string remoteAddr;
    crypto::Session session;

    // --- optional inbound listener (peer-to-peer mode) ---
    pv_socket_t listenSock = PV_INVALID_SOCKET;
    std::map<int, Peer> peers;
    std::deque<int> pendingAccepts; // peer ids accepted by the net thread
    int nextPeerId = 1;

    // --- queues handed from the network thread to the main thread ---
    std::deque<Value> inbox;
    std::deque<std::pair<int, Value>> peerInbox;
    std::deque<PendingCallback> callbackQueue;

    // --- script callbacks (raw function pointers; see file header) ---
    // Set when a connection succeeded while no OnConnect handler was
    // registered yet. The API reference's own example connects FIRST and
    // registers handlers immediately after:
    //     Pv::Connect(host, port);
    //     Pv::OnConnect(OnServerConnect);
    // Without this, that ordering -- the natural one, and the documented
    // one -- would mean OnConnect never fires at all, because the event had
    // already come and gone with nobody listening.
    bool connectPending = false;
    rt::NetCallback onConnect = nullptr;
    rt::NetCallback1 onDisconnect = nullptr;
    rt::NetCallback1 onReceive = nullptr;
    rt::NetCallback1 onNetworkError = nullptr;

    // --- statistics ---
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<uint64_t> pingsSent{0};
    std::atomic<uint64_t> pongsReceived{0};
    std::atomic<uint64_t> latencyMicros{0};
    std::atomic<bool> debugLogging{false};

    // --- thread control ---
    std::thread thread;
    std::atomic<bool> running{false};
    uint64_t lastPingMicros = 0;

    // Pv::Shutdown() joins the network thread, but a script is not obliged
    // to call it -- a tool, a test, or a script that simply returns from
    // main() will not. A std::thread that is still joinable when its
    // destructor runs calls std::terminate, so without this the process
    // aborts at exit ("terminate called without an active exception")
    // instead of ending cleanly. Joining here makes correct teardown the
    // default rather than something the script has to remember.
    //
    // Safe against the static-destruction order: the body runs before any
    // member is destroyed, so the thread is stopped and joined while every
    // field it touches is still alive. Bounded by the network thread's 50ms
    // select timeout.
    ~NetState() {
        running.store(false);
        if (thread.joinable()) thread.join();
    }
};

NetState& state() {
    static NetState s;
    return s;
}

void debugLog(const std::string& msg) {
    if (state().debugLogging.load()) rt::Log(Value("[net] " + msg));
}

// Queues a handler for the main thread. Caller must hold the lock.
void queueCallback(NetState& s, PendingKind kind, Value payload) {
    // Only queue what someone is listening for, so a script that uses the
    // polling style (Pv::PollMessages) never accumulates an unbounded queue
    // of undeliverable callbacks.
    switch (kind) {
        case PendingKind::Connect:    if (!s.onConnect) return; break;
        case PendingKind::Disconnect: if (!s.onDisconnect) return; break;
        case PendingKind::Receive:    if (!s.onReceive) return; break;
        case PendingKind::Error:      if (!s.onNetworkError) return; break;
    }
    s.callbackQueue.push_back(PendingCallback{kind, std::move(payload)});
}

// Writes a whole buffer, retrying on partial writes and on a full socket
// send buffer. Caller must hold the lock (serializing writers is what keeps
// two threads from interleaving halves of two frames on one socket).
bool sendAll(pv_socket_t fd, const std::string& data) {
    size_t off = 0;
    int spins = 0;
    while (off < data.size()) {
        int n = static_cast<int>(
            ::send(fd, data.data() + off, static_cast<int>(data.size() - off), 0));
        if (n > 0) {
            off += static_cast<size_t>(n);
            spins = 0;
            continue;
        }
        int err = pv_sock_errno;
        if (n < 0 && (err == PV_WOULDBLOCK
#ifndef _WIN32
                      || err == EINTR || err == EAGAIN
#endif
                      )) {
            // The kernel's send buffer is full: the peer isn't draining as
            // fast as we're producing. Yield briefly rather than spinning a
            // core, and give up after ~2s so a wedged peer can't block a
            // script's frame indefinitely.
            if (++spins > 2000) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return false;
    }
    state().bytesSent.fetch_add(data.size());
    return true;
}

void closeServerConnection(NetState& s, const std::string& reason, bool isError);

bool sendKindLocked(NetState& s, pv_socket_t fd, FrameKind kind, const std::string& payload) {
    if (kind == FRAME_HANDSHAKE) {
        return sendAll(fd, buildFrame(kind, payload));
    }
    if (!s.session.ready) return false;
    std::string aead;
    if (!crypto::seal(s.session, static_cast<uint8_t>(kind), payload, aead)) return false;
    return sendAll(fd, buildFrame(FRAME_AEAD, aead));
}

// Turns a received frame into whatever the script should observe.
// Caller must hold the lock.
void deliverInner(NetState& s, FrameKind kind, const std::string& payload, int peerId) {
    switch (kind) {
        case FRAME_PING: {
            std::string framePayload;
            if (peerId == 0) {
                sendKindLocked(s, s.sock, FRAME_PONG, payload);
            } else {
                auto it = s.peers.find(peerId);
                if (it != s.peers.end()) sendAll(it->second.fd, buildFrame(FRAME_PONG, payload));
            }
            return;
        }
        case FRAME_PONG: {
            uint64_t sentAt = readU64BE(payload, 0);
            uint64_t now = nowMicros();
            if (now >= sentAt) s.latencyMicros.store(now - sentAt);
            s.pongsReceived.fetch_add(1);
            return;
        }
        case FRAME_JSON: {
            std::string err;
            json::Json j = json::Json::parse(payload, &err);
            if (!err.empty()) {
                queueCallback(s, PendingKind::Error, Value("malformed JSON from peer: " + err));
                return;
            }
            Value v = jsonToValue(j);
            if (peerId != 0) {
                s.peerInbox.emplace_back(peerId, v);
            } else if (s.onReceive) {
                queueCallback(s, PendingKind::Receive, v);
            } else {
                s.inbox.push_back(v);
            }
            s.inboxCv.notify_all();
            return;
        }
        case FRAME_RAW: {
            Value v(payload);
            if (peerId != 0) {
                s.peerInbox.emplace_back(peerId, v);
            } else if (s.onReceive) {
                queueCallback(s, PendingKind::Receive, v);
            } else {
                s.inbox.push_back(v);
            }
            s.inboxCv.notify_all();
            return;
        }
        default:
            break;
    }
    debugLog("ignoring inner frame kind " + std::to_string(static_cast<int>(kind)));
}

void deliverFrame(NetState& s, FrameKind kind, const std::string& payload, int peerId) {
    if (peerId == 0 && kind == FRAME_HANDSHAKE) {
        if (s.session.ready) return;
        if (!crypto::parseHandshake(payload, s.session)) {
            closeServerConnection(s, "malformed handshake", true);
            return;
        }
        crypto::deriveKeys(s.session);
        s.status = "connected";
        if (s.onConnect) {
            queueCallback(s, PendingKind::Connect, Value());
        } else {
            s.connectPending = true;
        }
        s.inboxCv.notify_all();
        debugLog("session established token=" + crypto::tokenHex(s.session));
        return;
    }

    if (peerId == 0) {
        if (!s.session.ready) {
            closeServerConnection(s, "expected handshake from server", true);
            return;
        }
        if (kind != FRAME_AEAD) {
            closeServerConnection(s, "plaintext frame after handshake", true);
            return;
        }
        uint8_t inner = 0;
        std::string innerPayload;
        if (!crypto::open(s.session, payload, inner, innerPayload)) {
            closeServerConnection(s, "replay, clock skew, or MAC failure", true);
            return;
        }
        deliverInner(s, static_cast<FrameKind>(inner), innerPayload, peerId);
        return;
    }

    // P2P remains unencrypted (no handshake); used by tests and Listen().
    deliverInner(s, kind, payload, peerId);
}

// Tears the server connection down. Caller must hold the lock.
void closeServerConnection(NetState& s, const std::string& reason, bool isError) {
    if (s.sock != PV_INVALID_SOCKET) {
        pv_close_socket(s.sock);
        s.sock = PV_INVALID_SOCKET;
    }
    s.rx.buf.clear();
    crypto::wipe(s.session);
    s.status = isError ? "error" : "disconnected";
    if (isError) s.lastError = reason;
    queueCallback(s, PendingKind::Disconnect, Value(reason));
    if (isError) queueCallback(s, PendingKind::Error, Value(reason));
    s.inboxCv.notify_all();
}

void closePeer(NetState& s, int peerId) {
    auto it = s.peers.find(peerId);
    if (it == s.peers.end()) return;
    if (it->second.fd != PV_INVALID_SOCKET) pv_close_socket(it->second.fd);
    s.peers.erase(it);
}

// --- the network thread -------------------------------------------------

void netThreadMain() {
    NetState& s = state();
    std::string payload;
    while (s.running.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        pv_socket_t maxFd = 0;
        {
            std::lock_guard<std::mutex> lock(s.mtx);
            if (s.sock != PV_INVALID_SOCKET) {
                FD_SET(s.sock, &readSet);
                maxFd = std::max(maxFd, s.sock);
            }
            if (s.listenSock != PV_INVALID_SOCKET) {
                FD_SET(s.listenSock, &readSet);
                maxFd = std::max(maxFd, s.listenSock);
            }
            for (auto& kv : s.peers) {
                FD_SET(kv.second.fd, &readSet);
                maxFd = std::max(maxFd, kv.second.fd);
            }
        }

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000; // 50ms: bounds shutdown latency and paces pings
        int ready = ::select(static_cast<int>(maxFd) + 1, &readSet, nullptr, nullptr, &tv);
        if (!s.running.load()) break;

        // --- periodic latency probe ---
        {
            std::lock_guard<std::mutex> lock(s.mtx);
            uint64_t now = nowMicros();
            if (s.sock != PV_INVALID_SOCKET && s.status == "connected" && s.session.ready &&
                now - s.lastPingMicros > 1000 * 1000) {
                s.lastPingMicros = now;
                std::string stamp;
                appendU64BE(stamp, now);
                if (sendKindLocked(s, s.sock, FRAME_PING, stamp)) {
                    s.pingsSent.fetch_add(1);
                }
            }
        }

        if (ready <= 0) continue;

        // --- inbound peer connections ---
        {
            std::lock_guard<std::mutex> lock(s.mtx);
            if (s.listenSock != PV_INVALID_SOCKET && FD_ISSET(s.listenSock, &readSet)) {
                sockaddr_in addr{};
#ifdef _WIN32
                int addrLen = sizeof(addr);
#else
                socklen_t addrLen = sizeof(addr);
#endif
                pv_socket_t c = ::accept(s.listenSock, reinterpret_cast<sockaddr*>(&addr), &addrLen);
                if (c != PV_INVALID_SOCKET) {
                    setNonBlocking(c);
                    setNoDelay(c);
                    int id = s.nextPeerId++;
                    Peer p;
                    p.fd = c;
                    char ipbuf[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
                    p.addr = std::string(ipbuf) + ":" + std::to_string(ntohs(addr.sin_port));
                    s.peers[id] = std::move(p);
                    s.pendingAccepts.push_back(id);
                    debugLog("peer " + std::to_string(id) + " connected from " + s.peers[id].addr);
                }
            }
        }

        // --- server socket readable ---
        {
            std::unique_lock<std::mutex> lock(s.mtx);
            if (s.sock != PV_INVALID_SOCKET && FD_ISSET(s.sock, &readSet)) {
                char buf[8192];
                int n = static_cast<int>(::recv(s.sock, buf, sizeof(buf), 0));
                if (n > 0) {
                    s.bytesReceived.fetch_add(static_cast<uint64_t>(n));
                    s.rx.buf.append(buf, static_cast<size_t>(n));
                    FrameKind kind;
                    bool oversize = false;
                    while (s.rx.next(kind, payload, oversize)) {
                        deliverFrame(s, kind, payload, 0);
                    }
                    if (oversize) {
                        closeServerConnection(s, "protocol error: frame length out of range", true);
                    }
                } else if (n == 0) {
                    closeServerConnection(s, "closed by remote host", false);
                } else {
                    int err = pv_sock_errno;
                    if (err != PV_WOULDBLOCK
#ifndef _WIN32
                        && err != EAGAIN && err != EINTR
#endif
                    ) {
                        closeServerConnection(s, "recv failed (errno " + std::to_string(err) + ")",
                                              true);
                    }
                }
            }
        }

        // --- peer sockets readable ---
        {
            std::lock_guard<std::mutex> lock(s.mtx);
            std::vector<int> dead;
            for (auto& kv : s.peers) {
                if (!FD_ISSET(kv.second.fd, &readSet)) continue;
                char buf[8192];
                int n = static_cast<int>(::recv(kv.second.fd, buf, sizeof(buf), 0));
                if (n > 0) {
                    s.bytesReceived.fetch_add(static_cast<uint64_t>(n));
                    kv.second.rx.buf.append(buf, static_cast<size_t>(n));
                    FrameKind kind;
                    bool oversize = false;
                    while (kv.second.rx.next(kind, payload, oversize)) {
                        deliverFrame(s, kind, payload, kv.first);
                    }
                    if (oversize) dead.push_back(kv.first);
                } else if (n == 0) {
                    dead.push_back(kv.first);
                } else {
                    int err = pv_sock_errno;
                    if (err != PV_WOULDBLOCK
#ifndef _WIN32
                        && err != EAGAIN && err != EINTR
#endif
                    ) {
                        dead.push_back(kv.first);
                    }
                }
            }
            for (int id : dead) closePeer(s, id);
        }
    }
}

void startThreadIfNeeded() {
    NetState& s = state();
    if (s.running.load()) return;
    s.running.store(true);
    s.thread = std::thread(netThreadMain);
}

} // namespace

// --- Value <-> JSON -----------------------------------------------------

json::Json valueToJson(const Value& v) {
    switch (v.type()) {
        case ValueType::Null: return json::Json();
        case ValueType::Bool: return json::Json(v.asBool());
        case ValueType::Int: return json::Json(v.asInt());
        case ValueType::Float: return json::Json(v.asFloat());
        case ValueType::String: return json::Json(v.asString());
        case ValueType::Handle:
            // A handle's `kind` is a local debugging aid with no meaning in
            // the receiving process, so only the id crosses the wire.
            return json::Json(static_cast<int64_t>(v.asHandle().id));
        case ValueType::Array: {
            json::JsonArray arr;
            Value& mut = const_cast<Value&>(v);
            for (const Value& e : mut.arrayRef()) arr.push_back(valueToJson(e));
            return json::Json(std::move(arr));
        }
        case ValueType::Object: {
            json::JsonObject obj;
            Value& mut = const_cast<Value&>(v);
            for (const auto& kv : mut.objectRef()) obj[kv.first] = valueToJson(kv.second);
            return json::Json(std::move(obj));
        }
    }
    return json::Json();
}

Value jsonToValue(const json::Json& j) {
    switch (j.type()) {
        case json::JType::Null: return Value();
        case json::JType::Bool: return Value(j.asBool());
        case json::JType::Number: {
            // JSON has one number type; PlainVulkan distinguishes Int from
            // Float. Preserving integer-ness matters because entity ids and
            // player ids travel as numbers and are compared for equality --
            // `12 == 12.0` holds numerically, but an id that stringifies as
            // "12.0" would break any script using it as a lookup key.
            double d = j.asDouble();
            double intPart = 0.0;
            if (std::modf(d, &intPart) == 0.0 && d >= -9.2e18 && d <= 9.2e18) {
                return Value(static_cast<int64_t>(d));
            }
            return Value(d);
        }
        case json::JType::String: return Value(j.asString());
        case json::JType::Array: {
            Value out = Value::MakeArray();
            for (const auto& e : j.asArray()) out.arrayRef().push_back(jsonToValue(e));
            return out;
        }
        case json::JType::Object: {
            Value out = Value::MakeObject();
            for (const auto& kv : j.asObject()) out.objectRef()[kv.first] = jsonToValue(kv.second);
            return out;
        }
    }
    return Value();
}

void pumpCallbacks() {
    NetState& s = state();
    std::deque<PendingCallback> pending;
    rt::NetCallback onConnect;
    rt::NetCallback1 onDisconnect, onReceive, onError;
    {
        std::lock_guard<std::mutex> lock(s.mtx);
        if (s.callbackQueue.empty()) return;
        pending.swap(s.callbackQueue);
        onConnect = s.onConnect;
        onDisconnect = s.onDisconnect;
        onReceive = s.onReceive;
        onError = s.onNetworkError;
    }
    // Invoked with the lock released: a handler is script code, and script
    // code routinely calls back into Pv::Send / Pv::PollMessages, which take
    // this same lock. Holding it here would self-deadlock on the first such
    // handler.
    for (auto& pc : pending) {
        switch (pc.kind) {
            case PendingKind::Connect: if (onConnect) onConnect(); break;
            case PendingKind::Disconnect: if (onDisconnect) onDisconnect(pc.payload); break;
            case PendingKind::Receive: if (onReceive) onReceive(pc.payload); break;
            case PendingKind::Error: if (onError) onError(pc.payload); break;
        }
    }
}

void shutdownTransport() {
    NetState& s = state();
    if (!s.running.exchange(false)) return;
    if (s.thread.joinable()) s.thread.join();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.sock != PV_INVALID_SOCKET) {
        pv_close_socket(s.sock);
        s.sock = PV_INVALID_SOCKET;
    }
    if (s.listenSock != PV_INVALID_SOCKET) {
        pv_close_socket(s.listenSock);
        s.listenSock = PV_INVALID_SOCKET;
    }
    for (auto& kv : s.peers) {
        if (kv.second.fd != PV_INVALID_SOCKET) pv_close_socket(kv.second.fd);
    }
    s.peers.clear();
    s.status = "disconnected";
}

} // namespace net

namespace rt {

// ---- Connection management ---------------------------------------------

Value Connect(const Value& host, const Value& port) {
    net::ensureSocketSubsystem();
    net::NetState& s = net::state();

    // Reconnecting over a live connection is a script bug that would
    // otherwise leak the old socket silently, so close it explicitly first.
    {
        std::lock_guard<std::mutex> lock(s.mtx);
        if (s.sock != PV_INVALID_SOCKET) {
            pv_close_socket(s.sock);
            s.sock = PV_INVALID_SOCKET;
            s.rx.buf.clear();
        }
        s.status = "connecting";
        s.lastError.clear();
    }

    std::string hostStr = host.asString();
    if (hostStr.empty()) hostStr = "127.0.0.1";
    std::string portStr = std::to_string(port.asInt());

    addrinfo hints{};
    hints.ai_family = AF_INET; // IPv4: matches the PlainS server's listener
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    int rc = ::getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &result);
    if (rc != 0 || !result) {
        std::lock_guard<std::mutex> lock(s.mtx);
        s.status = "error";
        s.lastError = "could not resolve host '" + hostStr + "'";
        rt::LogError(Value("Pv::Connect: " + s.lastError));
        return Value();
    }

    pv_socket_t fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd == PV_INVALID_SOCKET) {
        ::freeaddrinfo(result);
        std::lock_guard<std::mutex> lock(s.mtx);
        s.status = "error";
        s.lastError = "could not create socket";
        rt::LogError(Value("Pv::Connect: " + s.lastError));
        return Value();
    }

    // Non-blocking connect + select gives a bounded connect timeout. A
    // blocking connect() to an unreachable host can otherwise sit for the
    // OS default (over a minute on some systems) with the game frozen.
    net::setNonBlocking(fd);
    int cr = ::connect(fd, result->ai_addr, static_cast<int>(result->ai_addrlen));
    ::freeaddrinfo(result);

    if (cr != 0) {
        int err = pv_sock_errno;
        if (err != PV_INPROGRESS && err != PV_WOULDBLOCK) {
            pv_close_socket(fd);
            std::lock_guard<std::mutex> lock(s.mtx);
            s.status = "error";
            s.lastError = "connection refused by " + hostStr + ":" + portStr;
            rt::LogError(Value("Pv::Connect: " + s.lastError));
            return Value();
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(fd, &writeSet);
        timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        int ready = ::select(static_cast<int>(fd) + 1, nullptr, &writeSet, nullptr, &tv);
        bool ok = ready > 0;
        if (ok) {
            // select() reporting writable only means the handshake finished
            // one way or the other; SO_ERROR carries which.
            int soErr = 0;
#ifdef _WIN32
            int len = sizeof(soErr);
#else
            socklen_t len = sizeof(soErr);
#endif
            getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
            ok = (soErr == 0);
        }
        if (!ok) {
            pv_close_socket(fd);
            std::lock_guard<std::mutex> lock(s.mtx);
            s.status = "error";
            s.lastError = "could not connect to " + hostStr + ":" + portStr + " (timed out or refused)";
            rt::LogError(Value("Pv::Connect: " + s.lastError));
            return Value();
        }
    }

    net::setNoDelay(fd);
    {
        std::lock_guard<std::mutex> lock(s.mtx);
        s.sock = fd;
        crypto::wipe(s.session);
        s.session = crypto::Session{};
        s.session.isServer = false;
        crypto::generateKeyPair(s.session);
        if (!net::sendAll(fd, net::buildFrame(net::FRAME_HANDSHAKE, crypto::handshakePayload(s.session)))) {
            pv_close_socket(fd);
            s.sock = PV_INVALID_SOCKET;
            s.status = "error";
            s.lastError = "failed to send handshake";
            rt::LogError(Value("Pv::Connect: " + s.lastError));
            return Value();
        }
        s.status = "connecting";
        s.remoteAddr = hostStr + ":" + portStr;
        s.lastPingMicros = net::nowMicros();
        // OnConnect waits until the server's handshake completes (see deliverFrame).
    }
    net::startThreadIfNeeded();
    net::debugLog("connected to " + hostStr + ":" + portStr);
    return Value::MakeHandle(static_cast<uint64_t>(fd), "connection");
}

Value Disconnect() {
    net::NetState& s = net::state();
    {
        std::lock_guard<std::mutex> lock(s.mtx);
        if (s.sock == PV_INVALID_SOCKET) return Value(false);
        pv_close_socket(s.sock);
        s.sock = PV_INVALID_SOCKET;
        s.rx.buf.clear();
        s.status = "disconnected";
        // A script-initiated disconnect still fires OnDisconnect, so a
        // handler that tears down UI or stops a sound has exactly one place
        // to live regardless of which side closed the connection.
        net::queueCallback(s, net::PendingKind::Disconnect, Value("local disconnect"));
    }
    net::pumpCallbacks();
    return Value(true);
}

Value IsConnected() {
    net::pumpCallbacks();
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    return Value(s.sock != PV_INVALID_SOCKET && s.status == "connected" && s.session.ready);
}

Value GetConnectionStatus() {
    net::pumpCallbacks();
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    return Value(s.status);
}

Value GetConnectionError() {
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    return Value(s.lastError);
}

Value GetSessionToken() {
    net::pumpCallbacks();
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.session.ready) return Value();
    return Value(crypto::tokenHex(s.session));
}

// ---- Sending -----------------------------------------------------------

Value Send(const Value& data) {
    net::NetState& s = net::state();
    std::string payload = net::valueToJson(data).dump(-1);
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.sock == PV_INVALID_SOCKET || !s.session.ready) return Value(false);
    bool ok = net::sendKindLocked(s, s.sock, net::FRAME_JSON, payload);
    if (!ok) {
        net::closeServerConnection(s, "send failed", true);
        return Value(false);
    }
    if (s.debugLogging.load()) rt::Log(Value("[net] sent " + std::to_string(payload.size()) + "B JSON"));
    return Value(true);
}

Value SendRaw(const Value& bytes) {
    net::NetState& s = net::state();
    std::string payload;
    if (bytes.isArray()) {
        Value& mut = const_cast<Value&>(bytes);
        for (const Value& b : mut.arrayRef()) {
            payload.push_back(static_cast<char>(b.asInt() & 0xFF));
        }
    } else {
        payload = bytes.asString();
    }
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.sock == PV_INVALID_SOCKET || !s.session.ready) return Value(false);
    if (!net::sendKindLocked(s, s.sock, net::FRAME_RAW, payload)) {
        net::closeServerConnection(s, "send failed", true);
        return Value(false);
    }
    return Value(true);
}

Value Flush() {
    // Sends are never buffered in user space: Pv::Send writes its frame to
    // the socket before returning, and TCP_NODELAY is set so the kernel
    // doesn't hold it back either. Flush therefore has nothing to force and
    // exists so scripts written against the buffered-send model in the API
    // reference still run correctly. Kept as a real call (rather than
    // removed) because a no-op is the *correct* implementation here, and
    // deleting it would break those scripts for no gain.
    net::pumpCallbacks();
    return Value(true);
}

// ---- Receiving ---------------------------------------------------------

Value Receive() {
    net::NetState& s = net::state();
    std::unique_lock<std::mutex> lock(s.mtx);
    // Waits in short slices rather than one indefinite wait so queued
    // callbacks still get dispatched while a script sits in a blocking
    // receive, and so a disconnect can't strand the caller forever.
    while (s.sock != PV_INVALID_SOCKET && s.inbox.empty()) {
        s.inboxCv.wait_for(lock, std::chrono::milliseconds(50));
        if (!s.callbackQueue.empty()) {
            lock.unlock();
            net::pumpCallbacks();
            lock.lock();
        }
    }
    if (s.inbox.empty()) return Value();
    Value v = s.inbox.front();
    s.inbox.pop_front();
    return v;
}

Value ReceiveNonBlocking() {
    net::pumpCallbacks();
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.inbox.empty()) return Value();
    Value v = s.inbox.front();
    s.inbox.pop_front();
    return v;
}

Value ReceiveTimeout(const Value& milliseconds) {
    net::NetState& s = net::state();
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(std::max<int64_t>(0, milliseconds.asInt()));
    std::unique_lock<std::mutex> lock(s.mtx);
    while (s.inbox.empty() && std::chrono::steady_clock::now() < deadline) {
        if (s.sock == PV_INVALID_SOCKET) break;
        s.inboxCv.wait_until(lock, deadline);
    }
    if (s.inbox.empty()) return Value();
    Value v = s.inbox.front();
    s.inbox.pop_front();
    return v;
}

Value PollMessages() {
    net::pumpCallbacks();
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    return Value(static_cast<int64_t>(s.inbox.size()));
}

Value ClearMessageQueue() {
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    int64_t n = static_cast<int64_t>(s.inbox.size());
    s.inbox.clear();
    return Value(n);
}

// ---- Callbacks ---------------------------------------------------------

Value OnConnect(NetCallback callback) {
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.onConnect = callback;
    // Deliver a connection that completed before this handler existed, so
    // the documented Connect-then-register order works. Cleared either way,
    // so a later reconnect is the only thing that can set it again.
    if (callback && s.connectPending) {
        s.callbackQueue.push_back(net::PendingCallback{net::PendingKind::Connect, Value()});
    }
    s.connectPending = false;
    return Value(true);
}

Value OnDisconnect(NetCallback1 callback) {
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.onDisconnect = callback;
    return Value(true);
}

Value OnReceive(NetCallback1 callback) {
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.onReceive = callback;
    // Anything already queued for polling predates the handler; hand it over
    // so the first frames after connecting aren't stranded in a queue the
    // script has now signalled it will never poll.
    while (!s.inbox.empty()) {
        s.callbackQueue.push_back(net::PendingCallback{net::PendingKind::Receive, s.inbox.front()});
        s.inbox.pop_front();
    }
    return Value(true);
}

Value OnNetworkError(NetCallback1 callback) {
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.onNetworkError = callback;
    return Value(true);
}

// ---- Latency & debugging -----------------------------------------------

Value GetLatency() {
    return Value(static_cast<double>(net::state().latencyMicros.load()) / 1000.0);
}

Value GetPacketLoss() {
    // TCP never exposes a datagram loss rate -- it retransmits until the
    // data arrives or the connection dies -- so reporting one would be
    // fiction. This is the fraction of latency probes that went unanswered,
    // which is the honest observable: it rises when the link is bad enough
    // that retransmission is stalling the stream.
    net::NetState& s = net::state();
    uint64_t sent = s.pingsSent.load();
    uint64_t got = s.pongsReceived.load();
    if (sent == 0) return Value(0.0);
    if (got >= sent) return Value(0.0);
    return Value(static_cast<double>(sent - got) / static_cast<double>(sent));
}

Value GetBytesReceived() {
    return Value(static_cast<int64_t>(net::state().bytesReceived.load()));
}

Value GetBytesSent() {
    return Value(static_cast<int64_t>(net::state().bytesSent.load()));
}

Value EnableNetworkDebug(const Value& enabled) {
    net::state().debugLogging.store(enabled.truthy());
    return Value(true);
}

Value GetNetworkStats() {
    Value out = Value::MakeObject();
    auto& o = out.objectRef();
    o["latency"] = GetLatency();
    o["packetLoss"] = GetPacketLoss();
    o["bytesSent"] = GetBytesSent();
    o["bytesReceived"] = GetBytesReceived();
    o["status"] = GetConnectionStatus();
    o["pendingMessages"] = PollMessages();
    return out;
}

// ---- Peer-to-peer ------------------------------------------------------

Value Listen(const Value& port) {
    net::ensureSocketSubsystem();
    net::NetState& s = net::state();

    pv_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == PV_INVALID_SOCKET) {
        rt::LogError(Value("Pv::Listen: could not create listening socket"));
        return Value(false);
    }
    // Without SO_REUSEADDR, restarting a host during development fails to
    // bind for the duration of TIME_WAIT (up to two minutes).
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port.asInt()));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        pv_close_socket(fd);
        rt::LogError(Value("Pv::Listen: port " + std::to_string(port.asInt()) + " is already in use"));
        return Value(false);
    }
    if (::listen(fd, 16) != 0) {
        pv_close_socket(fd);
        rt::LogError(Value("Pv::Listen: listen() failed"));
        return Value(false);
    }
    net::setNonBlocking(fd);
    {
        std::lock_guard<std::mutex> lock(s.mtx);
        if (s.listenSock != PV_INVALID_SOCKET) pv_close_socket(s.listenSock);
        s.listenSock = fd;
    }
    net::startThreadIfNeeded();
    net::debugLog("listening on port " + std::to_string(port.asInt()));
    return Value(true);
}

Value AcceptConnection() {
    // The network thread does the accept(); this hands the script the next
    // already-accepted peer id, or null when none is waiting. Polling here
    // rather than blocking keeps the pattern identical to the rest of the
    // API: the script asks once per frame and moves on.
    net::pumpCallbacks();
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.pendingAccepts.empty()) return Value();
    int id = s.pendingAccepts.front();
    s.pendingAccepts.pop_front();
    return Value(static_cast<int64_t>(id));
}

Value SendToPeer(const Value& peer_id, const Value& data) {
    net::NetState& s = net::state();
    std::string frame = net::buildFrame(net::FRAME_JSON, net::valueToJson(data).dump(-1));
    std::lock_guard<std::mutex> lock(s.mtx);
    auto it = s.peers.find(static_cast<int>(peer_id.asInt()));
    if (it == s.peers.end()) return Value(false);
    if (!net::sendAll(it->second.fd, frame)) {
        net::closePeer(s, it->first);
        return Value(false);
    }
    return Value(true);
}

Value ReceiveFromPeer() {
    net::pumpCallbacks();
    net::NetState& s = net::state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.peerInbox.empty()) return Value();
    auto entry = s.peerInbox.front();
    s.peerInbox.pop_front();
    Value out = Value::MakeObject();
    out.objectRef()["peer_id"] = Value(static_cast<int64_t>(entry.first));
    out.objectRef()["data"] = entry.second;
    return out;
}

} // namespace rt
} // namespace pv
