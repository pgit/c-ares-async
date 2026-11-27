.. cpp:namespace:: c_ares_async

DNS over QUIC
=============

`RFC 9250 <https://www.rfc-editor.org/rfc/rfc9250>`__ puts DNS on QUIC: a connection to UDP port
853 that negotiates the ALPN ``doq``, and on it one bidirectional stream per query. A stream
carries the query, the end of the stream, and back the answer and its end, each message behind the
two bytes of length that DNS over TCP puts in front of it. c-ares has no transport for it, and no
QUIC either; the library takes `anyhttp <https://github.com/pgit/anyhttp>`__'s, which has plain
QUIC streams next to HTTP/3, on ngtcp2.

It is not there by default. ``-DC_ARES_ASYNC_ANYHTTP=ON`` fetches and builds anyhttp, at the
commit :src:`CMakeLists.txt` pins -- and with it Capy and Boost.HTTP, which anyhttp's HTTP/1.1 is
built on, at pins of anyhttp's -- for the runtime that ``C_ARES_ASYNC_API`` chooses, and makes the
target ``c_ares_async::anyhttp``: ``c_ares_async::asio`` or ``c_ares_async::corosio``, with anyhttp
and ``C_ARES_ASYNC_ANYHTTP=1``. That brings :doc:`dns-over-https` along as well, with anyhttp's
HTTP client. A resolver from a translation unit compiled with that takes
:cpp:member:`tls_options::quic`:

.. code-block:: cpp

   c_ares_async::resolver resolver(executor, {.enabled = true,
                                              .quic = true,
                                              .hostname = "dns.adguard-dns.com"});
   resolver.set_servers("94.140.14.14");

The rest is what it is for DNS over TLS: the servers move to port 853 where they name none,
:cpp:member:`tls_options::hostname` is what the certificate is checked against and what goes into
SNI, and :cpp:member:`tls_options::verify` can turn the check off, for testing. Without
``C_ARES_ASYNC_ANYHTTP``, :cpp:member:`tls_options::quic` makes the constructor throw
:cpp:type:`system_error` with ``errc::operation_not_supported`` -- it does not fall back to DNS
over TLS, or to anything else.

How it is done
--------------

DNS over TLS works because c-ares does DNS over TCP and the transport puts TLS under the byte
stream, see :doc:`dns-over-tls`. QUIC is no byte stream to put under anything, but what goes on
its streams is what DNS over TCP sends, one message at a time. So c-ares does DNS over TCP here as
well -- ``ARES_FLAG_USEVC | ARES_FLAG_STAYOPEN`` -- and the transport goes in where the one for
TLS does, in the socket functions. :src:`include/c_ares_async/detail/anyhttp_transport.hpp` has a
bridge for every socket that c-ares takes for a TCP connection -- the c-ares side of it in
:src:`include/c_ares_async/detail/anyhttp_bridge.hpp`, which DNS over HTTPS has as well, and the
QUIC side in :src:`include/c_ares_async/detail/doq_bridge.hpp`:

.. code-block:: text

   c-ares --[len][query]-->  asendto()   --> bridge --> QUIC stream: ID 0, FIN     --> server
   c-ares <--[len][answer]-- arecvfrom() <-- bridge <-- QUIC stream: ID put back   <-- server

* ``aconnect()`` comes back at once, and starts the QUIC connection to the address c-ares has for
  the server. The queries that c-ares sends in the meantime wait in the bridge until it is there.
* ``abind()`` comes before that where the resolver has a local address of the socket's family,
  see :cpp:func:`resolver::set_local_address`. The bridge keeps it, and anyhttp binds the UDP
  socket of the connection to it. An address that is not one of the host's fails the connection
  then, not ``abind()`` as it would for TCP, and c-ares fails the lookup just the same.
* ``asendto()`` takes all that c-ares sends, and cuts it into its messages. Each goes out on a
  stream of its own, with a message ID of 0 and the end of the stream after it (section 4.2.1).
  The answer gets the ID back that c-ares chose, and waits in the bridge for ``arecvfrom()``,
  which hands it to c-ares as a TCP connection would.
* At the limit of streams the server allows, a query waits for the server to allow another, which
  it does a round trip after the answer on one of them is in: anyhttp's ``open_stream()`` waits
  for that, and serves the queries in the order they came.
