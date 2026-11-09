.. cpp:namespace:: c_ares_async

The Corosio runtime, next to Asio
=================================

One set of headers that serves either runtime: Asio with completion tokens, or `Corosio
<https://github.com/cppalliance/corosio>`__ on `Capy <https://github.com/cppalliance/capy>`__, with a
coroutine-only API. Which one is up to the translation unit that includes them, through the target
it links:

.. code-block:: cmake

   target_link_libraries(app PRIVATE c_ares_async::corosio)   # or c_ares_async::asio

The target defines ``C_ARES_ASYNC_COROSIO=1`` or ``C_ARES_ASYNC_ASIO=1``, and the headers go by
that; with neither, they are for Asio. Both can be in one program, as everything is in the inline
namespace ``c_ares_async::asio`` or ``c_ares_async::corosio``: the definitions are compiled from the
same headers, and without it the linker would keep one of each and use it for both. Nobody has to
spell the inline namespace -- ``c_ares_async::resolver`` is the one of the runtime at hand.

This repository's own tests and examples are built on one runtime per build tree, which is what
``C_ARES_ASYNC_API`` chooses:

.. code-block:: sh

   cmake --preset corosio          # -DC_ARES_ASYNC_API=COROSIO, in build-corosio/
   cmake --build --preset corosio
   ctest --preset corosio

Capy and Corosio are fetched at the commits :src:`CMakeLists.txt` pins and built as part of the
build, unless whoever brought c-ares-async in has made them available already. They are not
installed along with the headers: a program that uses the installed package on Corosio brings
them itself, see :ref:`Constraints <corosio-constraints>`.

.. _corosio-runtimes:

What the two runtimes are
-------------------------

.. list-table::
   :header-rows: 1

   * - 
     - ASIO
     - COROSIO
   * - Operations
     - :cpp:func:`async_resolve() <resolver::async_resolve>`, :cpp:func:`async_resolve_https() <resolver::async_resolve_https>`, any completion token, plus the coroutine spelling
     - :cpp:func:`resolve() <resolver::resolve>`, :cpp:func:`resolve_https() <resolver::resolve_https>` -- coroutines, and nothing else
   * - :cpp:type:`Task\<T> <Task>`
     - ``boost::asio::awaitable<T>``
     - ``boost::capy::task<T>``
   * - :cpp:type:`Executor`
     - ``boost::asio::any_io_executor``
     - ``boost::capy::any_executor``
   * - :cpp:type:`TcpEndpoint`
     - ``boost::asio::ip::tcp::endpoint``
     - ``boost::corosio::endpoint``
   * - :cpp:type:`error_code`
     - ``boost::system::error_code``
     - ``std::error_code``
   * - :cpp:type:`results\<Ts...> <results>`
     - ``std::tuple<error_code, Ts...>``
     - ``boost::capy::results<Ts...>``
   * - :cpp:type:`IoTask\<Ts...> <IoTask>`
     - ``boost::asio::awaitable<results<Ts...>>``
     - ``boost::capy::task<results<Ts...>>``
   * - :cpp:var:`errors::canceled`
     - ``errc::operation_canceled``, of the generic category
     - ``boost::capy::error::canceled``
   * - Cancelling a lookup
     - a cancellation of the terminal type, through a cancellation slot; ``cancel_after()``, with a timer of its own
     - the ``std::stop_token`` of the awaiting coroutine, ``corosio::timeout()``

Both have the same coroutine spelling, which yields the error and the result as a tuple:

.. code-block:: cpp

   auto [ec, endpoints] = co_await resolver.resolve("example.com", "443");

That is what the shared tests are written in. It is an :cpp:type:`IoTask` on both: on Asio,
:cpp:func:`async_resolve() <resolver::async_resolve>` with ``as_tuple(use_awaitable)`` behind it;
on Corosio a ``capy::task`` of ``capy::results<T>``, which *is* ``std::tuple<std::error_code, T>``
-- so a lookup goes into ``corosio::timeout()`` and ``capy::when_all()`` as it is, with nothing in
between. Either way, :cpp:var:`errors::canceled` compares equal to ``errc::operation_canceled``, as
the runtime's own operations do when they are cancelled.

