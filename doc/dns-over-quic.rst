.. cpp:namespace:: c_ares_async

DNS over QUIC
=============

`RFC 9250 <https://www.rfc-editor.org/rfc/rfc9250>`__ puts DNS on QUIC: a connection to UDP port
853 that negotiates the ALPN ``doq``, and on it one bidirectional stream per query. A stream
carries the query, the end of the stream, and back the answer and its end, each message behind the
two bytes of length that DNS over TCP puts in front of it. c-ares has no transport for it, and no
QUIC either; the library takes `anyhttp <https://github.com/pgit/anyhttp>`__'s, which has plain
QUIC streams next to HTTP/3, on ngtcp2.

It is not there by default. ``-DC_ARES_ASYNC_DOQ=ON`` fetches and builds anyhttp, at the commit
:src:`CMakeLists.txt` pins -- and with it Capy and Boost.HTTP, which anyhttp's HTTP/1.1 is built on,
at pins of anyhttp's -- and makes the target ``c_ares_async::doq``: ``c_ares_async::asio``
with anyhttp and ``C_ARES_ASYNC_DOQ=1``. A resolver from a translation unit compiled with that
takes :cpp:member:`tls_options::quic`:

.. code-block:: cpp

   c_ares_async::resolver resolver(executor, {.enabled = true,
                                              .quic = true,
                                              .hostname = "dns.adguard-dns.com"});
   resolver.set_servers("94.140.14.14");

The rest is what it is for DNS over TLS: the servers move to port 853 where they name none,
:cpp:member:`tls_options::hostname` is what the certificate is checked against and what goes into
SNI, and :cpp:member:`tls_options::verify` can turn the check off, for testing. Without
``C_ARES_ASYNC_DOQ``, :cpp:member:`tls_options::quic` makes the constructor throw
:cpp:type:`system_error` with ``errc::operation_not_supported`` -- it does not fall back to DNS
over TLS, or to anything else.

How it is done
--------------

DNS over TLS works because c-ares does DNS over TCP and the transport puts TLS under the byte
stream, see :doc:`dns-over-tls`. QUIC is no byte stream to put under anything, but what goes on
its streams is what DNS over TCP sends, one message at a time. So c-ares does DNS over TCP here as
well -- ``ARES_FLAG_USEVC | ARES_FLAG_STAYOPEN`` -- and the socket its ``asocket()`` gets is one
end of a ``socketpair()``. :src:`include/c_ares_async/impl/quic_transport.ipp` holds the other end:

.. code-block:: text

   c-ares --[len][query]-->  socketpair --> bridge --> QUIC stream: ID 0, FIN     --> server
   c-ares <--[len][answer]-- socketpair <-- bridge <-- QUIC stream: ID put back   <-- server

* ``aconnect()`` comes back at once, as the socketpair is connected from the start, and starts the
  QUIC connection to the address c-ares has for the server. The queries that c-ares writes in the
  meantime wait in the socketpair until it is there.
* The bridge reads the queries off its end, opens a stream for each, and sends it with a message ID
  of 0 and the end of the stream after it (section 4.2.1). The answer gets the ID back that c-ares
  chose, and goes into the socketpair, where c-ares reads it as it would from a TCP connection.
* At the limit of streams the server allows, a query waits for one of them to finish.
* A connection that cannot be made, or ends -- the server closing it, the idle timeout -- closes
  the bridge's end, and c-ares reads the end of its stream. It fails or reconnects the way it does
  for TCP. Waiting for the connection to end is waiting for a stream from the server, which a DoQ
  server does not open (section 4.2): it comes back when the connection is gone, and one that does
  come back with a stream is a protocol error.
* An answer that is not one message on its stream is a protocol error of the connection
  (section 4.3.3), which is closed with ``DOQ_PROTOCOL_ERROR``. Streams that are given up are reset
  with ``DOQ_REQUEST_CANCELLED``, and a connection that c-ares closes is closed with
  ``DOQ_NO_ERROR``.

To c-ares, all of this is a TCP connection: its timeouts and retries are the ones it has for TCP,
and the readiness of its socket is that of a socket. The resolver does not do anything for DoQ
that it does not do for UDP -- unlike DNS over TLS, it has no handshake to drive.

The bridge outlives the resolver where it has to: a connection that is still being made, or a
stream that has not been answered, end on the executor after the resolver has gone. It keeps
itself alive for that, and does not touch the resolver, or its logger, from the moment c-ares has
closed the socket.

What anyhttp needed
-------------------

anyhttp's ``quic::connect()`` used to resolve the host it was given and check the certificate
against that same host, which is the wrong way round for a resolver: c-ares has the server's
address and nothing else, and the name comes from :cpp:member:`tls_options::hostname`. Nor could the
check be turned off. `anyhttp#50 <https://github.com/pgit/anyhttp/pull/50>`__ added both, and the
pin is at its merge:

* ``quic::connect(executor, endpoints, host, config)``, to the endpoints given, with the
  certificate checked for ``host`` -- or the chain alone, and no SNI, for an empty one;
* ``quic::Config::tls_verify``.

What is not there
-----------------

* **Corosio.** anyhttp's runtime is chosen when it is built, and anyhttp pins Capy, and for
  Corosio Corosio as well, at other commits than this library does, and needs OpenSSL 3.5 there. DoQ is for
  ``C_ARES_ASYNC_API=ASIO`` only.
* **Installing.** ``c_ares_async::doq`` is not installed, as anyhttp is built as part of this build
  and not installed with it.
* **One program, one choice.** The resolver is the same object with and without
  ``C_ARES_ASYNC_DOQ``, but its constructor is not: the translation units of a program have to
  agree on the macro, which is what linking ``c_ares_async::doq`` wherever it uses a resolver does.
* **Padding.** Section 5.4 recommends padding the messages (RFC 8467), and c-ares does not pad.
* **0-RTT and session resumption,** which anyhttp's client does not do: every connection is a full
  handshake, which ``ARES_FLAG_STAYOPEN`` keeps to one per server, until the connection idles out
  -- after 30 seconds, or what the server says.
* **A local address to send from.** anyhttp has no way of being given one, so a channel with a
  local address set sends from wherever the system routes the datagrams.

anyhttp logs to spdlog's default logger, at the level of information for every connection it
makes: a program that does not want that sets the level, as :src:`test/fake_dns_server.cpp` does.
The resolver's own messages go to :cpp:func:`resolver::set_logger`, and a certificate that is not
taken is, as for TLS, a connection that failed:

.. code-block:: text

   error fd=8 DoQ connection to 94.140.14.14:853 failed: Connection refused
