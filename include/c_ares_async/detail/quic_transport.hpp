// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// DNS over QUIC (RFC 9250) for c-ares, with anyhttp's QUIC. Only where C_ARES_ASYNC_DOQ is 1,
// which c_ares_async::doq defines: without it, this header is empty. With it, on either runtime --
// the one anyhttp is built for, which has to be the one of the translation unit.
//
// c-ares has DNS over TCP, which is length-prefixed messages on a byte stream, and DoQ is the same
// messages with the same prefix, one on each QUIC stream of its own (RFC 9250, section 4.2). So
// c-ares does DNS over TCP, as it does for DNS over TLS, and the socket functions are where the
// transport goes in, as they are for TLS: what c-ares sends on a socket of ours is cut into its
// messages, and each goes out on a stream of its own; what comes back is what it receives.
//
//    c-ares  --[len][query]-->  asendto()   --> bridge --> QUIC stream, ID 0, FIN --> server
//    c-ares  <--[len][answer]-- arecvfrom() <-- bridge <-- QUIC stream, ID put back <-- server
//
// Where TLS has a socket of the system's under it, QUIC has a UDP socket, its retransmissions and
// its timers, which are anyhttp's, and nothing whose readiness would mean that an answer is there.
// So a socket of ours is no socket at all -- the descriptor c-ares gets is only there to reserve
// its number -- and it says itself when it can be read: the resolver waits on a descriptor of
// ours for it, in place of one of the event loop's. What c-ares waits for and when it gives up
// are what they are for TCP.
//
// A QUIC connection that cannot be made, or goes away, is the end of the stream to c-ares, which
// fails the server the way it does for TCP.
//
// https://www.rfc-editor.org/rfc/rfc9250
// https://c-ares.org/docs/ares_set_socket_functions_ex.html
//
#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_DOQ

#include <c_ares_async/detail/event_loop.hpp>
#include <c_ares_async/logging.hpp>
#include <c_ares_async/tls_options.hpp>

#include <anyhttp/runtime.hpp>

#if ANYHTTP_COROSIO != C_ARES_ASYNC_COROSIO
#error "C_ARES_ASYNC_DOQ: anyhttp is built for the other runtime than this translation unit's"
#endif

#include <ares.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail
{

//
// The application error codes of DoQ, RFC 9250, section 4.3.
//
enum class doq_error : std::uint64_t
{
   no_error = 0x0,
   internal_error = 0x1,
   protocol_error = 0x2,
   request_cancelled = 0x3,
   excessive_load = 0x4,
   unspecified_error = 0x5,
};

class quic_transport
{
   class bridge;

public:
   class descriptor;

   //
   // 'logger' is the resolver's, and looked at afresh for every message. What runs on after the
   // resolver is gone -- a connection that is still being made, a stream that is still open --
   // does not look at it any more.
   //
   quic_transport(Executor executor, tls_options options, const log_function& logger);
   ~quic_transport();

   quic_transport(const quic_transport&) = delete;
   quic_transport& operator=(const quic_transport&) = delete;

   //
   // Hands our socket functions to the channel, which must not have opened any sockets yet.
   //
   void install(ares_channel_t* channel);

   //
   // What the resolver waits on for 'fd' if it is a socket of ours, or nothing for one of the
   // system's, which the event loop waits on as ever.
   //
   std::optional<descriptor> find_descriptor(ares_socket_t fd) const;

private:
   //
   // The socket functions themselves. c-ares calls the static thunks, which forward here.
   //
   ares_socket_t on_socket(int domain, int type, int protocol);
   int on_close(ares_socket_t fd);
   int on_setsockopt(ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                     ares_socklen_t size);
   int on_connect(ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length);
   ares_ssize_t on_recv(ares_socket_t fd, void* buffer, size_t length);
   ares_ssize_t on_send(ares_socket_t fd, const void* buffer, size_t length, int flags);
   int on_getsockname(ares_socket_t fd, struct sockaddr* address, ares_socklen_t* length);
   int on_bind(ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length);

   bridge* find(ares_socket_t fd) const;

   static const struct ares_socket_functions_ex& functions();

   Executor m_executor;
   tls_options m_options;
   const log_function& m_logger;
   std::map<ares_socket_t, std::shared_ptr<bridge>> m_bridges;
};

//
// A socket of ours, as the resolver waits on it: the three calls of the event loop's Descriptor,
// for a socket that says itself when it is ready. It can be read once there is an answer, or once
// the QUIC connection is over, which c-ares reads as the end of its stream. It can always be
// written, as what c-ares sends is taken as a whole.
//
// A wait that is ready completes from the executor's queue, never from inside the call, as c-ares
// arms its sockets from inside its own functions and does not survive being called back there.
//
class quic_transport::descriptor
{
public:
   explicit descriptor(std::shared_ptr<bridge> bridge) noexcept;
   ~descriptor() { detach(); }

   descriptor(descriptor&&) noexcept = default;
   descriptor& operator=(descriptor&&) = delete;

   bool is_open() const noexcept { return m_bridge != nullptr; }

   //
   // Cancels the wait that is pending, whose callback still runs, with errors::canceled.
   //
   void detach();

   //
   // Calls 'on_ready(error_code)' once, when the socket is ready for 'type', or with
   // errors::canceled where it is detached first.
   //
   void wait(wait_type type, std::function<void(const error_code&)> on_ready);

private:
   std::shared_ptr<bridge> m_bridge;
};

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail

#include <c_ares_async/impl/quic_transport.ipp>

#endif // C_ARES_ASYNC_DOQ
