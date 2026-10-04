#!/usr/bin/env python3
"""mock_plains_server.py -- a minimal stand-in for the PlainS server.

The real PlainS/PlainVulkan path now requires an X25519 handshake
(FRAME_HANDSHAKE=4) and XChaCha20-Poly1305 frames (FRAME_AEAD=5) via
Monocypher. This mock still speaks the old cleartext JSON frames and is
only useful for isolated framing experiments, not for Pv::Connect against
a current client.
"""

Wire format (per frame):
    [0..4)  uint32 big-endian: 1 + len(payload)
    [4]     uint8 frame kind: 0=JSON 1=RAW 2=PING 3=PONG
    [5..)   payload

Behaviour:
  * on accept, sends a JSON "welcome" frame then an initial "state" frame
  * treats each received JSON input packet {"action","x","y","z"} as the
    authoritative new position of that client's entity (server id 1) and
    broadcasts a fresh "state" frame
  * echoes any PING payload straight back as PONG, which is what drives
    Pv::GetLatency()

Usage:  python3 mock_plains_server.py [--host 127.0.0.1] [--port 8080]
"""

import argparse
import json
import socket
import struct
import threading

FRAME_JSON, FRAME_RAW, FRAME_PING, FRAME_PONG = 0, 1, 2, 3


def build_frame(kind: int, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload) + 1) + bytes([kind]) + payload


def send_json(conn, obj) -> None:
    conn.sendall(build_frame(FRAME_JSON, json.dumps(obj).encode("utf-8")))


def state_packet(tick: int, pos) -> dict:
    return {
        "type": "state",
        "tick": tick,
        "entities": {
            "1": {
                "id": 1,
                "name": "Player_1",
                "state": "alive",
                "health": 100,
                "position": [pos[0], pos[1], pos[2]],
                "rotation": [0.0, 0.0, 0.0],
                "velocity": [0.0, 0.0, 0.0],
            }
        },
    }


def handle(conn, addr, verbose):
    if verbose:
        print(f"[mock-plains] client connected: {addr}", flush=True)
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    tick = 0
    pos = [0.0, 0.0, 0.0]
    buf = b""
    try:
        # The field is "player_id", not "playerId". The real PlainS server
        # (examples/mp_server/main.pls, OnConnect) sends snake_case, and this
        # mock exists to stand in for it -- a client written against a
        # camelCase welcome packet here would read null against the real
        # server. initial_position and tickrate are sent for the same reason:
        # to match the real packet rather than a subset of it.
        send_json(conn, {
            "type": "welcome",
            "player_id": 1,
            "initial_position": [0, 1, 0],
            "tickrate": 60,
        })
        tick += 1
        send_json(conn, state_packet(tick, pos))

        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            buf += chunk
            # Frames arrive coalesced or split; drain whole ones only.
            while len(buf) >= 4:
                length = struct.unpack(">I", buf[:4])[0]
                if length == 0 or len(buf) < 4 + length:
                    break
                kind = buf[4]
                payload = buf[5 : 4 + length]
                buf = buf[4 + length :]

                if kind == FRAME_PING:
                    # Echo the timestamp verbatim -- the client measures the
                    # round trip itself, so the server must not reinterpret it.
                    conn.sendall(build_frame(FRAME_PONG, payload))
                elif kind == FRAME_JSON:
                    try:
                        msg = json.loads(payload.decode("utf-8"))
                    except (ValueError, UnicodeDecodeError):
                        continue
                    if isinstance(msg, dict) and "action" in msg:
                        pos = [
                            float(msg.get("x", 0.0)),
                            float(msg.get("y", 0.0)),
                            float(msg.get("z", 0.0)),
                        ]
                        tick += 1
                        send_json(conn, state_packet(tick, pos))
                elif kind == FRAME_RAW:
                    conn.sendall(build_frame(FRAME_RAW, payload))
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass
        if verbose:
            print(f"[mock-plains] client disconnected: {addr}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(8)
    if not args.quiet:
        print(f"[mock-plains] listening on {args.host}:{args.port}", flush=True)
    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(
                target=handle, args=(conn, addr, not args.quiet), daemon=True
            ).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()
