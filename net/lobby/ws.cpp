// SPDX-License-Identifier: GPL-3.0-or-later
#include "ws.h"

#include <cctype>
#include <cstring>

namespace znxlobby
{

namespace
{
const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
} // namespace

std::string base64_encode(const uint8_t* data, size_t len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1 < len ? data[i + 1] : 0;
        const uint32_t b2 = i + 2 < len ? data[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? tbl[v & 63] : '=');
    }
    return out;
}

// FIPS 180-1. Only used to check the server's Sec-WebSocket-Accept.
void sha1(const uint8_t* data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string msg(reinterpret_cast<const char*>(data), len);
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56)
        msg.push_back('\0');
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    for (size_t off = 0; off < msg.size(); off += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(msg.data() + off + i * 4);
            w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; ++i)
    {
        out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

WsClient::~WsClient()
{
    if (m_stream)
        m_stream.reset(); // owns m_fd
    else if (m_fd >= 0)
        tcp_close(m_fd);
}

bool WsClient::open(const std::string& url, std::string& error)
{
    if (m_state != State::Idle && m_state != State::Closed)
    {
        error = "already open";
        return false;
    }
    std::string rest;
    if (url.rfind("ws://", 0) == 0)
    {
        m_tls = false;
        rest = url.substr(5);
    }
    else if (url.rfind("wss://", 0) == 0)
    {
        m_tls = true;
        rest = url.substr(6);
    }
    else
    {
        error = "not a ws:// or wss:// URL";
        return false;
    }
    const size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    m_path = slash == std::string::npos ? "/" : rest.substr(slash);
    m_port = m_tls ? 443 : 80;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos)
    {
        const int port = std::atoi(authority.c_str() + colon + 1);
        if (port <= 0 || port > 65535)
        {
            error = "bad port in URL";
            return false;
        }
        m_port = static_cast<uint16_t>(port);
        authority.resize(colon);
    }
    if (authority.empty())
    {
        error = "no host in URL";
        return false;
    }
    m_host = authority;

    m_in.clear();
    m_out.clear();
    m_outPos = 0;
    m_fragment.clear();
    m_closeSent = false;
    m_events.clear();
    m_fd = tcp_connect_start(m_host, m_port, error);
    if (m_fd < 0)
        return false;
    m_state = State::Connecting;
    m_deadline = now_ms() + connectTimeoutMs;
    return true;
}

void WsClient::fail(const std::string& reason)
{
    if (m_state == State::Closed || m_state == State::Idle)
        return;
    if (m_stream)
        m_stream.reset(); // owns m_fd
    else if (m_fd >= 0)
        tcp_close(m_fd);
    m_fd = -1;
    m_state = State::Closed;
    m_events.push_back({WsEvent::Closed, reason});
}

void WsClient::abort(const std::string& reason)
{
    fail(reason);
}

std::string WsClient::localAddress() const
{
    return m_stream ? tcp_local_address(m_stream->fd()) : std::string();
}

void WsClient::queueFrame(uint8_t opcode, const void* data, size_t len)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode)); // FIN
    if (len < 126)
        frame.push_back(static_cast<char>(0x80 | len));
    else if (len <= 0xffff)
    {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xff));
        frame.push_back(static_cast<char>(len & 0xff));
    }
    else
    {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xff));
    }
    const uint64_t r = random_u64();
    uint8_t mask[4] = {uint8_t(r), uint8_t(r >> 8), uint8_t(r >> 16), uint8_t(r >> 24)};
    frame.append(reinterpret_cast<const char*>(mask), 4);
    const auto* p = static_cast<const uint8_t*>(data);
    const size_t start = frame.size();
    frame.resize(start + len);
    for (size_t i = 0; i < len; ++i)
        frame[start + i] = static_cast<char>(p[i] ^ mask[i & 3]);
    if (m_outPos > 0 && m_outPos == m_out.size())
    {
        m_out.clear();
        m_outPos = 0;
    }
    m_out += frame;
}

bool WsClient::sendText(const std::string& text)
{
    if (m_state != State::Open)
        return false;
    queueFrame(0x1, text.data(), text.size());
    flush();
    return m_state == State::Open;
}

