// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The runtime layer for Asio: completion tokens, boost::asio::awaitable, any_io_executor. See
// c_ares_async/runtime.hpp for what this is, and corosio/runtime.hpp for the other runtime, which
// defines the same names in the same order.
//
#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/append.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/error_category.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <cerrno>
#include <chrono>
#include <optional>
#include <stop_token>
#include <tuple>
#include <utility>

namespace c_ares_async::inline asio
{

/** A coroutine, as the runtime runs it: `boost::asio::awaitable<T>`. */
template <typename T = void>
using Task = boost::asio::awaitable<T>;

/** The type of the executor a @ref resolver runs on:
    `boost::asio::any_io_executor`.
 */
using Executor = boost::asio::any_io_executor;

/** An IP address, as the runtime has it: `boost::asio::ip::address`. */
using IpAddress = boost::asio::ip::address;

/** An address and a port, as the runtime has it:
    `boost::asio::ip::tcp::endpoint`. What a lookup comes back with.
 */
using TcpEndpoint = boost::asio::ip::tcp::endpoint;

/** What went wrong, as the runtime reports it:
    `boost::system::error_code`.
 */
using error_code = boost::system::error_code;

/** What an operation yields: its error first, then what it has to report
    besides, which `auto [ec, endpoints] = co_await resolver.resolve(...)`
    takes apart. A `std::tuple` here, and Capy's `capy::results` with the
    runtime's @ref error_code on Corosio, which is the same type.
 */
template <typename... Ts>
using results = std::tuple<error_code, Ts...>;

/** An operation, as the coroutine that yields its @ref results --
    `IoTask<results_type>` is `Task<results<results_type>>`.
 */
template <typename... Ts>
using IoTask = Task<results<Ts...>>;

/** The base of an error category of the runtime's own:
    `boost::system::error_category`, which @ref ares_category derives
    from.
 */
using error_category = boost::system::error_category;

namespace detail
{

//
// The base of an error category of the library's own. The headers define it, so every shared
// library that uses it has a copy of its one object -- and Boost.System takes two categories for
// one where they have the same 'Id', which is a random number of 64 bits. Not in anyhttp, which is
// compiled.
//
template <unsigned long long Id>
class error_category_base : public error_category
{
protected:
   constexpr error_category_base() noexcept : error_category(Id) {}
};

} // namespace detail

// --------------------------------------------------------------------------------------------------

/** Portable error conditions: `boost::system::errc`.

    `ec == errc::operation_canceled` compares by condition, so it matches
    whichever category the code is of.
 */
namespace errc = boost::system::errc;

/** The error codes of the API contract that the two runtimes spell
    differently.
 */
namespace errors
{

/** An operation that was cancelled, or whose resolver went away:
    `errc::operation_canceled`, of the generic category.

    It compares equal to `errc::operation_canceled`, and so does
    `boost::asio::error::operation_aborted`, which is what Asio's own
    operations are cancelled with.

    @hideinitializer
 */
inline const error_code canceled = make_error_code(errc::operation_canceled);

} // namespace errors

/** What the runtime throws an @ref error_code as:
    `boost::system::system_error`.
 */
using system_error = boost::system::system_error;

namespace detail
{

//
// What errno holds, for code that calls the operating system itself.
//
inline error_code last_error() noexcept { return {errno, boost::system::system_category()}; }

// --------------------------------------------------------------------------------------------------

//
// An operation that has been started and not completed yet, waiting to be completed with a
// 'Signature' of void(error_code, ...). The resolver parks one of these per lookup, in the
// request it hands to c-ares, and completes it from the c-ares callback.
//
template <typename Signature>
using Completion = boost::asio::any_completion_handler<Signature>;

//
// Completes 'handler' from 'executor's queue, after whatever is running now has returned. That is
// how a c-ares callback completes a lookup: c-ares calls it from inside ares_getaddrinfo() or
// ares_process_fds(), and resuming the application in there would let it call back into c-ares
// from inside it.
//
// From 'executor's queue, the handler is passed on to the executor it is associated with, if it
// has one: append() keeps what the handler is associated with, where anyhttp's lambda around it
// has nothing, and so runs a handler bound to a strand outside of it.
//
template <typename Handler, typename... Args>
inline void complete(Handler&& handler, const Executor& executor, Args&&... args)
{
   boost::asio::post(
      executor, boost::asio::append(std::forward<Handler>(handler), std::forward<Args>(args)...));
}

//
// Calls 'on_cancel' when whoever started the operation that 'handler' stands for cancels it. Does
// nothing where the caller has no way of cancelling.
//
// Only a request with the terminal bit counts: that is the one kind of cancellation there is, the
// same as a stop request on Corosio, and it completes the operation right away. A request for
// partial or total alone promises a state that is not always there to keep, so it is ignored,
// which is what Asio asks of an operation that does not support a type. Asio's own coroutines
// filter the same way by default, cancel_after sends terminal and || sends all three.
//
template <typename Signature, typename F>
inline void on_cancel(Completion<Signature>& handler, F&& on_cancel)
{
   auto slot = boost::asio::get_associated_cancellation_slot(handler);
   if (slot.is_connected() && !slot.has_handler())
      slot.assign([on_cancel = std::forward<F>(on_cancel)](
                     boost::asio::cancellation_type_t type) mutable { //
         if ((type & boost::asio::cancellation_type::terminal) !=
             boost::asio::cancellation_type::none)
            on_cancel();
      });
}

//
// Takes that back, for an operation that is about to complete: a cancellation arriving afterwards
// has nothing left to cancel, and would take the lookups that are still pending with it. Not in
// anyhttp, whose operations can each be cancelled on their own.
//
template <typename Signature>
inline void clear_on_cancel(Completion<Signature>& handler)
{
   if (auto slot = boost::asio::get_associated_cancellation_slot(handler); slot.is_connected())
      slot.clear();
}

//
// Starts an operation that completes a Completion<Signature>, and waits for it, yielding the
// completion's arguments as a tuple:
//
//    auto [ec, endpoints] = co_await initiate<resolve_signature>(
//       [&](Completion<resolve_signature> handler) { start_resolve(..., std::move(handler)); });
//
// The initiating function runs when the operation is awaited, so it may capture by reference.
//
template <typename Signature, typename Init>
auto initiate(Init&& init)
{
   return boost::asio::async_initiate<const boost::asio::as_tuple_t<boost::asio::deferred_t>&,
                                      Signature>(
      [init = std::forward<Init>(init)](Completion<Signature> handler) mutable {
         init(std::move(handler));
      },
      boost::asio::as_tuple(boost::asio::deferred));
}

// --------------------------------------------------------------------------------------------------

//
// Lets whatever else is ready to run on the caller's executor run first.
//
inline auto yield_now() { return boost::asio::post(boost::asio::deferred); }

// --------------------------------------------------------------------------------------------------

//
// A timer that calls back when it expires, which is what c-ares needs: ares_timeout() says when,
// and wants ares_process_fds() called then. Arming the timer again replaces both the expiry and
// the callback, and a callback that has been replaced or cancelled is never called. Callbacks run
// on the timer's executor.
//
// Cancelling a steady_timer does nothing for a wait that has expired already and only has its
// handler left on the queue -- and by the time that one runs, the timer may have been re-armed, or
// be gone along with whoever wanted to hear from it. So every wait carries a stop token of its
// own, which arm(), cancel() and the destructor trigger, and a handler that finds it triggered
// keeps quiet.
//
class Timer
{
public:
   explicit Timer(const Executor& executor) : timer_(executor) {}
   Timer(const Timer&) = delete;
   Timer& operator=(const Timer&) = delete;
   ~Timer() { cancel(); }

   template <typename Rep, typename Period, typename F>
   void arm(std::chrono::duration<Rep, Period> delay, F&& on_expiry)
   {
      cancel();
      stop_.emplace();
      timer_.expires_after(delay);
      timer_.async_wait([on_expiry = std::forward<F>(on_expiry),
                         token = stop_->get_token()](const error_code& ec) mutable {
         if (!ec && !token.stop_requested())
            on_expiry();
      });
   }

   void cancel()
   {
      if (stop_)
         stop_->request_stop();
      stop_.reset();
      timer_.cancel();
   }

private:
   boost::asio::steady_timer timer_;
   std::optional<std::stop_source> stop_;
};

} // namespace detail

} // namespace c_ares_async::inline asio
