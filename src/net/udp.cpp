#include "net/udp.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

namespace ob::net {

std::optional<Endpoint> Endpoint::parse(const std::string& host_port) {
    const auto colon = host_port.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    const std::string host = host_port.substr(0, colon);
    const int port = std::atoi(host_port.c_str() + colon + 1);
    if (port <= 0 || port > 65535) return std::nullopt;
    Endpoint ep;
    ep.addr.sin_family = AF_INET;
    ep.addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &ep.addr.sin_addr) != 1) return std::nullopt;
    return ep;
}

std::string Endpoint::to_string() const {
    char buf[INET_ADDRSTRLEN]{};
    ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

bool Endpoint::is_multicast() const {
    const auto ip = ntohl(addr.sin_addr.s_addr);
    return (ip >> 28) == 0xE;  // 224.0.0.0/4
}

namespace {
[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}
}  // namespace

UdpSocket::UdpSocket() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) fail("socket");
    const int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

UdpSocket::~UdpSocket() {
    if (fd_ >= 0) ::close(fd_);
}

UdpSocket::UdpSocket(UdpSocket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
}

void UdpSocket::bind(std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) fail("bind");
}

void UdpSocket::join_multicast(const Endpoint& group) {
    ip_mreq req{};
    req.imr_multiaddr = group.addr.sin_addr;
    req.imr_interface.s_addr = htonl(INADDR_ANY);
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &req, sizeof(req)) != 0) {
        fail("IP_ADD_MEMBERSHIP");
    }
}

void UdpSocket::set_recv_buffer(int bytes) {
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
}

std::uint16_t UdpSocket::local_port() const {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0) fail("getsockname");
    return ntohs(a.sin_port);
}

void UdpSocket::send_to(const Endpoint& to, std::span<const std::byte> data) {
    const auto n = ::sendto(fd_, data.data(), data.size(), 0,
                            reinterpret_cast<const sockaddr*>(&to.addr), sizeof(to.addr));
    if (n < 0) fail("sendto");
}

std::size_t UdpSocket::recv(std::span<std::byte> buf, int timeout_ms, Endpoint* from) {
    // timeout 0 = pure non-blocking drain: skip poll(), halving the syscall
    // cost per datagram — the difference between keeping up and overflowing
    // the kernel buffer at high packet rates.
    if (timeout_ms != 0) {
        pollfd pfd{fd_, POLLIN, 0};
        const int r = ::poll(&pfd, 1, timeout_ms);
        if (r <= 0) return 0;  // timeout (or EINTR: caller just retries)
    }
    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    const auto n = ::recvfrom(fd_, buf.data(), buf.size(), timeout_ms == 0 ? MSG_DONTWAIT : 0,
                              reinterpret_cast<sockaddr*>(&src), &slen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        fail("recvfrom");
    }
    if (from != nullptr) from->addr = src;
    return static_cast<std::size_t>(n);
}

}  // namespace ob::net
