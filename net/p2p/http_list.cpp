// SPDX-License-Identifier: GPL-3.0-or-later
/* znx_p2p_fetch_list: the public list of waiting P2P games, http://p2plist.smash64.net:27887/plist.txt
 * (KailleraWaitingGamesDialog.cpp kPlistUrl), fetched with a plain HTTP/1.0 GET on a BSD socket so
 * that it needs nothing but sockets on every platform (libnx, macOS, Linux). Plain HTTP, as RMG-K
 * fetches it. Blocking, bounded: about 3 s to connect, 3 s without data ends the read. */

#include "znx_p2p.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
bool wait_socket(int fd, bool for_write, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    const int n = select(fd + 1, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr, &tv);
    return n > 0;
}
} // namespace

extern "C" int znx_p2p_fetch_list(const char* host, int port, struct znx_p2p_listing* out, int max, char* error,
                                  size_t error_size)
{
    auto fail = [&](const char* text) {
        std::snprintf(error, error_size, "%s", text);
        return -1;
    };
    if (host == nullptr || host[0] == 0)
        host = ZNX_P2P_LIST_HOST;
    if (port <= 0)
        port = ZNX_P2P_LIST_PORT;
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%d", port);
    if (getaddrinfo(host, port_text, &hints, &result) != 0 || result == nullptr)
        return fail("The list server's name could not be looked up. Check the internet connection.");
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        freeaddrinfo(result);
        return fail("No network socket.");
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        close(fd);
        return fail("The list server cannot be reached.");
    }
    if (rc != 0) {
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (!wait_socket(fd, true, 3000) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
            close(fd);
            return fail("The list server did not answer.");
        }
    }
    std::string request = std::string("GET /plist.txt HTTP/1.0\r\nHost: ") + host + ":" + port_text +
                          "\r\nUser-Agent: " ZNX_P2P_APP "\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        if (!wait_socket(fd, true, 3000)) {
            close(fd);
            return fail("The list server stopped answering.");
        }
        const ssize_t n = send(fd, request.data() + sent, request.size() - sent, 0);
        if (n <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(fd);
            return fail("The list server closed the connection.");
        }
        if (n > 0)
            sent += (size_t)n;
    }
    std::string body;
    char buffer[4096];
    for (;;) {
        if (!wait_socket(fd, false, 3000))
            break;
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        body.append(buffer, (size_t)n);
        if (body.size() > (1u << 20))
            break;
    }
    close(fd);
    if (body.compare(0, 5, "HTTP/") == 0) {
        const size_t space = body.find(' ');
        const int status = space != std::string::npos ? std::atoi(body.c_str() + space + 1) : 0;
        if (status != 200)
            return fail("The list server answered with an error.");
    }
    error[0] = 0;
    return znx_p2p_parse_list(body.c_str(), out, max);
}
