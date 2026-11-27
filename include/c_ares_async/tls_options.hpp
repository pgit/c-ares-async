// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// How a resolver uses DNS over TLS, QUIC or HTTPS. It is the same on either runtime, so it is in
// the namespace itself rather than in the inline one of a runtime.
//
#pragma once

#include <string>

namespace c_ares_async
{

/** Whether a @ref resolver uses DNS over TLS (RFC 7858), DNS over QUIC
    (RFC 9250) or DNS over HTTPS (RFC 8484), and how.

    c-ares has no encrypted transport of its own. What the library does is
    to put a TLS session under the TCP connections that c-ares opens, which
    is all it takes to turn DNS over TCP into DNS over TLS -- or to have a
    QUIC connection answer for them, one stream per query, which makes it
    DNS over QUIC, or an HTTP/2 session, one request per query, which makes
    it DNS over HTTPS.
 */
struct tls_options
{
   /** Sends every query over TLS: no UDP, and no TCP in the clear either. */
   bool enabled = false;

   /** With @ref enabled, sends the queries over QUIC rather than over TCP:
       DNS over QUIC, on UDP port 853, with the same @ref hostname and
       @ref verify.

       Only where the library is built with it, with anyhttp's QUIC: the
       target `c_ares_async::anyhttp`, which defines `C_ARES_ASYNC_ANYHTTP`.
       Without that, a resolver asked for it cannot be made.
    */
   bool quic = false;

   /** With @ref enabled, sends the queries as HTTPS requests rather than
       over TLS: DNS over HTTPS, each query a POST to @ref path, on TCP port
       443, with the same @ref hostname and @ref verify. HTTP/2, or HTTP/1.1
       where that is all the server speaks, or all that @ref http2 allows.

       Only where the library is built with it, with anyhttp's HTTP client,
       as for @ref quic. It is the one or the other: a resolver asked for
       both cannot be made.
    */
   bool https = false;

   /** The name of the server, for SNI and for its certificate to be checked
       against.

       c-ares knows its servers by address and nothing else, so the name has
       to come from here. Without one there is no SNI, and @ref verify still
       checks that the certificate is one to trust but not whose it is --
       which is no more than knowing that the server is somebody.

       With @ref https, it is also the host of the URL that the queries go
       to, and the server's address is where there is none: the certificate
       is then checked against that address, which takes a certificate that
       is issued for it.
    */
   std::string hostname;

   /** Whether the certificate of the server is verified, against the trust
       store that the TLS library uses by default.

       Without this any certificate is taken, and with it anybody who is in
       a position to answer for the server. That is for testing.
    */
   bool verify = true;

   /** Whether DNS over TLS offers `dot` in ALPN, the protocol ID that is
       registered for it (RFC 9103, which has zone transfers over TLS insist
       on it; RFC 9461 has it as `alpn=dot` in SVCB records).

       Plain DNS over TLS (RFC 7858) does without, and a server that does not
       do ALPN, or does not care, ignores the offer -- most of them do. One
       that does ALPN for other protocols only is meant to break off the
       handshake, though (RFC 7301, section 3.2), which is what turning it
       off is for. What the server picked, if anything, is in the message
       the logger gets for the handshake.

       Only for DNS over TLS: DNS over QUIC has `doq` and DNS over HTTPS has
       `h2` or `http/1.1`, which they cannot do without -- see @ref http2
       for which of those.
    */
   bool alpn = true;

   /** With @ref https, the path of the URL that the queries are sent to:
       `/dns-query`, as most servers have it, unless told otherwise. The rest
       of the URL is `https://`, @ref hostname and the port of the server.
    */
   std::string path = "/dns-query";

   /** With @ref https, whether ALPN offers `h2` ahead of `http/1.1`, and
       the session speaks what the server picks of the two. Without it,
       `http/1.1` is all that is offered, and HTTP/1.1 all that is spoken,
       with a server that would have HTTP/2 as well: one request at a time,
       its answer included. That is for a server whose HTTP/2 is not to be
       relied on, or for seeing what its HTTP/1.1 does.
    */
   bool http2 = true;
};

} // namespace c_ares_async
