// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// DNS over QUIC (RFC 9250) for c-ares, with anyhttp's QUIC. Only where C_ARES_ASYNC_DOQ is 1,
// which c_ares_async::doq defines: without it, this header is empty. With it, on either runtime --
// the one anyhttp is built for, which has to be the one of the translation unit.
//
// c-ares has DNS over TCP, which is length-prefixed messages on a byte stream, and DoQ is the same
// messages with the same prefix, one on each QUIC stream of its own (RFC 9250, section 4.2). So
// c-ares does DNS over TCP, as it does for DNS over TLS -- but the socket that its asocket() gives
// it is one end of a socketpair(). The other end is ours: a bridge reads the queries off it, sends
// each one on a stream of its own, and writes the answers back, so c-ares sees a TCP connection
// that answers. What it waits for and when it gives up are what they are for TCP, and the
// readiness of its socket is a socket's.
//
//    c-ares  --[len][query]-->  socketpair  --> bridge --> QUIC stream, ID 0, FIN --> server
//    c-ares  <--[len][answer]-- socketpair  <-- bridge <-- QUIC stream, ID put back <-- server
//
// A QUIC connection that cannot be made, or goes away, is the end of the socketpair as well:
// c-ares reads the end of its stream, and fails the server the way it does for TCP.
//
// https://www.rfc-editor.org/rfc/rfc9250
// https://c-ares.org/docs/ares_set_socket_functions_ex.html
//
#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_DOQ

#include <c_ares_async/logging.hpp>
#include <c_ares_async/tls_options.hpp>

#include <anyhttp/runtime.hpp>

#if ANYHTTP_COROSIO != C_ARES_ASYNC_COROSIO
#error "C_ARES_ASYNC_DOQ: anyhttp is built for the other runtime than this translation unit's"
#endif

#include <ares.h>

#include <cstdint>
#include <map>
#include <memory>

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
public:
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

private:
   class bridge;

   //
   // The socket functions themselves. c-ares calls the static thunks, which forward here.
   //
   ares_socket_t on_socket(int domain, int type, int protocol);
   int on_close(ares_socket_t fd);
   int on_connect(ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length);
   int on_getsockname(ares_socket_t fd, struct sockaddr* address, ares_socklen_t* length);

   static const struct ares_socket_functions_ex& functions();

   Executor m_executor;
   tls_options m_options;
   const log_function& m_logger;
   std::map<ares_socket_t, std::shared_ptr<bridge>> m_bridges;
};

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail

#include <c_ares_async/impl/quic_transport.ipp>

#endif // C_ARES_ASYNC_DOQ
