// SPDX-License-Identifier: GPL-3.0-or-later
/* The byte stream under the lobby's WebSocket: a non-blocking TCP socket (BSD sockets: macOS and
 * libnx alike), optionally wrapped in TLS. Everything is polled; nothing here blocks except the
 * name lookup (getaddrinfo), and the lobby servers RMG-K knows are IP literals.
 *
 * TLS sits behind Stream: make_tls_stream() returns nullptr in a build without a TLS backend
 * (every lobby URL in RMG-K 0.9.16 is ws://, LobbyConnectDialog.cpp:31-35). A Switch build that
 * needs wss:// adds a backend file (mbedTLS from devkitPro's portlibs) implementing it. */
#ifndef ZNX_LOBBY_NET_IO_H
#define ZNX_LOBBY_NET_IO_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace znxlobby
{

uint64_t now_ms();   /* monotonic milliseconds */
uint64_t now_us();   /* monotonic microseconds */
uint64_t random_u64();

/* Starts a non-blocking TCP connect. Returns the socket or -1 (error set). */
int tcp_connect_start(const std::string& host, uint16_t port, std::string& error);
/* 1 connected, 0 still connecting, -1 failed (error set). */
int tcp_connect_poll(int fd, std::string& error);
void tcp_close(int fd);
/* The local address the socket was routed through ("" if unknown). */
std::string tcp_local_address(int fd);

class Stream
{
  public:
    virtual ~Stream() = default;
    /* 1 ready for application data, 0 call again, -1 failed. */
    virtual int handshake(std::string& error) = 0;
    /* >0 bytes, 0 would block, -1 closed or failed. */
    virtual long read(void* buf, size_t len) = 0;
    virtual long write(const void* buf, size_t len) = 0;
    virtual int fd() const = 0;
};

/* Takes ownership of fd. */
Stream* make_plain_stream(int fd);
/* Takes ownership of fd on success. nullptr when this build has no TLS backend. */
Stream* make_tls_stream(int fd, const std::string& host);

} // namespace znxlobby

#endif