void WsClient::close(uint16_t code, const std::string& reason)
{
    if (m_state == State::Open)
    {
        std::string payload;
        payload.push_back(static_cast<char>(code >> 8));
        payload.push_back(static_cast<char>(code & 0xff));
        payload += reason.substr(0, 120);
        queueFrame(0x8, payload.data(), payload.size());
        m_closeSent = true;
        m_state = State::Closing;
        m_deadline = now_ms() + 2000;
        flush();
    }
    else if (m_state != State::Closed && m_state != State::Idle)
    {
        fail("closed");
    }
}

void WsClient::flush()
{
    if (!m_stream)
        return;
    while (m_outPos < m_out.size())
    {
        const long n = m_stream->write(m_out.data() + m_outPos, m_out.size() - m_outPos);
        if (n < 0)
        {
            fail("connection lost while sending");
            return;
        }
        if (n == 0)
            break;
        m_outPos += static_cast<size_t>(n);
    }
    if (m_outPos == m_out.size())
    {
        m_out.clear();
        m_outPos = 0;
    }
}

bool WsClient::readSome()
{
    char buf[16384];
    for (;;)
    {
        const long n = m_stream->read(buf, sizeof(buf));
        if (n < 0)
        {
            // Frames that arrived together with the close (a server that sends HELLO_FAIL and hangs up at
            // once) are delivered first.
            if ((m_state == State::Open || m_state == State::Closing) && !m_in.empty())
                parseFrames();
            fail(m_state == State::Closing ? "closed" : "connection closed by server");
            return false;
        }
        if (n == 0)
            return true;
        m_in.append(buf, static_cast<size_t>(n));
        if (m_in.size() > maxMessage + 64 * 1024)
        {
            // parseFrames consumes as it goes; this only trips on a runaway frame.
            if (m_state == State::Open || m_state == State::Closing)
                return true;
            fail("handshake response too large");
            return false;
        }
    }
}

void WsClient::parseHandshake()
{
    const size_t end = m_in.find("\r\n\r\n");
    if (end == std::string::npos)
        return;
    const std::string head = m_in.substr(0, end);
    m_in.erase(0, end + 4);
    size_t lineEnd = head.find("\r\n");
    const std::string status = head.substr(0, lineEnd);
    if (status.find(" 101") == std::string::npos)
    {
        fail("server refused the WebSocket upgrade: " + status);
        return;
    }
    std::string accept;
    bool upgrade = false;
    while (lineEnd != std::string::npos)
    {
        const size_t next = head.find("\r\n", lineEnd + 2);
        const std::string line = head.substr(lineEnd + 2, next == std::string::npos ? std::string::npos
                                                                                     : next - lineEnd - 2);
        lineEnd = next;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "sec-websocket-accept")
            accept = value;
        else if (name == "upgrade" && lower(value) == "websocket")
            upgrade = true;
    }
    const std::string material = m_key + kGuid;
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(material.data()), material.size(), digest);
    if (!upgrade || accept != base64_encode(digest, 20))
    {
        fail("bad WebSocket handshake response");
        return;
    }
    m_state = State::Open;
    m_events.push_back({WsEvent::Opened, {}});
    parseFrames();
}

