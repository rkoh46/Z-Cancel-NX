#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A local stand-in for RMG-K's two netplay servers, for tests that must not touch the real ones.

  nat.smash64.net:6364      connect codes (KailleraTraversalConfig.hpp, N02TRAV1 over UDP)
  p2plist.smash64.net:27887 the public list: ENLIST/ENLISP/UNENLIST/WHATISMYIP over UDP, and
                            http://.../plist.txt over TCP on the same port number

Written from what RMG-K's client sends and expects (KailleraP2PDialog.cpp onSsrvPacketReceived,
travSend*, ssrvSend, enlistGame; KailleraWaitingGamesDialog.cpp parseResponse), not from the
servers' own code, which is not public. Every reply is a datagram that starts with a 0 byte, which
is how n02 (p2p_message.h ingest_raw_packet) tells a server's text from a lobby instruction.

  tools/mock-smash64.py [--nat-port 16364] [--list-port 17887] [--plist FIXTURE] [--bind 127.0.0.1]

--plist serves that file as plist.txt in place of the games registered with ENLIST/ENLISP.
Each event is printed on stdout, one line, for the test to check.
"""
import argparse
import itertools
import secrets
import socket
import socketserver
import sys
import threading
import time

def log(*parts):
    print(*parts, flush=True)

class Traversal:
    """N02TRAV1|CLAIM|AUTO -> CLAIMOK|code|token; HOSTOPEN|token -> HOSTOK|code|live|ip|port;
    HOSTKEEP/HOSTCLOSE|live -> OK; JOIN|code|tick -> HOST|live|ip|port to the joiner and
    PEER|live|ip|port to the host; errors as ERR|reason."""
    def __init__(self, sock):
        self.sock = sock
        self.numbers = itertools.count(7)
        self.owners = {}      # token -> code
        self.live = {}        # code -> (live token, host address, last seen)

    def send(self, addr, text):
        self.sock.sendto(b"\0" + text.encode(), addr)
        log("nat ->", addr[0], addr[1], text.split("|")[1] if "|" in text else text)

    def handle(self, data, addr):
        if not data.startswith(b"\0"):
            return
        text = data[1:].rstrip(b"\0").decode(errors="replace")
        parts = text.split("|")
        if len(parts) < 2 or parts[0] != "N02TRAV1":
            log("nat <-", addr[0], addr[1], "unknown", text)
            return
        kind = parts[1]
        log("nat <-", addr[0], addr[1], kind, *(parts[2:3] if kind in ("CLAIM", "JOIN") else []))
        if kind == "CLAIM":
            code = "ZNX@%d" % next(self.numbers)
            token = secrets.token_hex(8)
            self.owners[token] = code
            self.send(addr, "N02TRAV1|CLAIMOK|%s|%s" % (code, token))
        elif kind == "CLAIMACK":
            self.send(addr, "N02TRAV1|OK")
        elif kind == "HOSTOPEN" and len(parts) >= 3:
            code = self.owners.get(parts[2])
            if code is None:
                self.send(addr, "N02TRAV1|ERR|NOAUTH")
                return
            live = secrets.token_hex(6)
            self.live[code] = (live, addr, time.time())
            self.send(addr, "N02TRAV1|HOSTOK|%s|%s|%s|%d" % (code, live, addr[0], addr[1]))
        elif kind == "HOSTKEEP" and len(parts) >= 3:
            for code, (live, host, _) in list(self.live.items()):
                if live == parts[2]:
                    self.live[code] = (live, addr, time.time())
                    self.send(addr, "N02TRAV1|OK")
                    return
            self.send(addr, "N02TRAV1|ERR|NOSESSION")
        elif kind == "HOSTCLOSE" and len(parts) >= 3:
            for code, (live, _, _) in list(self.live.items()):
                if live == parts[2]:
                    del self.live[code]
            self.send(addr, "N02TRAV1|OK")
        elif kind == "JOIN" and len(parts) >= 3:
            code = parts[2].upper()
            if code not in self.owners.values():
                self.send(addr, "N02TRAV1|ERR|UNKNOWNCODE")
                return
            if code not in self.live:
                self.send(addr, "N02TRAV1|ERR|OFFLINE")
                return
            live, host, _ = self.live[code]
            self.send(host, "N02TRAV1|PEER|%s|%s|%d" % (live, addr[0], addr[1]))
            self.send(addr, "N02TRAV1|HOST|%s|%s|%d" % (live, host[0], host[1]))
        else:
            self.send(addr, "N02TRAV1|ERR|BADREQ")

class ListServer:
    """ENLIST game|app|user, ENLISP game|app|user|port, UNENLIST, WHATISMYIP (UDP, NUL-terminated)."""
    def __init__(self, sock, fixture):
        self.sock = sock
        self.fixture = fixture
        self.games = {}       # address -> line
        self.lock = threading.Lock()

    def handle(self, data, addr):
        if not data.startswith(b"\0"):
            return
        text = data[1:].split(b"\0")[0].decode(errors="replace")
        log("list <-", addr[0], addr[1], text)
        if text.startswith("WHATISMYIP"):
            self.sock.sendto(b"\0%s:%d\0" % (addr[0].encode(), addr[1]), addr)
        elif text.startswith("ENLISP ") or text.startswith("ENLIST "):
            fields = text[7:].split("|")
            if len(fields) < 3:
                return
            port = int(fields[3]) if text.startswith("ENLISP ") and len(fields) >= 4 else 27886
            with self.lock:
                self.games[addr] = "%s|%s|%s|%s:%d" % (fields[0], fields[1], fields[2], addr[0], port)
        elif text.startswith("UNENLIST"):
            with self.lock:
                self.games.pop(addr, None)

    def plist(self):
        if self.fixture is not None:
            with open(self.fixture, "rb") as f:
                return f.read()
        with self.lock:
            return ("".join(line + "\n" for line in self.games.values())).encode()

def udp_loop(sock, handler):
    while True:
        data, addr = sock.recvfrom(4096)
        try:
            handler.handle(data, addr)
        except Exception as error:   # a test tool: say it and keep serving
            log("error", error)

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nat-port", type=int, default=16364)
    parser.add_argument("--list-port", type=int, default=17887)
    parser.add_argument("--plist")
    parser.add_argument("--bind", default="127.0.0.1")
    args = parser.parse_args()

    nat = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    nat.bind((args.bind, args.nat_port))
    udp_list = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_list.bind((args.bind, args.list_port))
    traversal = Traversal(nat)
    listing = ListServer(udp_list, args.plist)

    class Http(socketserver.StreamRequestHandler):
        def handle(self):
            request = self.rfile.readline().decode(errors="replace").strip()
            while self.rfile.readline().strip():
                pass
            log("http <-", request)
            if request.split(" ")[1:2] == ["/plist.txt"]:
                body = listing.plist()
                self.wfile.write(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n" % len(body))
                self.wfile.write(body)
            else:
                self.wfile.write(b"HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n")

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    http = socketserver.ThreadingTCPServer((args.bind, args.list_port), Http)
    threading.Thread(target=udp_loop, args=(nat, traversal), daemon=True).start()
    threading.Thread(target=udp_loop, args=(udp_list, listing), daemon=True).start()
    log("mock ready: nat udp %s:%d, list udp+http %s:%d" % (args.bind, args.nat_port, args.bind, args.list_port))
    try:
        http.serve_forever()
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    sys.exit(main())
