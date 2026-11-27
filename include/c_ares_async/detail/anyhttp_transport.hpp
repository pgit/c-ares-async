// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// DNS over QUIC (RFC 9250) and DNS over HTTPS (RFC 8484) for c-ares, with anyhttp. Only where
// C_ARES_ASYNC_ANYHTTP is 1, which c_ares_async::anyhttp defines: without it, this header is
// empty. With it, on either runtime -- the one anyhttp is built for, which has to be the one of
// the translation unit.
//
// c-ares has DNS over TCP, which is length-prefixed messages on a byte stream. DoQ is the same
// messages with the same prefix, one on each QUIC stream of its own (RFC 9250, section 4.2), and
// DoH is the same messages without the prefix, one in the body of each HTTP request (RFC 8484,
// section 4.1). So c-ares does DNS over TCP, as it does for DNS over TLS, and the socket
// functions are where the transport goes in, as they are for TLS: what c-ares sends on a socket
// of ours is cut into its messages, and each goes out on a stream of its own; what comes back is
// what it receives.
//
//    c-ares  --[len][query]-->  asendto()   --> bridge --> QUIC stream, ID 0, FIN    --> server
//    c-ares  <--[len][answer]-- arecvfrom() <-- bridge <-- QUIC stream, ID put back  <-- server
//
//    c-ares  --[len][query]-->  asendto()   --> bridge --> POST, ID 0                --> server
//    c-ares  <--[len][answer]-- arecvfrom() <-- bridge <-- 200, ID put back, prefixed <-- server
//
// The bridge is in detail/anyhttp_bridge.hpp, which is the c-ares side of it, and the protocols in
// detail/doq_bridge.hpp and detail/doh_bridge.hpp.
//
// Where TLS has a socket of the system's under it, QUIC has a UDP socket, its retransmissions and
// its timers, which are anyhttp's, and HTTP/2 a session that is anyhttp's as well, with nothing
// whose readiness would mean that an answer is there. So a socket of ours is no socket at all --
// the descriptor c-ares gets is only there to reserve its number -- and it says itself when it can
// be read: the resolver waits on a descriptor of ours for it, in place of one of the event loop's.
// What c-ares waits for and when it gives up are what they are for TCP.
//
// A connection that cannot be made, or goes away, is the end of the stream to c-ares, which fails
// the server the way it does for TCP.
//
// https://www.rfc-editor.org/rfc/rfc9250
// https://www.rfc-editor.org/rfc/rfc8484
// https://c-ares.org/docs/ares_set_socket_functions_ex.html
//
#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_ANYHTTP

#include <c_ares_async/detail/event_loop.hpp>
#include <c_ares_async/logging.hpp>
#include <c_ares_async/tls_options.hpp>

#include <anyhttp/runtime.hpp>

#if ANYHTTP_COROSIO != C_ARES_ASYNC_COROSIO
#error "C_ARES_ASYNC_ANYHTTP: anyhttp is built for the other runtime than this translation unit's"
#endif

#include <ares.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail
{

class anyhttp_bridge;

class anyhttp_transport
{
public:
   class descriptor;

   //
   // 'options' say which of the two protocols it is: DoH with 'https', DoQ with 'quic'.
   //
   // 'logger' is the resolver's, and looked at afresh for every message. What runs on after the
   // resolver is gone -- a connection that is still being made, a query that is still out -- does
   // not look at it any more.
   //
   anyhttp_transport(Executor executor, tls_options options, const log_function& logger);
   ~anyhttp_transport();

   anyhttp_transport(const anyhttp_transport&) = delete;
   anyhttp_transport& operator=(const anyhttp_transport&) = delete;

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

   std::shared_ptr<anyhttp_bridge> make_bridge(ares_socket_t fd, int domain) const;
   anyhttp_bridge* find(ares_socket_t fd) const;

   static const struct ares_socket_functions_ex& functions();

   Executor m_executor;
   tls_options m_options;
   const log_function& m_logger;
   std::map<ares_socket_t, std::shared_ptr<anyhttp_bridge>> m_bridges;
};

//
// A socket of ours, as the resolver waits on it: the three calls of the event loop's Descriptor,
// for a socket that says itself when it is ready. It can be read once there is an answer, or once
// the connection is over, which c-ares reads as the end of its stream. It can always be written,
// as what c-ares sends is taken as a whole.
//
// A wait that is ready completes from the executor's queue, never from inside the call, as c-ares
// arms its sockets from inside its own functions and does not survive being called back there.
//
class anyhttp_transport::descriptor
{
public:
   explicit descriptor(std::shared_ptr<anyhttp_bridge> bridge) noexcept;
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
   std::shared_ptr<anyhttp_bridge> m_bridge;
};

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail

#include <c_ares_async/impl/anyhttp_transport.ipp>

#endif // C_ARES_ASYNC_ANYHTTP