void WsClient::parseFrames()
{
    size_t pos = 0;
    while (m_state == State::Open || m_state == State::Closing)
    {
        if (m_in.size() - pos < 2)
            break;
        const auto* p = reinterpret_cast<const uint8_t*>(m_in.data() + pos);
        const bool fin = (p[0] & 0x80) != 0;
        const uint8_t opcode = p[0] & 0x0f;
        const bool masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7f;
        size_t header = 2;
        if (len == 126)
        {
            if (m_in.size() - pos < 4)
                break;
            len = (uint64_t(p[2]) << 8) | p[3];
            header = 4;
        }
        else if (len == 127)
        {
            if (m_in.size() - pos < 10)
                break;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | p[2 + i];
            header = 10;
        }
        if (len > maxMessage)
        {
            fail("WebSocket frame too large");
            return;
        }
        const size_t maskOff = header;
        if (masked)
            header += 4;
        if (m_in.size() - pos < header + len)
            break;
        std::string payload(m_in.data() + pos + header, static_cast<size_t>(len));
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(payload[i] ^ p[maskOff + (i & 3)]);
        pos += header + static_cast<size_t>(len);

        switch (opcode)
        {
        case 0x0: // continuation
            if (m_fragmentOpcode == 0)
            {
                fail("unexpected continuation frame");
                return;
            }
            m_fragment += payload;
            if (m_fragment.size() > maxMessage)
            {
                fail("WebSocket message too large");
                return;
            }
            if (fin)
            {
                m_events.push_back({WsEvent::Message, std::move(m_fragment)});
                m_fragment.clear();
                m_fragmentOpcode = 0;
            }
            break;
        case 0x1:
        case 0x2:
            if (fin)
                m_events.push_back({WsEvent::Message, std::move(payload)});
            else
            {
                m_fragment = std::move(payload);
                m_fragmentOpcode = opcode;
            }
            break;
        case 0x8: // close
        {
            std::string reason = "closed by server";
            if (payload.size() >= 2)
            {
                const int code = (uint8_t(payload[0]) << 8) | uint8_t(payload[1]);
                reason += " (" + std::to_string(code) + (payload.size() > 2 ? " " + payload.substr(2) : "") + ")";
            }
            if (!m_closeSent)
            {
                queueFrame(0x8, payload.data(), payload.size() >= 2 ? 2 : 0);
                m_closeSent = true;
                flush();
            }
            m_in.clear();
            fail(reason);
            return;
        }
        case 0x9: // ping
            if (m_state == State::Open)
                queueFrame(0xA, payload.data(), payload.size());
            break;
        case 0xA: // pong
            break;
        default:
            fail("unknown WebSocket opcode");
            return;
        }
    }
    if (pos > 0 && m_state != State::Closed)
        m_in.erase(0, pos);
}

void WsClient::step()
{
    switch (m_state)
    {
    case State::Idle:
    case State::Closed:
        return;
    case State::Connecting:
    {
        std::string error;
        const int r = tcp_connect_poll(m_fd, error);
        if (r < 0)
        {
            fail(error);
            return;
        }
        if (r == 0)
        {
            if (now_ms() > m_deadline)
                fail("connection timed out");
            return;
        }
        if (m_tls)
        {
            Stream* tls = make_tls_stream(m_fd, m_host);
            if (tls == nullptr)
            {
                fail("wss:// needs a TLS backend, and this build has none");
                return;
            }
            m_stream.reset(tls);
            m_state = State::Tls;
        }
        else
        {
            m_stream.reset(make_plain_stream(m_fd));
            m_state = State::Handshake;
        }
        if (m_state == State::Handshake)
            goto send_request;
        return;
    }
    case State::Tls:
    {
        std::string error;
        const int r = m_stream->handshake(error);
        if (r < 0)
        {
            fail("TLS: " + error);
            return;
        }
        if (r == 0)
        {
            if (now_ms() > m_deadline)
                fail("TLS handshake timed out");
            return;
        }
        m_state = State::Handshake;
        goto send_request;
    }
    case State::Handshake:
        flush();
        if (m_state != State::Handshake)
            return;
        if (!readSome())
            return;
        parseHandshake();
        if (m_state == State::Handshake && now_ms() > m_deadline)
            fail("WebSocket handshake timed out");
        return;
    case State::Open:
    case State::Closing:
        flush();
        if (m_state == State::Closed)
            return;
        if (!readSome())
            return;
        parseFrames();
        if (m_state == State::Closing && now_ms() > m_deadline)
            fail("closed");
        return;
    }
    return;

send_request:
{
    uint8_t key[16];
    for (int i = 0; i < 16; i += 8)
    {
        const uint64_t r = random_u64();
        std::memcpy(key + i, &r, 8);
    }
    m_key = base64_encode(key, 16);
    std::string req = "GET " + m_path + " HTTP/1.1\r\n";
    const bool defaultPort = m_port == (m_tls ? 443 : 80);
    req += "Host: " + m_host + (defaultPort ? std::string() : ":" + std::to_string(m_port)) + "\r\n";
    req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + m_key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";
    m_out += req;
    flush();
}
}

bool WsClient::poll(WsEvent& ev)
{
    if (m_events.empty())
        step();
    if (m_events.empty())
        return false;
    ev = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

} // namespace znxlobby