Where the code is
-----------------

Only three places know which runtime they are in, and they are the ones that say ``C_ARES_ASYNC_ASIO``
or ``C_ARES_ASYNC_COROSIO`` (exactly one of which is 1, see :src:`include/c_ares_async/runtime.hpp`):

.. list-table::

   * - :src:`include/c_ares_async/runtime.hpp`
     - the vocabulary of the interface, which picks ``asio/runtime.hpp`` or ``corosio/runtime.hpp``, and the name of the inline namespace, ``C_ARES_ASYNC_RUNTIME``
   * - :src:`include/c_ares_async/detail/event_loop.hpp`
     - the event loop the implementation drives: ``Descriptor`` and ``make_endpoint()``, again one file per runtime; the ``Timer`` is the runtime layer's
   * - :src:`include/c_ares_async/resolver.hpp`
     - the front end: the completion-token operations are ASIO-only

The ``resolve`` example has a small layer of the same kind for what a program needs beyond the
library -- the event loop itself, the signal handling, the workers of ``--concurrency``, the
resolver of the runtime that ``--native`` compares against:
:src:`example/resolve/runtime.hpp`, again one file per runtime, with
one ``main.cpp`` on top of either. The minimal example, which is there to be read rather than used,
has a version per runtime instead: :src:`example/asio/minimal.cpp` and
:src:`example/corosio/minimal.cpp`.

Everything else -- the lookups, the HTTPS records, the server lists, the DNS over TLS transport --
is written once, against those names. ``asio/runtime.hpp`` documents each of them and
``corosio/runtime.hpp`` has the same definitions in the same order, so the pair can be read side by
side; the same goes for ``asio/event_loop.hpp`` and ``corosio/event_loop.hpp``, and for the test
fixtures in ``test/asio/`` and ``test/corosio/``. What is written once is in the inline namespace of
whichever runtime it is compiled for, as ``namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME``.

The runtime layer is `anyhttp <https://github.com/pgit/anyhttp>`__'s, as far as this library uses
it: the same names, defined the same way, with what is not part of the interface in ``detail::``.
It differs in two places, and says so where it does. ``detail::complete()`` on Asio passes a
handler on to the executor it is associated with, where anyhttp's runs it on the one it is given;
and ``detail::clear_on_cancel()`` is not in anyhttp, whose operations can each be cancelled on
their own.

The tests follow the same rule: what is about protocol behaviour is written once, in the coroutine
spelling, and runs on either runtime. What is about one runtime only has a file of its own --
:src:`test/asio/async_test.cpp` for completion tokens, associated
executors, cancellation slots and strands, :src:`test/corosio/stop_test.cpp`
for stop tokens and ``corosio::timeout()``. A COROSIO build has both runtimes, which
:src:`test/corosio/mixed_runtimes_test.cpp` puts into one program: a lookup compiled with
``c_ares_async::asio`` next to one compiled with ``c_ares_async::corosio``.

The fake DNS server stays on Asio in both: it serves UDP, TCP and TLS, and a second
implementation of it would only be a second thing to keep right. The COROSIO fixture steps its
``asio::io_context`` and the resolver's ``corosio::io_context`` from the one thread the test runs on,
so the tests stay as deterministic as they are on Asio.

What the port found
-------------------

Three of these are bugs that were in the ASIO build all along, where they are races rather than
certainties -- Corosio answers "is this socket ready" with a ``poll()`` of its own before it parks
anything, so what Asio gets to one turn of the event loop later happens here right away.

* **c-ares must not be re-entered.** It hands its sockets over from inside ``ares_process_fds()``,
  through the socket state callback, so a wait armed from there was started -- and, for a
  readiness that held already, completed -- on the stack of that very call, which called
  ``ares_process_fds()`` again. c-ares closed its connections underneath itself and came apart in a
  use-after-free. The COROSIO ``Descriptor`` posts every wait before it starts it.
