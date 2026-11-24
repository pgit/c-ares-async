// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// Minimal integration of c-ares into an event loop.
//
// c-ares is told about nothing but its own sockets: it hands them to us through its socket state
// callback, we wait for readiness on each of them the way the runtime waits for anything, and
// c-ares is told when one of them is ready, using ares_process_fds().
//
// https://c-ares.org/docs/ares_process_fds.html
//
#pragma once

#include <c_ares_async/error.hpp>
#include <c_ares_async/https_record.hpp>
#include <c_ares_async/logging.hpp>
#include <c_ares_async/runtime.hpp>
#include <c_ares_async/tls_options.hpp>

#include <ares.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if C_ARES_ASYNC_ASIO
#include <boost/asio/async_result.hpp>
#endif

/** Name resolution with c-ares, as asynchronous operations of the runtime
    of the translation unit: Asio or Corosio.
 */
namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME
{

namespace detail
{
class tls_transport;
class quic_transport;
} // namespace detail

// --------------------------------------------------------------------------------------------------

/** Resolves host names with c-ares, as asynchronous operations of the
    runtime of the translation unit.

    A resolver has one c-ares channel and drives it from the executor it was
    given. There is no thread behind it and nothing blocks: c-ares says which
    sockets it wants watched, the resolver waits for them the way the runtime
    waits for anything, and c-ares is told when one of them is ready.

    Any number of operations may be pending at a time. c-ares keeps the
    queries apart, over UDP as well as on the one connection that TCP or TLS
    take.

    @par The two runtimes
    Both lookups come as a coroutine, @ref resolve and @ref resolve_https,
    which is the spelling that an `C_ARES_ASYNC_ASIO` and an
    `C_ARES_ASYNC_COROSIO` build have in common: an @ref IoTask, which
    yields the error and the result as a tuple. On Asio there is also
    @ref async_resolve and @ref async_resolve_https, which take any
    completion token. See `doc/corosio.rst`.

    @par Thread safety
    Distinct objects: safe. Shared objects: unsafe. A resolver also works
    on its own, when one of its sockets is ready or its timer expires, and
    does so on its executor. Where the context runs on more than one thread,
    make that executor a strand, and use the resolver from inside that
    strand only: start lookups from a coroutine on it, cancel them from
    there, configure and destroy the resolver there. On Corosio, the
    resolver takes a `capy::strand` with a constructor of its own. The
    resolver does not call `ares_library_init()` or
    `ares_library_cleanup()`, which are not thread safe. See
    `doc/programmers-guide.rst`.

    @par Cancellation
    Both operations can be cancelled -- by a cancellation of the terminal
    type on Asio, which is what `cancel_after` and `||` send, by the
    `std::stop_token` of the awaiting coroutine on Corosio -- and then
    complete with @ref errors::canceled. Partial and total cancellation
    alone are ignored. But c-ares has no handle for a single request, so
    cancelling one operation cancels every operation that is pending on
    the resolver.

    @par Lifetime
    Destroying the resolver completes what is pending with
    @ref errors::canceled as well. The completions are not called from the
    destructor but handed to their executors, so the context has to run once
    more for them to be.

    @par Example
    @code
    boost::asio::awaitable<void> lookup(std::string host)
    {
       c_ares_async::resolver resolver(co_await boost::asio::this_coro::executor);

       for (const auto& endpoint :
            co_await resolver.async_resolve(host, "443", boost::asio::use_awaitable))
          std::cout << endpoint << '\n';
    }
    @endcode

    The same on either runtime, where the error is wanted rather than an
    exception:

    @code
    c_ares_async::Task<void> lookup(c_ares_async::Executor executor, std::string host)
    {
       c_ares_async::resolver resolver(executor);

       auto [ec, endpoints] = co_await resolver.resolve(host, "443");
       for (const auto& endpoint : endpoints)
          std::cout << endpoint << '\n';
    }
    @endcode
 */
class resolver
{
public:
   /** The type of the executor that a resolver runs on. */
   using executor_type = Executor;

   /** What @ref resolve comes back with: the endpoints of the host, best
       first.
    */
   using results_type = std::vector<TcpEndpoint>;

   /** The completion signature of @ref resolve. */
   using resolve_signature = void(error_code, results_type);

   /** What @ref resolve_https comes back with: the records of the host, in
       no particular order.
    */
   using https_results_type = std::vector<https_record>;

   /** The completion signature of @ref resolve_https. */
   using https_signature = void(error_code, https_results_type);

   /** Makes a resolver that runs on `executor`.

       Which servers to ask, which domains to search and the rest of the
       configuration are those of the system, found the way c-ares finds
       them: in `/etc/resolv.conf` and what goes with it.

       With `tls.enabled`, every query goes out over DNS over TLS instead of
       UDP or TCP in the clear, and the servers are asked on port 853. With
       `tls.quic` as well, it is DNS over QUIC instead, on the same port.

       @param executor Runs everything the resolver does, and every
       completion handler that has no executor of its own.

       @param tls Whether to use DNS over TLS, and how.

       @par Exceptions
       @ref system_error where c-ares cannot be set up, with its status code
       as @ref make_ares_error makes it, and `std::runtime_error` where the
       TLS library cannot. @ref system_error with
       `errc::operation_not_supported` for `tls.quic` where the library is
       built without DNS over QUIC.
    */
   explicit resolver(executor_type executor, const tls_options& tls = {});

#if C_ARES_ASYNC_COROSIO
   //
   // Makes a resolver that runs on 'strand', as the one above does on an executor. Corosio only:
   // capy::any_executor, which is the executor_type there, takes no capy::strand, so the strand
   // goes in under another type. See doc/corosio.rst.
   //
   template <typename Ex>
   explicit resolver(boost::capy::strand<Ex> strand, const tls_options& tls = {})
      : resolver(executor_type(detail::StrandExecutor<boost::capy::strand<Ex>>{std::move(strand)}),
                 tls)
   {
   }
#endif

   /** Completes every pending operation with @ref errors::canceled and
       closes the sockets.
    */
   ~resolver();

   //
   // A resolver stays where it was made: a moved-from one would still be the 'this' that the
   // socket state callback of its channel points at. Spelled out rather than left out, so that
   // whoever tries it reads it here.
   //
   resolver(const resolver&) = delete;
   resolver& operator=(const resolver&) = delete;
   resolver(resolver&&) = delete;
   resolver& operator=(resolver&&) = delete;

   /** Returns the executor that the resolver was made with.

       @return The executor.
    */
   const executor_type& get_executor() const noexcept { return m_executor; }

   /** Replaces the servers that the resolver asks.

       The list is one that `ares_set_servers_csv()` takes: addresses
       separated by commas, each with or without a port, as in "192.0.2.1",
       "192.0.2.1:5353" or "[2001:db8::1]:5353". An address without a port is
       asked on port 53, or on port 853 over TLS.

       A server has to be given by its address. A resolver that is to ask a
       server it only has the name of needs that name resolved first, by
       another resolver.

       @param servers The list. An empty one changes nothing.

       @return An error if the list cannot be made sense of, in which case
       the resolver keeps the servers it had.
    */
   error_code set_servers(std::string_view servers);

   /** Sends the queries from `address`.

       There is an address to send from for each family, as there is in
       c-ares, which takes them with `ares_set_local_ip4()` and
       `ares_set_local_ip6()`: this sets the one of the family of `address`,
       and the servers of that family are asked from there. Those of the
       other family are not affected. The unspecified address, "0.0.0.0" or
       "::", has the system choose again, by its routes, which is how a
       resolver starts out.

       It is the same over every transport. A socket of c-ares' is bound to
       the address before it connects, and for DNS over QUIC the UDP socket
       of the connection is.

       It holds for the sockets that are made from here on. A connection
       that TCP, TLS or QUIC have up already goes on sending from where it
       did, so this is for before the first lookup, as @ref set_servers is.

       @par Errors
       There are none here. A socket cannot be bound to an address that is
       not one of the host's, and the lookups fail then, with
       `ARES_ECONNREFUSED`, as for a server that cannot be reached.

       @param address The address, of either family.
    */
   void set_local_address(const IpAddress& address);

   /** Has `logger` called with every message from here on.

       The messages come from inside the operations of the resolver, so
       `logger` is called on a thread that runs the executor. It should come
       back quickly, and must not destroy the resolver.

       @param logger Receives the messages. An empty function turns them off
       again, which is how a resolver starts out.
    */
   void set_logger(log_function logger);

   /** Resolves a host name to the endpoints of a service.

       Looks up the addresses of `host`, of both families, the way
       `ares_getaddrinfo()` does for a stream socket: `/etc/hosts` and the
       search domains are taken into account, and the endpoints come sorted,
       the one to try first in front. An address in place of the name is
       taken as it is, without a query.

       This is the coroutine spelling, which both runtimes have: it yields
       the error and the endpoints as a tuple, the way the completion token
       `as_tuple(use_awaitable)` does on Asio.

       @par Errors
       Any of the c-ares status codes, as @ref make_ares_error makes them --
       `ARES_ENOTFOUND` for a name that does not exist and `ARES_ETIMEOUT`
       for servers that do not answer among them -- or @ref errors::canceled.

       @param host The name to resolve, or an address.

       @param service The port of every endpoint, by its number or by the
       name it has in `/etc/services`. Empty for port 0, which is also what a
       name that is not in there comes to -- that is no error.

       @return The error and the endpoints.
    */
   IoTask<results_type> resolve(std::string host, std::string service);

   /** Resolves the HTTPS records (RFC 9460) of a host name.

       Asks for the records of type HTTPS that `host` has, search domains and
       all, the way @ref resolve asks for addresses. It is a question of its
       own, so the two can be asked at the same time.

       @par Errors
       As for @ref resolve. A name that has no such record completes with
       `ARES_ENODATA`, which is the common case rather than a problem.

       @param host The name to resolve.

       @return The error and the records.
    */
   IoTask<https_results_type> resolve_https(std::string host);

#if C_ARES_ASYNC_ASIO

   /** A completion handler of @ref async_resolve, whatever its type. */
   using resolve_handler = detail::Completion<resolve_signature>;

   /** A completion handler of @ref async_resolve_https, whatever its type. */
   using https_handler = detail::Completion<https_signature>;

   /** Resolves a host name to the endpoints of a service, with a completion
       token.

       The same lookup as @ref resolve, as an Asio asynchronous operation.
       Asio only.

       @par Completion signature
       @code
       void(boost::system::error_code, results_type)
       @endcode

       @par Errors
       As for @ref resolve.

       @param host The name to resolve, or an address.

       @param service The port of every endpoint, as @ref resolve takes it.

       @param token The completion token: a callback,
       `boost::asio::use_awaitable`, `boost::asio::deferred` or any other.
       The handler is never called from inside this function.

       @return Whatever the completion token makes of the operation.
    */
   template <boost::asio::completion_token_for<resolve_signature> CompletionToken>
   auto async_resolve(std::string_view host, std::string_view service, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, resolve_signature>(
         [this](resolve_handler handler, std::string host, std::string service) {
            start_resolve(std::move(host), std::move(service), std::move(handler));
         },
         token, std::string(host), std::string(service));
   }

   /** Resolves the HTTPS records (RFC 9460) of a host name, with a
       completion token.

       The same lookup as @ref resolve_https, as an Asio asynchronous
       operation. Asio only.

       @par Completion signature
       @code
       void(boost::system::error_code, https_results_type)
       @endcode

       @par Errors
       As for @ref resolve_https.

       @param host The name to resolve.

       @param token The completion token: a callback,
       `boost::asio::use_awaitable`, `boost::asio::deferred` or any other.
       The handler is never called from inside this function.

       @return Whatever the completion token makes of the operation.
    */
   template <boost::asio::completion_token_for<https_signature> CompletionToken>
   auto async_resolve_https(std::string_view host, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, https_signature>(
         [this](https_handler handler, std::string host) {
            start_resolve_https(std::move(host), std::move(handler));
         },
         token, std::string(host));
   }

#endif // C_ARES_ASYNC_ASIO

private:
   struct socket_watch;

   void start_resolve(std::string host, std::string service,
                      detail::Completion<resolve_signature> handler);
   void start_resolve_https(std::string host, detail::Completion<https_signature> handler);

   static void socket_state_callback(void* data, ares_socket_t fd, int readable, int writable);
   void on_socket_state(ares_socket_t fd, bool readable, bool writable);

   void arm(const std::shared_ptr<socket_watch>& socket);
   void on_socket_event(const std::shared_ptr<socket_watch>& socket, unsigned int event,
                        const error_code& ec);

   void update_timeout();

   executor_type m_executor;
   std::unique_ptr<detail::Timer> m_timer;
   ares_channel_t* m_channel = nullptr;
   log_function m_logger; // declared ahead of the transport, which keeps a reference to it
   std::unique_ptr<detail::tls_transport> m_tls; // null unless DNS over TLS was asked for

   //
   // Null unless DNS over QUIC was asked for. A shared_ptr, as that is what can be destroyed where
   // the type is incomplete: it is defined where C_ARES_ASYNC_DOQ is 1, and not otherwise, and the
   // resolver is the same object either way.
   //
   std::shared_ptr<detail::quic_transport> m_quic;
   std::map<ares_socket_t, std::shared_ptr<socket_watch>> m_sockets;
};

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME

#include <c_ares_async/impl/resolver.ipp>
