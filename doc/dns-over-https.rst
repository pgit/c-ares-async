.. cpp:namespace:: c_ares_async

DNS over HTTPS
==============

`RFC 8484 <https://www.rfc-editor.org/rfc/rfc8484>`__ puts DNS on HTTPS: a query is the body of a
request, ``POST`` to a URL of the server's, with ``Content-Type: application/dns-message``, and the
answer is the body of the response, of the same type. It is HTTP like any other, on TCP port 443,
and HTTP/2 is the least it recommends (section 5.2), which can have as many queries out on one
connection as the server allows. c-ares has no transport for it, nor any HTTP: the library takes
`anyhttp <https://github.com/pgit/anyhttp>`__'s client, as it takes anyhttp's QUIC for
:doc:`dns-over-quic`.

It is built where DNS over QUIC is: ``-DC_ARES_ASYNC_ANYHTTP=ON`` fetches and builds anyhttp, and
makes the target ``c_ares_async::anyhttp``, see :doc:`dns-over-quic`. A resolver from a
translation unit compiled with that takes :cpp:member:`tls_options::https`:

.. code-block:: cpp

   c_ares_async::resolver resolver(executor, {.enabled = true,
                                              .https = true,
                                              .hostname = "cloudflare-dns.com"});
   resolver.set_servers("1.1.1.1");

The servers move to port 443 where they name none, and the queries go to
``https://cloudflare-dns.com/dns-query`` -- on the address c-ares has for the server, which is the
only one anyhttp connects to: it resolves nothing. :cpp:member:`tls_options::path` is where on the
server, ``/dns-query`` unless told otherwise, which is where most servers have it.
:cpp:member:`tls_options::hostname` is what the certificate is checked against and what goes into
SNI, and :cpp:member:`tls_options::verify` can turn the check off, for testing.

ALPN offers ``h2`` and ``http/1.1``, and the session speaks what the server picks, HTTP/2 wherever
it has it. Without :cpp:member:`tls_options::http2`, ``http/1.1`` is all that ALPN offers, and the
session speaks HTTP/1.1 with any server -- one query at a time, see below. That is for a server
whose HTTP/2 is not to be relied on, or for seeing what its HTTP/1.1 does; ``--doh-http1`` is the
example's for it.

Without ``C_ARES_ASYNC_ANYHTTP``, :cpp:member:`tls_options::https` makes the constructor throw
:cpp:type:`system_error` with ``errc::operation_not_supported``, and with
:cpp:member:`tls_options::quic` as well, with ``errc::invalid_argument``: it is the one or the
other, and never anything else in its place.

Whose certificate
-----------------

The URL has to have a host, and where :cpp:member:`tls_options::hostname` has none, it is the
server's address: ``https://1.1.1.1/dns-query``. So the certificate is checked against that
address, which takes a certificate issued for it -- some public servers have those, most do not.
That is where DNS over HTTPS differs from DNS over TLS and DNS over QUIC, which check the chain
alone without a name. anyhttp's client has no way of checking a chain without a host.

How it is done
--------------

Much as DNS over QUIC is, see :doc:`dns-over-quic`. c-ares does DNS over TCP --
``ARES_FLAG_USEVC | ARES_FLAG_STAYOPEN`` -- and the transport goes in where the one for TLS does,
in the socket functions. :src:`include/c_ares_async/detail/anyhttp_transport.hpp` is the transport
of both: a socket of its own for every TCP connection that c-ares takes, each with a bridge to
anyhttp. :src:`include/c_ares_async/detail/anyhttp_bridge.hpp` is the c-ares side of a bridge,
which is the same for both, and :src:`include/c_ares_async/detail/doh_bridge.hpp` the one for
HTTPS:

.. code-block:: text

   c-ares --[len][query]-->  asendto()   --> bridge --> POST, ID 0          --> server
   c-ares <--[len][answer]-- arecvfrom() <-- bridge <-- 200, ID put back   <-- server

* ``aconnect()`` comes back at once, and starts the session with the server, offering HTTP/2 and
  HTTP/1.1 by ALPN, or HTTP/1.1 alone. The queries that c-ares sends in the meantime wait in the bridge until it is
  there.
* ``abind()`` comes before that where the resolver has a local address of the socket's family, see
  :cpp:func:`resolver::set_local_address`, and anyhttp binds its TCP socket to it then.
* ``asendto()`` takes all that c-ares sends, and cuts it into its messages. Each one goes out as
  the body of a ``POST`` of its own, without the two bytes of length that DNS over TCP has in front
  of it, and with a message ID of 0, which a cache can take for the same question each time
  (section 4.1). The answer is the body of the response, which gets the ID back that c-ares chose,
  and its two bytes of length, and waits in the bridge for ``arecvfrom()``.