* A connection that cannot be made, or ends -- the server closing it, the idle timeout -- is the
  end of the stream for c-ares, once it has read what is there. It fails or reconnects the way it
  does for TCP. Waiting for the connection to end is waiting for a stream from the server, which a DoQ
  server does not open (section 4.2): it comes back when the connection is gone, and one that does
  come back with a stream is a protocol error.
* An answer that is not one message on its stream is a protocol error of the connection
  (section 4.3.3), which is closed with ``DOQ_PROTOCOL_ERROR``. Streams that are given up are reset
  with ``DOQ_REQUEST_CANCELLED``, and a connection that c-ares closes is closed with
  ``DOQ_NO_ERROR``.

What c-ares gets from ``asocket()`` is no socket at all. Where TLS has a TCP socket under it,
whose readiness is that of the TLS session as well, QUIC has a UDP socket, retransmissions and
timers -- all of them anyhttp's -- and nothing whose readiness would say that an answer is there.
So the bridge says it itself: for a socket of the transport's, the resolver waits on a descriptor
of the transport's in place of the event loop's. It can be read once there is an answer, or once
the connection is over, and written at any time, as ``asendto()`` takes everything; and like the
event loop's, it completes a wait from the executor's queue, never from inside the call. The
descriptor that c-ares gets, of ``/dev/null``, reserves a number that no socket of the system's can
have while it is c-ares', and nothing is ever done on it.

To c-ares, all of this is a TCP connection: its timeouts and retries are the ones it has for TCP.
Unlike DNS over TLS, there is no handshake for the resolver to drive -- only the readiness to take
from the bridge.

The bridge outlives the resolver where it has to: a connection that is still being made, or a
stream that has not been answered, end on the executor after the resolver has gone. It keeps
itself alive for that, and does not touch the resolver, or its logger, from the moment c-ares has
closed the socket.

The bridge is written once, for both runtimes. What it does on the runtime goes through anyhttp's
runtime layer, which it is only ever compiled with: ``launch()`` to start its coroutines,
``run_later()`` to complete the resolver's waits, and ``when_either()`` with an ``Event`` to give up
a connection that is still being made when c-ares closes the socket.

On Corosio
----------

anyhttp is built for one runtime, and here that is the one of the build:
``-DC_ARES_ASYNC_API=COROSIO -DC_ARES_ASYNC_ANYHTTP=ON`` builds it for Corosio, and makes
``c_ares_async::anyhttp`` from ``c_ares_async::corosio``. The ``anyhttp-corosio`` preset does that,
in ``build-anyhttp-corosio/``:

.. code-block:: bash

   cmake --preset anyhttp-corosio
   cmake --build --preset anyhttp-corosio
   ctest --preset anyhttp-corosio

* **OpenSSL, 3.5 or later.** anyhttp's Corosio build has Corosio's ``openssl_stream``, which uses an
  API that AWS-LC does not have, and ngtcp2 needs the QUIC API of OpenSSL 3.5. The configure stops
  where ``C_ARES_ASYNC_TLS`` is ``AWS-LC``.
* **Capy and Corosio at anyhttp's pins.** This library declares them before anyhttp does, so
  anyhttp is built on the ones :src:`CMakeLists.txt` pins -- which are the commits that anyhttp
  pins itself, at the commit it is pinned at. Moving one pin means moving the others with it.
* **An anyhttp of the program's own.** Where ``anyhttp::anyhttp`` is there already when this
  library is configured, it is used as it is, with the Capy and Corosio it was built on. It has to
  be built for the runtime of ``C_ARES_ASYNC_API``: the configure says so where ``ANYHTTP_API``
  tells, and :src:`include/c_ares_async/detail/anyhttp_transport.hpp` stops the compile where it
  does not.
* **Each coroutine of the bridge yields first.** ``launch()`` starts a coroutine from the
  executor's queue on Asio, as ``co_spawn()`` does, and right away on Corosio, where the caller is
  on the executor already. The connection is started from c-ares' ``aconnect()``, and the stream
  of a query from its ``asendto()``, neither of which is where they should run.
