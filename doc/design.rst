.. cpp:namespace:: c_ares_async

How the integration works
=========================

c-ares is told about nothing but its own sockets, and is driven entirely from the event loop of
the application. :src:`include/c_ares_async/impl/resolver.ipp` holds all of it:

* the channel is created with ``ARES_OPT_SOCK_STATE_CB``, so c-ares reports every socket it opens,
  closes, or changes its read/write interest on -- no c-ares event thread is involved;
* each such socket gets a non-owning ``detail::Descriptor`` (``detail/event_loop.hpp``), which waits for
  readiness on it and is detached again before c-ares closes the socket. On Asio that is a
  ``posix::stream_descriptor`` and ``async_wait()``; the other runtime is in :doc:`the Corosio page <corosio>`;
* readiness is fed back with `ares_process_fds() <https://c-ares.org/docs/ares_process_fds.html>`__.
  Since the socket state callback only fires on *changes*, the waits are re-armed after every call;
* ``ares_timeout()`` drives a ``detail::Timer``, whose expiry calls ``ares_process_fds()`` with no
  events;
* the socket functions of the channel are our own (:src:`include/c_ares_async/detail/socket_functions.hpp`),
  for one reason: they turn TCP fast open down. Fast open makes a socket writable before its
  connection is up, and the write event is what c-ares takes for the connect having finished --
  told otherwise, it writes the query a second time and drops the connection. DNS over TLS
  installs socket functions of its own, which refuse it just the same.

Queries come in two spellings, and are the same operation behind both: :cpp:func:`resolve() <resolver::resolve>` and
:cpp:func:`resolve_https() <resolver::resolve_https>`, coroutines that yield the error and the result as a tuple, which is what either
runtime has; and, on Asio, :cpp:func:`async_resolve() <resolver::async_resolve>` and :cpp:func:`async_resolve_https() <resolver::async_resolve_https>` through
``async_initiate``, which take ``use_awaitable``, ``as_tuple``, ``deferred``, callbacks and futures alike.

Completion
----------

c-ares calls back from inside ``ares_getaddrinfo()`` or ``ares_process_fds()``, and for a name it can
answer without a query -- an address, ``localhost``, an entry of ``/etc/hosts`` -- that is before the
initiating function has even returned. A completion handler must never be invoked from in there, so
it is posted: to the executor of the resolver, from where it is passed on to the executor the
handler is associated with, if it has one.

The same holds in the other direction, and c-ares does not survive it being broken: a readiness
wait must not call ``ares_process_fds()`` from inside one that is already running. It would be,
otherwise, since the waits are armed from the socket state callback, which c-ares calls from in
there. On Asio a wait only ever completes from the event loop, so the question does not come
up; the other runtime completes one whose readiness holds already right where it is started, and
posts it for that reason -- see :doc:`the Corosio page <corosio>`.

Cancellation
------------

Cancellation works, but only bluntly: c-ares has no handle for an individual ``ares_getaddrinfo()``
request, so cancelling one calls ``ares_cancel()``, which takes every pending query on the channel
with it -- the lookups of every other name in flight included. Each of them completes with
:cpp:var:`errors::canceled`, which is what ``ARES_ECANCELLED`` and ``ARES_EDESTRUCTION`` are
reported as: ``errc::operation_canceled`` on Asio and ``boost::capy::error::canceled`` on Corosio,
which both compare equal to ``errc::operation_canceled``. What does the cancelling is a
cancellation of the terminal type on Asio, through a cancellation slot, and the
``std::stop_token`` of the awaiting coroutine on Corosio. A partial or total cancellation alone
promises a state that a lookup cannot keep, and is ignored, which is what Asio asks of an
operation that does not support it.

A cancellation that arrives once a lookup has completed, with its handler still on its way, would
take the lookups that are still pending with it. So the completion takes the cancellation handler
back first, with ``detail::clear_on_cancel()``, which anyhttp's runtime layer has no need for: its
operations can each be cancelled on their own.