* One coroutine submits the requests, in the order the queries came. Where the session has as many
  out as the server allows -- ``SETTINGS_MAX_CONCURRENT_STREAMS`` over HTTP/2 -- anyhttp says so
  with ``would_block`` rather than waiting, and the next query waits in the bridge for an answer to
  come in. Over HTTP/1.1 it is one at a time, the answer included.
* Until the server's ``SETTINGS`` are in, the session does not know that limit, and sends what it
  has. The server refuses the streams over it then, which it may (RFC 9113, sections 5.1.2 and
  8.7), and their queries go out again, first in line. anyhttp does not say why a stream was reset,
  so that goes for any stream that ends before there is a response, on a session that goes on --
  but only once for each query, so that a server that resets every stream is not asked for ever.
* A response that is not ``2xx``, or whose body is not a DNS message, says that this is no DoH
  server, or none at that URL -- a redirect included, which is not followed. Either is the end of the connection for c-ares, as are a session
  that cannot be made and one that cannot take a request any more. It fails or reconnects the way
  it does for TCP, and sends again what it has not had an answer for -- rather than having every
  query wait for its time to be up.

There is nothing in anyhttp's session to wait on for its end, as there is a stream from the server
for DNS over QUIC. So c-ares learns about a session that has ended -- the server closing it after
a while, say -- from the next query, which the session cannot take: that query goes out again,
on a connection made for it.

What c-ares gets from ``asocket()`` is no socket, as for DNS over QUIC, and for the same reason:
the socket is anyhttp's, under a TLS session and HTTP/2, and its readiness says nothing about an
answer being there. The bridge says that itself, with the descriptor of the transport's that the
resolver waits on.

The bridge outlives the resolver where it has to: a session that is still being made, or requests
that have not been answered, end on the executor after the resolver has gone. A session that is
there ends the orderly way, with a ``GOAWAY`` and a ``close_notify``, which takes a coroutine that
keeps the bridge -- two seconds at most for each of anyhttp's waits.

What anyhttp needed
-------------------

anyhttp's client could not be told to take any certificate, nor where to connect from.
`anyhttp#59 <https://github.com/pgit/anyhttp/pull/59>`__ added ``client::Config::tls_verify`` and
``client::Config::local_endpoint``, as ``quic::Config`` has them, and `anyhttp#61
<https://github.com/pgit/anyhttp/pull/61>`__ made the endpoint ``client::Config::local_address`` and
``client::Config::local_port``. What it already had is ``client::Config::endpoints``, to connect to
the address c-ares has while the URL says the name.

Nor could anyhttp's server be told to speak HTTP/1.1 only, over TLS: it took HTTP/2 wherever the
client offered it, and the bridge always does. `anyhttp#62
<https://github.com/pgit/anyhttp/pull/62>`__ added ``server::Config::tls_protocols``, with which
the tests run against a server of each kind -- the one that speaks both, and the one that has no
HTTP/2, which the bridge falls back to HTTP/1.1 for.

What is not there
-----------------

* **GET.** Section 4.1 has a query go out as ``GET`` as well, with the message in the URL, which
  an HTTP cache can keep and give to the next client that asks the same. Every server has to take
  ``POST``, and that is what the queries go out as here.
* **HTTP/3**, which anyhttp's client has. A server says so with ``Alt-Svc``, or in its HTTPS
  record, and a resolver is the wrong place to follow either: it is told where its servers are.
* **Padding** (section 8.1, RFC 8467), as for DNS over QUIC.
* **The cache headers.** Section 5.1 has a response say how long it may be cached, which c-ares
  takes from the TTLs instead.
* **Installing,** as for DNS over QUIC: ``c_ares_async::anyhttp`` is not installed.

The messages are what they are for DNS over QUIC, with ``DoH`` in place of ``DoQ``. A certificate
that is not taken is a connection that failed, here for ``--tls-hostname dns.google`` on 1.1.1.1,
with AWS-LC:

.. code-block:: text

   error fd=8 DoH connection to 1.1.1.1:443 failed: CERTIFICATE_VERIFY_FAILED (SSL routines, OPENSSL_internal)

A server that does not answer queries at the URL is given up on, here for ``--doh-path
/nothing-here``, which Cloudflare redirects:

.. code-block:: text

   error fd=8 DoH the server answers a query with status 301, giving up on 104.16.248.249:443

A session that the server has ended is no error, as for DNS over QUIC. It shows at the level of
debugging, as ``connection over``, when the next query runs into it.
