// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The event loop of Asio, see ../detail/event_loop.hpp.
//
#pragma once

#include <c_ares_async/runtime.hpp>

#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace c_ares_async::inline asio::detail
{

//
// What readiness to wait for. c-ares asks for one or the other, or for both at a time.
//
enum class wait_type
{
   read,
   write
};

//
// One socket of c-ares', watched but not owned: the event loop is used to wait for readiness on
// it and for nothing else, which is why the descriptor is released again instead of being closed.
//
class Descriptor
{
public:
   //
   // Throws system_error where 'fd' cannot be watched, as Asio's own constructor does.
   //
   Descriptor(const Executor& executor, int fd) : descriptor_(executor, fd) {}
   ~Descriptor() { detach(); }

   Descriptor(const Descriptor&) = delete;
   Descriptor& operator=(const Descriptor&) = delete;

   bool is_open() const noexcept { return descriptor_.is_open(); }

   //
   // Cancels what is pending and gives the file descriptor back, without closing it. The
   // callbacks of the cancelled waits still run, with errors::canceled.
   //
   void detach()
   {
      if (descriptor_.is_open())
         descriptor_.release();
   }

   //
   // Calls 'on_ready(error_code)' once, when the descriptor is ready for 'type' -- or with an
   // error, errors::canceled among them, where it never gets there.
   //
   template <typename F>
   void wait(wait_type type, F&& on_ready)
   {
      descriptor_.async_wait(type == wait_type::read
                                ? boost::asio::posix::stream_descriptor::wait_read
                                : boost::asio::posix::stream_descriptor::wait_write,
                             std::forward<F>(on_ready));
   }

private:
   boost::asio::posix::stream_descriptor descriptor_;
};

//
// Our end of a socketpair() that c-ares has the other end of, for what it takes to be a TCP
// connection: that is how DNS over QUIC gets to c-ares, see quic_transport.hpp. A Unix domain
// stream socket of the runtime's own, made from the executor, which closes the descriptor it has
// adopted when it is closed or destroyed.
//
using LocalSocket = boost::asio::local::stream_protocol::socket;

//
// Has 'socket' take 'fd', which it closes from then on. Where that fails, 'fd' is still the
// caller's.
//
inline error_code adopt_socket(LocalSocket& socket, int fd) noexcept
{
   error_code ec;
   socket.assign(boost::asio::local::stream_protocol(), fd, ec);
   return ec;
}

//
// Closes 'socket', which completes what is pending on it with an error.
//
inline void close_socket(LocalSocket& socket) noexcept
{
   error_code ignored;
   socket.close(ignored);
}

//
// The bytes of an IPv4 or IPv6 address, as the text of it. What c-ares hands over in the
// "ipv4hint" and "ipv6hint" parameters of an HTTPS record is the wire format, not an endpoint.
//
inline std::string address_text(const std::array<unsigned char, 4>& bytes)
{
   return boost::asio::ip::address_v4(bytes).to_string();
}

inline std::string address_text(const std::array<unsigned char, 16>& bytes)
{
   return boost::asio::ip::address_v6(bytes).to_string();
}

//
// 'address' as a TcpEndpoint of the runtime's own, or nothing where it is of a family that the
// runtime has no endpoint for.
//
inline std::optional<TcpEndpoint> make_endpoint(const sockaddr* address, std::size_t length)
{
   if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in))
   {
      sockaddr_in sa{};
      std::memcpy(&sa, address, sizeof(sa));

      boost::asio::ip::address_v4::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin_addr, bytes.size());
      return TcpEndpoint(boost::asio::ip::address_v4(bytes), ntohs(sa.sin_port));
   }

   if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6))
   {
      sockaddr_in6 sa{};
      std::memcpy(&sa, address, sizeof(sa));

      boost::asio::ip::address_v6::bytes_type bytes;
      std::memcpy(bytes.data(), &sa.sin6_addr, bytes.size());
      return TcpEndpoint(boost::asio::ip::address_v6(bytes, sa.sin6_scope_id), ntohs(sa.sin6_port));
   }

   return std::nullopt;
}

} // namespace c_ares_async::inline asio::detail
