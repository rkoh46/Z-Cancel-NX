// SPDX-License-Identifier: GPL-3.0-or-later
/* A WebSocket client (RFC 6455), driven by poll(): what QWebSocket does for RMG-K's LobbyClient.
 * Text messages only in practice (the lobby speaks JSON text frames); binary frames are reported
 * the same way. Answers pings, reassembles fragments, masks every frame it sends. */
#ifndef ZNX_LOBBY_WS_H
#define ZNX_LOBBY_WS_H

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "net_io.h"

namespace znxlobby
{

struct WsEvent
{
    enum Type { Opened, Message, Closed } type;
    std::string data;   // message text, or the reason for Closed
};

class WsClient
{
  public:
    enum class State { Idle, Connecting, Tls, Handshake, Open, Closing, Closed };

    WsClient() = default;
    ~WsClient();

    /* ws://host[:port]/path or wss://... Returns false (and error) if the URL is unusable. */
    bool open(const std::string& url, std::string& error);
    /* Drives the connection. Returns true and fills ev while there are events. */
    bool poll(WsEvent& ev);
    bool sendText(const std::string& text);
    void close(uint16_t code = 1000, const std::string& reason = std::string());
    /* Drops the connection without a closing handshake. */
    void abort(const std::string& reason);

    State state() const { return m_state; }
    bool isOpen() const { return m_state == State::Open; }
    size_t pendingWrite() const { return m_out.size() - m_outPos; }
    std::string localAddress() const;

    /* Frames larger than this are refused (RMG-K's server limit is 1 MiB, LobbyClient.cpp:3515). */
    size_t maxMessage = 4u * 1024 * 1024;
    uint64_t connectTimeoutMs = 15000;

  private:
    void step();
    void fail(const std::string& reason);
    void queueFrame(uint8_t opcode, const void* data, size_t len);
    void flush();
    bool readSome();
    void parseHandshake();
    void parseFrames();

    State m_state = State::Idle;
    std::string m_host;
    uint16_t m_port = 0;
    std::string m_path;
    bool m_tls = false;
    int m_fd = -1;
    std::unique_ptr<Stream> m_stream;
    uint64_t m_deadline = 0;
    std::string m_key;
    std::string m_in;
    std::string m_out;
    size_t m_outPos = 0;
    std::string m_fragment;
    uint8_t m_fragmentOpcode = 0;
    bool m_closeSent = false;
    std::deque<WsEvent> m_events;
};

/* Exposed for the self-test. */
std::string base64_encode(const uint8_t* data, size_t len);
void sha1(const uint8_t* data, size_t len, uint8_t out[20]);

} // namespace znxlobby

#endif