* **No Asio.** ``c_ares_async::anyhttp`` brings none of it along, as ``c_ares_async::corosio`` does
  not: anyhttp's public headers have nothing of Asio on Corosio, and the bridges include nothing
  else of anyhttp's -- ``runtime.hpp`` and ``quic.hpp``, and ``client.hpp`` and ``session.hpp``
  for DNS over HTTPS. The address that c-ares binds a socket to is made into an endpoint the way the
  event loop makes every other one, not with anyhttp's ``io::from_sockaddr()``, which is in
  ``anyhttp/net.hpp`` -- along with Asio's buffers, which anyhttp keeps for its own protocol code.
  So a program that names nothing of Asio compiles nothing of it. The ``headers.self_contained``
  test of a COROSIO build compiles each header on its own, and stops at the one that brings Asio
  along.

In the tests, anyhttp's server for DoQ runs on the resolver's ``corosio::io_context``, and the rest
of the fake DNS server on Asio's, as for the other tests of a COROSIO build: both are stepped from
the thread the test runs on.

What anyhttp needed
-------------------

anyhttp's ``quic::connect()`` used to resolve the host it was given and check the certificate
against that same host, which is the wrong way round for a resolver: c-ares has the server's
address and nothing else, and the name comes from :cpp:member:`tls_options::hostname`. Nor could the
check be turned off. `anyhttp#50 <https://github.com/pgit/anyhttp/pull/50>`__ added both:

* ``quic::connect(executor, endpoints, host, config)``, to the endpoints given, with the
  certificate checked for ``host`` -- or the chain alone, and no SNI, for an empty one;
* ``quic::Config::tls_verify``.

Nor did ``open_stream()`` wait at the limit of streams the server allows: it failed with
``would_block``, and the bridge could only try again once one of its own streams was through --
which is a round trip before the server allows another, so the queries that waited went out once
c-ares had given up on them and sent them again (`anyhttp#53
<https://github.com/pgit/anyhttp/issues/53>`__). `anyhttp#55
<https://github.com/pgit/anyhttp/pull/55>`__ has it wait for the server to allow another stream, and
serve those that wait in the order they started; what it used to be is ``try_open_stream()`` now.

Nor could the client be told where to send from, which c-ares has an address for, of each family
(`anyhttp#54 <https://github.com/pgit/anyhttp/issues/54>`__). `anyhttp#56
<https://github.com/pgit/anyhttp/pull/56>`__ added ``quic::Config::local_endpoint``, and
``quic::Config::device`` for an interface to send through.

The pin is at the merge of `anyhttp#59 <https://github.com/pgit/anyhttp/pull/59>`__, for DNS over
HTTPS, see :doc:`dns-over-https`. It comes after anyhttp#56, anyhttp#55 and `anyhttp#51
<https://github.com/pgit/anyhttp/pull/51>`__: that one moved anyhttp's Capy and Corosio to the
commits this library pins.

What is not there
-----------------

* **Installing.** ``c_ares_async::anyhttp`` is not installed, as anyhttp is built as part of this
  build and not installed with it.
* **One program, one choice.** The resolver is the same object with and without
  ``C_ARES_ASYNC_ANYHTTP``, but its constructor is not: the translation units of a program have to
  agree on the macro, which is what linking ``c_ares_async::anyhttp`` wherever it uses a resolver
  does.
* **Padding.** Section 5.4 recommends padding the messages (RFC 8467), and c-ares does not pad.
* **0-RTT and session resumption,** which anyhttp's client does not do: every connection is a full
  handshake, which ``ARES_FLAG_STAYOPEN`` keeps to one per server, until the connection idles out
  -- after 30 seconds, or what the server says.
* **An interface to send through,** which c-ares binds its sockets to with ``SO_BINDTODEVICE``
  (``ares_set_local_dev()``). The resolver takes none, on any transport, and the bridge turns down
  the socket option, as it does all of them. ``quic::Config::device`` is there for when it does.

anyhttp logs to spdlog's default logger, at the level of information for every connection it
makes: a program that does not want that sets the level, as :src:`test/fake_dns_server.cpp` does.
The resolver's own messages go to :cpp:func:`resolver::set_logger`, and a certificate that is not
taken is, as for TLS, a connection that failed:

.. code-block:: text

   error fd=8 DoQ connection to 94.140.14.14:853 failed: Connection refused