* **TCP Fast Open makes a socket writable before its connection is up.** c-ares enables it on
  every TCP connection and asks to be told when the socket becomes writable, which
  ``ares_set_socket_functions_ex()`` documents to mean "the connect has finished" -- and an event
  loop that answers "is this socket writable" cannot tell the two apart. Told so anyway, c-ares
  writes the query a second time and drops the connection, which costs a lookup every retry it
  has. :src:`include/c_ares_async/detail/socket_functions.hpp` turns fast open down, as the TLS
  transport already did.
* **SO_ERROR is consume-on-read.** Corosio's reactor reads it to report the error of a socket as
  the error of the wait, which left the TLS transport's connect check with a zero and a handshake
  started on a dead socket -- all the way to a ``SIGPIPE``. It confirms the connect with
  ``getpeername()`` now, which answers the same question and keeps answering it.
* **A socket that has failed is ready**, as far as a readiness wait is concerned: whoever owns the
  socket is the one to find the failure out, by reading or writing. Asio reports such a wait
  as ready; Corosio reports the error of the socket, and the COROSIO ``Descriptor`` maps that back.
* **A timer that has expired may still be queued** when it is re-armed or cancelled, in both
  runtimes. Each wait carries a stop token of its own, and a callback that finds it triggered
  keeps quiet -- which is also what keeps a timeout that was due when the resolver went away from
  looking for it.

.. _corosio-constraints:

Constraints
-----------

* **A context does not have to run dry.** Destroying a ``corosio::io_context`` unwinds the
  coroutines still suspended on it, which includes what a resolver cancelled on its way out: their
  frames are destroyed, and nothing is resumed or delivered. Whatever parks a coroutine therefore
  carries the owner of its chain in its ``capy::continuation``, without which this leaks
  (``build-corosio-asan/`` is where LeakSanitizer says so). The resolver still has to go before its
  context, like any I/O object of Corosio's.
* **A lookup is held by its resolver, not by the context.** So teardown cannot reach a coroutine
  that owns its resolver and is suspended in a lookup of that very resolver, directly or through
  ``corosio::timeout()``: the frame holds the resolver, which holds the frame, and it leaks. Asio
  leaks there, too. Stop such a lookup, or keep the resolver out of the frame.
* **corosio::io_context::poll() does not ask the reactor.** It runs what is ready already; a
  socket that became readable in the meantime is not noticed. Stepping such a context by hand
  takes ``run_one_for()``, see :src:`test/corosio/event_loop_fixture.hpp`.
* **std::optional<c_ares_async::resolver> does not compile.** Capy's ``any_executor`` has a
  converting constructor that is constrained on ``std::copy_constructible`` only, so asking whether
  a resolver converts to an executor -- which ``std::optional`` does, through
  ``is_trivially_move_constructible`` -- instantiates it with the resolver and fails inside. A
  ``std::unique_ptr`` works, and so does holding the resolver by value. To be filed upstream.
* **Corosio has no output operator for an endpoint.** ``endpoint.address()`` and ``endpoint.port()``
  are there to be printed; the tests teach GoogleTest the same (``PrintTo``).
* **The installed package does not bring Corosio.** Capy and Corosio are built from pinned sources
  here and not installed, and have no package of their own yet. ``find_package(c_ares_async
  COMPONENTS corosio)`` makes ``c_ares_async::corosio`` where the targets ``Boost::corosio`` and
  ``Boost::capy`` are there already, from a build of Corosio that is part of the program's, and
  fails where they are not. The CMake consumer tests of a COROSIO build do it that way, with the
  sources this build fetched.
* **Capy and Corosio are position-independent code here.** The headers may be compiled into a
  shared library, and Capy and Corosio then go in there with them; the tests make one. A build that
  brings its own Capy and Corosio has to see to that itself.

Not done yet
------------

* Threads. A resolver is for one thread at a time in either runtime, and where the ASIO build has
  a strand test (``async_test.cpp``), the COROSIO one has none: ``capy::strand`` exists and Corosio
  resumes a coroutine through the executor of its ``await_context``, so a resolver on a strand should stay
  on it, but nothing here runs it that way yet.
* Moving the pins. Both libraries are pre-release and change their API on ``develop`` without
  notice, so moving a pin is a commit of its own.
