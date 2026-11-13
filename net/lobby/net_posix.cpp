// SPDX-License-Identifier: GPL-3.0-or-later
/* Platform file: BSD sockets for the lobby's TCP connection. The same calls exist on macOS and in
 * libnx (socketInitialize must have run on the Switch); the few platform differences are marked. */
#include "net_io.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace znxlobby
{

uint64_t now_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t now_us()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t random_u64()
{
    // Not for secrets: WebSocket keys and masks, ping nonces. std::random_device is not
    // dependable on every libc, so mix the clocks and an address into the seed.
    static std::mt19937_64 rng([] {
        uint64_t seed = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        seed ^= reinterpret_cast<uintptr_t>(&seed) * 0x9e3779b97f4a7c15ull;
        seed ^= static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) << 1;
        return seed;
    }());
    return rng();
}

int tcp_connect_start(const std::string& host, uint16_t port, std::string& error)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;   // RMG-K's servers are IPv4; libnx has no IPv6
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const std::string service = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &list);
    if (rc != 0 || list == nullptr)
    {
        error = "cannot resolve " + host + ": " + gai_strerror(rc);
        return -1;
    }
    int fd = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
    if (fd < 0)
    {
        error = std::string("socket: ") + std::strerror(errno);
        freeaddrinfo(list);
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    // macOS: a write to a reset connection must not raise SIGPIPE (Linux uses MSG_NOSIGNAL).
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    const int c = ::connect(fd, list->ai_addr, list->ai_addrlen);
    freeaddrinfo(list);
    if (c != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK)
    {
        error = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

int tcp_connect_poll(int fd, std::string& error)
{
    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(fd, &wr);
    timeval tv{0, 0};
    const int n = select(fd + 1, nullptr, &wr, nullptr, &tv);
    if (n < 0)
    {
        if (errno == EINTR)
            return 0;
        error = std::string("select: ") + std::strerror(errno);
        return -1;
    }
    if (n == 0)
        return 0;
    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0)
        soerr = errno;
    if (soerr != 0)
    {
        error = std::string("connect: ") + std::strerror(soerr);
        return -1;
    }
    return 1;
}

void tcp_close(int fd)
{
    if (fd >= 0)
        ::close(fd);
}

std::string tcp_local_address(int fd)
{
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) != 0 || sa.sin_family != AF_INET)
        return {};
    char buf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof(buf)))
        return {};
    return buf;
}

namespace
{
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

class PlainStream final : public Stream
{
  public:
    explicit PlainStream(int fd) : m_fd(fd) {}
    ~PlainStream() override { tcp_close(m_fd); }
    int handshake(std::string&) override { return 1; }
    long read(void* buf, size_t len) override
    {
        const ssize_t n = ::recv(m_fd, buf, len, 0);
        if (n > 0)
            return static_cast<long>(n);
        if (n == 0)
            return -1; // orderly close
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    }
    long write(const void* buf, size_t len) override
    {
        const ssize_t n = ::send(m_fd, buf, len, kSendFlags);
        if (n >= 0)
            return static_cast<long>(n);
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    }
    int fd() const override { return m_fd; }

  private:
    int m_fd;
};
} // namespace

Stream* make_plain_stream(int fd)
{
    return new PlainStream(fd);
}

#ifndef ZNX_LOBBY_TLS
Stream* make_tls_stream(int, const std::string&)
{
    return nullptr;
}
#endif

} // namespace znxlobby
