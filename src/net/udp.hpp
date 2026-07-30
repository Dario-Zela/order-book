#pragma once

// Thin POSIX UDP wrapper: enough socket for MoldUDP64 replay over loopback
// or multicast, nothing more. Blocking receives use a poll() timeout so
// receivers can interleave gap re-requests with reads.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <netinet/in.h>

namespace ob::net {

struct Endpoint {
    sockaddr_in addr{};
    static std::optional<Endpoint> parse(const std::string& host_port);  // "127.0.0.1:5000"
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool is_multicast() const;
};

class UdpSocket {
public:
    UdpSocket();   // throws std::system_error
    ~UdpSocket();
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void bind(std::uint16_t port);                 // 0 = ephemeral
    void join_multicast(const Endpoint& group);    // for multicast feeds
    void set_recv_buffer(int bytes);
    [[nodiscard]] std::uint16_t local_port() const;

    void send_to(const Endpoint& to, std::span<const std::byte> data);
    // Bytes received, 0 on timeout. Fills `from` when provided.
    std::size_t recv(std::span<std::byte> buf, int timeout_ms, Endpoint* from = nullptr);

private:
    int fd_ = -1;
};

}  // namespace ob::net