That puts some weight on ``ares_cancel()``, which is worth knowing about:
`GHSA-6wfj-rwm7-3542 <https://github.com/c-ares/c-ares/security/advisories/GHSA-6wfj-rwm7-3542>`__
is a use-after-free/double-free in c-ares' query-completion handling, where a query's callback
runs while the query is still linked into the channel, and one of the two documented triggers is
an application calling ``ares_cancel()``. It affects everything after 1.32.3 and is fixed in
**1.34.7** -- the devcontainer currently installs Debian's ``libcares2``, which is 1.34.5, and CMake
says so when it configures.

Lifetime
--------

Destroying a resolver is ``ares_destroy()``, which fails what is pending with ``ARES_EDESTRUCTION``
and closes the sockets. The handlers of those operations are posted like any other, so they run
after the destructor has returned.

What the resolver had waiting in the event loop for itself needs more care. Cancelling a wait does
nothing for one that has completed already and only has its callback left to run, so such a
callback may find the resolver gone. The ones for socket readiness hold a ``weak_ptr`` to their
socket, which goes with the resolver and is the first thing they look at; the timer gives every
wait of its a stop token, and a wait that finds it triggered -- by the next ``arm()``, by ``cancel()``,
or by the timer being destroyed along with the resolver -- keeps quiet.

Logging
-------

The library writes nowhere. What it has to say -- the queries it starts and how they end, the
sockets c-ares opens, what a TLS handshake negotiated or why it failed -- goes to the function
given to :cpp:func:`resolver::set_logger`, and is not even formatted where there is none.

Headers only
------------

There is nothing to build: every translation unit that includes :src:`include/c_ares_async/resolver.hpp`
compiles the resolver, for the runtime that it is on. The declarations are where they always were,
and the definitions follow them in ``impl/*.ipp``, which each header includes at its end -- the way
Asio has it -- so the headers read the same as before, and each one compiles on its own, which a
test makes sure of.

Three things come with that.

* **The runtime is chosen per translation unit.** The headers go by ``C_ARES_ASYNC_ASIO`` or
  ``C_ARES_ASYNC_COROSIO``, which the targets ``c_ares_async::asio`` and ``c_ares_async::corosio``
  define, so one program can have both. Everything is in an inline namespace of the runtime's name
  for that reason: without it, ``resolver::~resolver()`` or ``resolver::set_logger()`` would be the
  same symbol on either runtime, with a different resolver behind it, and the linker would keep
  one of them for both. :src:`test/corosio/mixed_runtimes_test.cpp` puts both into one program.
  What is the same on either runtime -- :cpp:struct:`https_record`, :cpp:struct:`tls_options`, the
  logging types -- is in ``c_ares_async`` itself, in headers of its own.
* **The category of the c-ares codes is one object, although every shared library has a copy.**
  ``ares_category()`` is an inline function with a static object in it, which a shared library
  compiled with ``-fvisibility=hidden`` would keep to itself -- and then a code it makes compares
  unequal to the same code made elsewhere. The function is declared visible, so the dynamic linker
  binds every copy to the same one, and on Asio the category has an id besides, which is what
  Boost.System compares by. :src:`test/shared_library_test.cpp` checks it across a shared library
  that is compiled that way.
* **It costs compile time.** The headers of c-ares and of the TLS library come with the resolver,
  and so does its code: with clang, about half a second more to parse a translation unit than the
  declarations alone would take, and one to three seconds more to compile one that makes a
  resolver, depending on the optimization.

The runtime layer was headers in anyhttp already. What it has in addition for being used this way
is ``detail::error_category_base``, which gives a category the id that Boost.System compares by.

Not done yet
------------

* Batching several ready descriptors into a single ``ares_process_fds()`` call -- there is one
  completion per descriptor, so each one currently triggers its own call.
* Cancelling a single operation, see above.
* Anything but Linux: the sockets are waited on as POSIX descriptors, and the DNS over TLS
  transport creates its own with ``SOCK_NONBLOCK``.
