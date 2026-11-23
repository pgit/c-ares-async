// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// How a resolver uses DNS over TLS. It is the same on either runtime, so it is in the namespace
// itself rather than in the inline one of a runtime.
//
#pragma once

#include <string>

namespace c_ares_async
{

/** Whether a @ref resolver uses DNS over TLS (RFC 7858) or DNS over QUIC
    (RFC 9250), and how.

    c-ares has no encrypted transport of its own. What the library does is
    to put a TLS session under the TCP connections that c-ares opens, which
    is all it takes to turn DNS over TCP into DNS over TLS -- or to have a
    QUIC connection answer for them, one stream per query, which makes it
    DNS over QUIC.
 */
struct tls_options
{
   /** Sends every query over TLS: no UDP, and no TCP in the clear either. */
   bool enabled = false;

   /** With @ref enabled, sends the queries over QUIC rather than over TCP:
       DNS over QUIC, on UDP port 853, with the same @ref hostname and
       @ref verify.

       Only where the library is built with it, with anyhttp's QUIC: the
       target `c_ares_async::doq`, which defines `C_ARES_ASYNC_DOQ`. Without
       that, a resolver asked for it cannot be made.
    */
   bool quic = false;

   /** The name of the server, for SNI and for its certificate to be checked
       against.

       c-ares knows its servers by address and nothing else, so the name has
       to come from here. Without one there is no SNI, and @ref verify still
       checks that the certificate is one to trust but not whose it is --
       which is no more than knowing that the server is somebody.
    */
   std::string hostname;

   /** Whether the certificate of the server is verified, against the trust
       store that the TLS library uses by default.

       Without this any certificate is taken, and with it anybody who is in
       a position to answer for the server. That is for testing.
    */
   bool verify = true;
};

} // namespace c_ares_async
