#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A local stand-in for RMG-K's lobby server (rmgk-lobby), for testing net/lobby without the
real one. The server's source is not public; this implements the server side as RMG-K 0.9.16's
client expects it (docs/PLAN.md, "RMG-K lobby, in detail"): the WebSocket upgrade (RFC 6455),
HELLO/HELLO_OK, presence, room list, rooms (create/join/leave/delay/prediction/start), chat,
the ICE_SIGNAL relay and MATCH_BEGIN. Standard library only.

    python3 net/lobby/test/mock_lobby.py --port 18080          (listens on 127.0.0.1 only)
"""
import argparse
import asyncio
import base64
import hashlib
import json
import re
import struct
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
USERNAME = re.compile(r"^[A-Za-z0-9_\-\.]{2,20}$")
VERBOSE = False


def log(*args):
    print("[mock]", *args, file=sys.stderr, flush=True)


def version_ok(v):
    # RollbackLobbyDialog.cpp:2744-2746: "requires RMG-K 0.9.13 or vdev-2367 or newer".
    m = re.match(r"^v?(\d+)\.(\d+)\.(\d+)", v or "")
    if m:
        return tuple(int(x) for x in m.groups()) >= (0, 9, 13)
    m = re.match(r"^vdev-(\d+)", v or "")
    return bool(m) and int(m.group(1)) >= 2367


class Client:
    def __init__(self, server, reader, writer):
        self.server = server
        self.reader = reader
        self.writer = writer
        self.id = 0
        self.username = ""
        self.hello = {}
        self.room = None
        self.frame_delay = -1
        self.peer = writer.get_extra_info("peername")

    async def send(self, type_, data=None):
        env = {"type": type_}
        if data is not None:
            env["data"] = data
        text = json.dumps(env, separators=(",", ":"))
        if VERBOSE or type_ not in ("HEARTBEAT_ACK",):
            log(f"-> {self.username or self.peer} {type_} {text[:300]}")
        payload = text.encode()
        header = bytearray([0x81])
        if len(payload) < 126:
            header.append(len(payload))
        elif len(payload) < 65536:
            header.append(126)
            header += struct.pack(">H", len(payload))
        else:
            header.append(127)
            header += struct.pack(">Q", len(payload))
        try:
            self.writer.write(bytes(header) + payload)
            await self.writer.drain()
        except (ConnectionError, OSError):
            pass

    def presence(self):
        state = "idle"
        if self.room:
            state = "in_game" if self.room.state == "in_game" else "in_room"
        return {
            "id": self.id, "username": self.username, "state": state, "region": "us-east",
            "country": "" if self.hello.get("anonymous") else "US",
            "clientVersion": self.hello.get("clientVersion", ""),
            "connection": self.hello.get("connection", ""),
            "anonymous": bool(self.hello.get("anonymous")), "pingToServer": 0,
            "currentRoomId": self.room.id if self.room else 0,
            "currentRoomName": self.room.name if self.room else "", "searchingRom": "",
        }


class Room:
    def __init__(self, rid, host, d):
        self.id = rid
        self.name = d.get("name") or f"{host.username}'s room"
        self.host = host
        rom = d.get("rom") or {}
        self.rom = {"name": rom.get("name", ""), "md5": rom.get("md5", ""), "region": rom.get("region", "")}
        self.max_players = max(2, min(4, int(d.get("maxPlayers", 2))))
        self.prediction = max(1, min(9, int(d.get("prediction", 7) or 7)))
        self.prediction_auto = True
        self.pacing = int(d.get("pacing", 1))
        self.password = d.get("password", "")
        self.seats = {}  # slot -> Client
        self.state = "waiting"
        self.match_id = 0
        self.started_at = 0

    def slot_of(self, c):
        for s, x in self.seats.items():
            if x is c:
                return s
        return 0

    def state_json(self):
        players = []
        for slot in sorted(self.seats):
            c = self.seats[slot]
            players.append({
                "userId": c.id, "username": c.username, "slot": slot, "frameDelay": c.frame_delay,
                "iceSupported": bool(c.hello.get("iceSupported")), "inMatch": self.state == "in_game",
                "lateJoin": bool(c.hello.get("lateJoin")), "saveData": bool(c.hello.get("saveData")),
            })
        return {
            "id": self.id, "name": self.name, "hostId": self.host.id, "rom": self.rom,
            "maxPlayers": self.max_players, "state": self.state, "prediction": self.prediction,
            "predictionAuto": self.prediction_auto, "pacing": self.pacing,
            "liveReplayEnabled": False, "inGameSettings": True, "lateJoin": False,
            "canStartAlone": False, "players": players,
        }

    def summary(self):
        return {
            "id": self.id, "name": self.name, "hostId": self.host.id, "hostName": self.host.username,
            "rom": {"name": self.rom["name"], "md5": self.rom["md5"]}, "players": len(self.seats),
            "maxPlayers": self.max_players, "state": self.state, "hasPassword": bool(self.password),
            "startedAt": self.started_at, "broadcasting": False, "matchId": 0, "viewerCount": 0,
            "lateJoin": False, "playerNames": [self.seats[s].username for s in sorted(self.seats)],
        }


class Server:
    def __init__(self):
        self.clients = {}
        self.rooms = {}
        self.next_user = 1
        self.next_room = 100
        self.next_match = 1000
        self.relayed = 0

    async def broadcast(self, type_, data, only=None):
        for c in list(self.clients.values()):
            if only is None or c in only:
                await c.send(type_, data)

    async def room_list(self):
        await self.broadcast("ROOM_LIST", {"rooms": [r.summary() for r in self.rooms.values()]})

    async def room_state(self, room):
        await self.broadcast("ROOM_STATE", room.state_json(), only=list(room.seats.values()))

    async def presence_update(self, c):
        await self.broadcast("PRESENCE_DELTA", {"added": [], "updated": [c.presence()], "removed": []})

    async def leave_room(self, c, reason="left"):
        room = c.room
        if not room:
            return
        slot = room.slot_of(c)
        room.seats.pop(slot, None)
        c.room = None
        if room.state == "in_game":
            await self.broadcast("MATCH_PEER_LEFT", {"matchId": room.match_id, "userId": c.id, "slot": slot,
                                                     "reason": reason}, only=list(room.seats.values()))
        if not room.seats:
            del self.rooms[room.id]
        else:
            if room.host is c:
                room.host = room.seats[min(room.seats)]
            await self.room_state(room)
        await self.room_list()

    async def handle(self, c, env):
        t = env.get("type", "")
        d = env.get("data") or {}
        if t != "HEARTBEAT" or VERBOSE:
            log(f"<- {c.username or c.peer} {t} {json.dumps(d)[:300]}")
        if c.id == 0:
            if t != "HELLO":
                await c.send("HELLO_FAIL", {"reason": "invalid_hello"})
                return False
            name = d.get("username", "")
            if not USERNAME.match(name):
                await c.send("HELLO_FAIL", {"reason": "invalid_payload"})
                return False
            if not version_ok(d.get("clientVersion", "")):
                await c.send("HELLO_FAIL", {"reason": "version_mismatch"})
                return False
            if any(x.username.lower() == name.lower() for x in self.clients.values()):
                await c.send("HELLO_FAIL", {"reason": "username_taken"})
                return False
            c.id = self.next_user
            self.next_user += 1
            c.username = name
            c.hello = d
            self.clients[c.id] = c
            host = c.peer[0] if c.peer else "127.0.0.1"
            # STUN points at this machine so nothing ever leaves it, even for a client that does
            # not override the server's ICE configuration (nothing listens there).
            await c.send("HELLO_OK", {"userId": c.id, "observedIp": host, "region": "us-east",
                                      "ice": {"stun": {"host": "127.0.0.1", "port": 3478}, "turn": []}})
            await c.send("PRESENCE_FULL", {"users": [x.presence() for x in self.clients.values()]})
            await c.send("ROOM_LIST", {"rooms": [r.summary() for r in self.rooms.values()]})
            await self.broadcast("PRESENCE_DELTA", {"added": [c.presence()], "updated": [], "removed": []},
                                 only=[x for x in self.clients.values() if x is not c])
            return True

        if t == "HEARTBEAT":
            await c.send("HEARTBEAT_ACK", {"serverTime": int(time.time() * 1000)})
        elif t == "CHAT_SEND":
            ch = d.get("channel", "lobby")
            msg = {"channel": ch, "fromUserId": c.id, "fromUsername": c.username,
                   "message": d.get("message", ""), "serverTime": int(time.time() * 1000)}
            if ch == "room" and c.room:
                await self.broadcast("CHAT_MSG", msg, only=list(c.room.seats.values()))
            elif ch == "lobby":
                await self.broadcast("CHAT_MSG", msg)
        elif t == "CHAT_HISTORY":
            await c.send("CHAT_HISTORY_REPLY", {"channel": d.get("channel", ""), "messages": []})
        elif t == "ROOM_CREATE":
            if c.room:
                await c.send("ROOM_CREATE_FAIL", {"reason": "already in a room"})
                return True
            room = Room(self.next_room, c, d)
            self.next_room += 1
            room.seats[1] = c
            c.room = room
            self.rooms[room.id] = room
            await c.send("ROOM_CREATED", {"roomId": room.id})
            await self.room_state(room)
            await self.room_list()
            await self.presence_update(c)
        elif t == "ROOM_JOIN":
            room = self.rooms.get(int(d.get("roomId", 0)))
            if not room:
                await c.send("ROOM_JOIN_FAIL", {"reason": "room not found"})
            elif c.room:
                await c.send("ROOM_JOIN_FAIL", {"reason": "already in a room"})
            elif room.password and d.get("password", "") != room.password:
                await c.send("ROOM_JOIN_FAIL", {"reason": "wrong password"})
            elif len(room.seats) >= room.max_players or room.state != "waiting":
                await c.send("ROOM_JOIN_FAIL", {"reason": "room is full"})
            else:
                slot = min(s for s in range(1, 5) if s not in room.seats)
                room.seats[slot] = c
                c.room = room
                await c.send("ROOM_JOIN_OK", room.state_json())
                await self.room_state(room)
                await self.room_list()
                await self.presence_update(c)
        elif t == "ROOM_LEAVE":
            if c.room:
                await self.leave_room(c)
                await c.send("ROOM_LEFT", {"reason": "left"})
                await self.presence_update(c)
        elif t == "ROOM_UPDATE_DELAY":
            c.frame_delay = int(d.get("delay", -1))
            if c.room:
                await self.room_state(c.room)
        elif t == "ROOM_UPDATE_PREDICTION":
            if c.room and c.room.host is c:
                c.room.prediction = max(1, min(9, int(d.get("prediction", 7))))
                c.room.prediction_auto = bool(d.get("predictionAuto", False))
                await self.room_state(c.room)
        elif t == "ICE_SIGNAL":
            room = c.room
            target = self.clients.get(int(d.get("targetUserId", 0)))
            if room and target and target.room is room and int(d.get("roomId", 0)) == room.id:
                out = {"roomId": room.id, "fromUserId": c.id, "kind": d.get("kind", ""),
                       "generation": int(d.get("generation", 0))}
                if "value" in d:
                    out["value"] = d["value"]
                self.relayed += 1
                await target.send("ICE_SIGNAL", out)
        elif t == "ROOM_START":
            room = c.room
            if room and room.host is c and room.state == "waiting" and len(room.seats) >= 2:
                room.state = "in_game"
                room.match_id = self.next_match
                self.next_match += 1
                room.started_at = int(time.time() * 1000)
                await self.room_state(room)
                peers = []
                for slot in sorted(room.seats):
                    p = room.seats[slot]
                    ip, port = (p.peer[0], p.peer[1]) if p.peer else ("127.0.0.1", 0)
                    peers.append({"userId": p.id, "username": p.username, "publicIp": ip, "publicPort": port,
                                  "localIp": p.hello.get("localIp", ""), "slot": slot,
                                  "iceSupported": bool(p.hello.get("iceSupported"))})
                await self.broadcast("MATCH_BEGIN", {"matchId": room.match_id, "peers": peers, "seats": 0,
                                                     "keyframeIntervalSec": 30}, only=list(room.seats.values()))
                await self.room_list()
        elif t in ("MATCH_FINISHED", "MATCH_PUNCH_FAILED"):
            room = c.room
            if room and room.state == "in_game" and int(d.get("matchId", 0)) == room.match_id:
                room.state = "waiting"
                room.match_id = 0
                room.started_at = 0
                await self.room_state(room)
                await self.room_list()
        elif t == "MATCH_CONNECTED":
            pass
        elif t == "ROOM_SWAP_SLOTS":
            room = c.room
            a, b = int(d.get("slotA", 0)), int(d.get("slotB", 0))
            if room and room.host is c and room.state == "waiting" and 1 <= a <= 4 and 1 <= b <= 4:
                sa, sb = room.seats.pop(a, None), room.seats.pop(b, None)
                if sa: room.seats[b] = sa
                if sb: room.seats[a] = sb
                await self.room_state(room)
        elif t == "ROOM_KICK":
            room = c.room
            target = self.clients.get(int(d.get("userId", 0)))
            if room and room.host is c and target and target.room is room and target is not c:
                await self.leave_room(target, "kicked")
                await target.send("ROOM_LEFT", {"reason": "kicked"})
        else:
            log(f"   (unhandled {t})")
        return True

    async def serve(self, reader, writer):
        c = Client(self, reader, writer)
        try:
            head = await reader.readuntil(b"\r\n\r\n")
            lines = head.decode("latin-1").split("\r\n")
            headers = {}
            for line in lines[1:]:
                if ":" in line:
                    k, v = line.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
            key = headers.get("sec-websocket-key", "")
            if not lines[0].startswith("GET /ws ") or headers.get("upgrade", "").lower() != "websocket" or not key:
                writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
                await writer.drain()
                writer.close()
                return
            accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
            writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
            await writer.drain()
            # Exercise the client's control-frame handling once: a ping it must answer.
            writer.write(bytes([0x89, 4]) + b"mock")
            await writer.drain()
            fragments = b""
            while True:
                b0, b1 = await reader.readexactly(2)
                opcode = b0 & 0x0F
                n = b1 & 0x7F
                if not (b1 & 0x80):
                    log("client frame not masked: protocol error")
                    break
                if n == 126:
                    n = struct.unpack(">H", await reader.readexactly(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", await reader.readexactly(8))[0]
                if n > (1 << 20):
                    log("frame over the 1 MiB limit")
                    break
                mask = await reader.readexactly(4)
                payload = bytes(x ^ mask[i & 3] for i, x in enumerate(await reader.readexactly(n)))
                if opcode == 0x8:
                    writer.write(bytes([0x88, len(payload[:2])]) + payload[:2])
                    await writer.drain()
                    break
                if opcode == 0x9:
                    writer.write(bytes([0x8A, len(payload)]) + payload)
                    await writer.drain()
                    continue
                if opcode == 0xA:
                    if VERBOSE:
                        log(f"pong from {c.username or c.peer}")
                    continue
                fragments += payload
                if not (b0 & 0x80):
                    continue
                text, fragments = fragments.decode("utf-8"), b""
                try:
                    env = json.loads(text)
                except ValueError:
                    log("bad JSON from client")
                    continue
                if not await self.handle(c, env):
                    break
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            if c.id:
                await self.leave_room(c, "disconnected")
                self.clients.pop(c.id, None)
                await self.broadcast("PRESENCE_DELTA", {"added": [], "updated": [], "removed": [c.id]})
                log(f"{c.username} disconnected (ICE signals relayed so far: {self.relayed})")
            try:
                writer.close()
            except Exception:
                pass


async def main():
    global VERBOSE
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    VERBOSE = args.verbose
    server = Server()
    srv = await asyncio.start_server(server.serve, "127.0.0.1", args.port)
    log(f"listening on ws://127.0.0.1:{args.port}/ws")
    print("READY", flush=True)
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
