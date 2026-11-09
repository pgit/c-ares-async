// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The runtime layer for Corosio: coroutines only, capy::task, capy::any_executor, and
// std::stop_token where Asio has cancellation slots. See c_ares_async/runtime.hpp for what this
// is; asio/runtime.hpp defines the same names in the same order and has their documentation, and
// what is said here is what differs.
//
#pragma once

#include <boost/capy/continuation.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/ex/await_context.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/capy/results.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ip_address.hpp>

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <stop_token>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace c_ares_async::inline corosio
{

/** A coroutine, as the runtime runs it: `boost::capy::task<T>`. */
template <typename T = void>
using Task = boost::capy::task<T>;

/** The type of the executor a @ref resolver runs on:
    `boost::capy::any_executor`.
 */
using Executor = boost::capy::any_executor;

/** An IP address, as the runtime has it: `boost::corosio::ip_address`. */
using IpAddress = boost::corosio::ip_address;

/** An address and a port, as the runtime has it:
    `boost::corosio::endpoint`, which serves TCP and UDP alike. What a
    lookup comes back with.
 */
using TcpEndpoint = boost::corosio::endpoint;

/** What went wrong, as the runtime reports it: `std::error_code`. */
using error_code = std::error_code;

/** What an operation yields: `boost::capy::results`, which is a
    `std::tuple` with the @ref error_code first.
 */
using boost::capy::results;

/** An operation, as the coroutine that yields its @ref results. */
template <typename... Ts>
using IoTask = Task<results<Ts...>>;

/** The base of an error category of the runtime's own:
    `std::error_category`, which @ref ares_category derives from.
 */
using error_category = std::error_category;

namespace detail
{

//
// std::error_category has no id, it compares by address only. What makes the copies of a category
// that the headers define one object is ares_category() being visible outside of its shared
// library, so that the dynamic linker binds them all to the same.
//
template <unsigned long long>
class error_category_base : public error_category
{
protected:
   constexpr error_category_base() noexcept = default;
};

} // namespace detail

// --------------------------------------------------------------------------------------------------

/** Portable error conditions: `std::errc`. */
using errc = std::errc;

/** The error codes of the API contract that the two runtimes spell
    differently.
 */
namespace errors
{

/** An operation that was cancelled, or whose resolver went away:
    `boost::capy::error::canceled`, which compares equal to
    `errc::operation_canceled`.
 */
inline const error_code canceled = boost::capy::error::canceled;

} // namespace errors

/** What the runtime throws an @ref error_code as: `std::system_error`. */
using system_error = std::system_error;

namespace detail
{

inline error_code last_error() noexcept { return {errno, std::system_category()}; }

// --------------------------------------------------------------------------------------------------

//
// Where a parked operation leaves its result, and how its caller gets resumed. It is part of the
// awaitable that initiate() returns, which lives in the caller's coroutine frame for as long as
// the operation is in progress.
//
template <typename... Args>
struct CompletionState
{
   boost::capy::continuation continuation;
   const boost::capy::await_context* env = nullptr;
   std::optional<results<Args...>> result;
   std::optional<std::stop_callback<std::function<void()>>> on_stop;
   bool starting = false; // the operation is being started, see Initiation::await_suspend()

   //
   // Never resumes the caller inline: it is posted to the caller's executor, which is what Asio's
   // post() does for the handler there -- unless the operation completes while it is being
   // started, in which case the caller resumes once that returns.
   //
   void complete(error_code ec, Args... args)
   {
      result.emplace(ec, std::move(args)...);
      on_stop.reset();
      if (!starting)
         env->executor.post(continuation);
   }
};

template <typename Signature>
struct CompletionTraits;

template <typename... Args>
struct CompletionTraits<void(error_code, Args...)>
{
   using State = CompletionState<Args...>;
   using Result = results<Args...>;
};

template <typename Signature>
class Completion;

//
// An operation that has been started and not completed yet. Here it refers to the state of the
// awaitable its caller waits on, and invoking it posts the caller's resumption.
//
// One that is destroyed without having been invoked completes as cancelled, so that nobody waits
// for it forever -- which is what a c-ares request whose channel is destroyed comes to.
//
template <typename... Args>
class Completion<void(error_code, Args...)>
{
public:
   using State = CompletionState<Args...>;

   Completion() noexcept = default;
   Completion(std::nullptr_t) noexcept {} // NOLINT(*-explicit-constructor)
   explicit Completion(State* state) noexcept : state_(state) {}
   Completion(Completion&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}
   Completion& operator=(Completion&& other) noexcept
   {
      if (this != &other)
      {
         abandon();
         state_ = std::exchange(other.state_, nullptr);
      }
      return *this;
   }
   Completion& operator=(std::nullptr_t) noexcept
   {
      abandon();
      return *this;
   }
   ~Completion() { abandon(); }

   explicit operator bool() const noexcept { return state_ != nullptr; }

   void operator()(error_code ec, Args... args) &&
   {
      std::exchange(state_, nullptr)->complete(ec, std::move(args)...);
   }

   State* state() const noexcept { return state_; }

private:
   void abandon() noexcept
   {
      if (state_)
         std::exchange(state_, nullptr)->complete(errors::canceled, Args{}...);
   }

   State* state_ = nullptr;
};

//
// Invoking the completion posts by itself, so there is nothing for the executor to do here.
//
template <typename Handler, typename... Args>
inline void complete(Handler&& handler, const Executor&, Args&&... args)
{
   if (handler)
      std::move(handler)(std::forward<Args>(args)...);
}

//
// Calls 'on_cancel' when the stop token of whoever waits for the operation is triggered. A stop
// that was requested before the operation started never gets here: initiate() completes such an
// operation as cancelled right away.
//
template <typename Signature, typename F>
inline void on_cancel(Completion<Signature>& handler, F&& on_cancel)
{
   auto* state = handler.state();
   if (!state || !state->env->stop_token.stop_possible())
      return;
   state->on_stop.emplace(state->env->stop_token,
                          std::function<void()>(std::forward<F>(on_cancel)));
}

//
// Completing the operation drops the stop callback, so this is only here to be called in both
// runtimes.
//
template <typename Signature>
inline void clear_on_cancel(Completion<Signature>&)
{
}

template <typename Signature, typename Init>
class Initiation
{
   using Traits = CompletionTraits<Signature>;

public:
   explicit Initiation(Init init) : init_(std::move(init)) {}

   //
   // Capy awaits nothing it cannot move, and moving one of these is only ever done before it is
   // awaited, while its state is still empty: the stop callback in there does not move.
   //
   Initiation(Initiation&& other) noexcept(std::is_nothrow_move_constructible_v<Init>)
      : init_(std::move(other.init_))
   {
   }

   bool await_ready() const noexcept { return false; }

   std::coroutine_handle<> await_suspend(std::coroutine_handle<> h,
                                         const boost::capy::await_context* env)
   {
      state_.continuation.h = h;
      state_.continuation.owner = env->owner;
      state_.env = env;
      if (env->stop_token.stop_requested())
      {
         canceled(std::type_identity<typename Traits::Result>{});
         return h;
      }

      //
      // An operation that completes while it is being started resumes its caller right away, once
      // the initiation has returned: by then the stack is the caller's own again, with no c-ares
      // call on it. That is what Asio's awaitables do, too.
      //
      state_.starting = true;
      init_(Completion<Signature>(&state_));
      state_.starting = false;
      if (state_.result)
         return h;
      return std::noop_coroutine();
   }

   typename Traits::Result await_resume() { return std::move(*state_.result); }

private:
   template <typename... Args>
   void canceled(std::type_identity<results<Args...>>)
   {
      state_.result.emplace(errors::canceled, Args{}...);
   }

   Init init_;
   typename Traits::State state_;
};

//
// Starts an operation that completes a Completion<Signature>, and waits for it.
//
template <typename Signature, typename Init>
auto initiate(Init&& init)
{
   return Initiation<Signature, std::decay_t<Init>>(std::forward<Init>(init));
}

// --------------------------------------------------------------------------------------------------

//
// Lets whatever else is ready to run on the caller's executor run first. Capy starts a task by
// dispatching it, which runs it inline where the caller is on its executor already -- so this is
// what a task awaits to be sure it is not on the stack of whoever started it.
//
struct YieldNow
{
   boost::capy::continuation continuation;

   bool await_ready() const noexcept { return false; }
   std::coroutine_handle<> await_suspend(std::coroutine_handle<> h,
                                         const boost::capy::await_context* env)
   {
      continuation.h = h;
      continuation.owner = env->owner;
      env->executor.post(continuation);
      return std::noop_coroutine();
   }
   void await_resume() const noexcept {}
};

inline YieldNow yield_now() { return {}; }

// --------------------------------------------------------------------------------------------------

//
// A timer that calls back when it expires. Corosio has a re-armable timer in its detail namespace
// only; what it offers is a delay, so every arm() is a wait of its own, which the next arm() or
// cancel() stops.
//
// A wait whose delay has expired may still be queued behind the executor when it is stopped: it
// looks at its stop token again once it runs, so that a callback that has been replaced or
// cancelled is never called.
//
class Timer
{
public:
   explicit Timer(const Executor& executor) : executor_(executor) {}
   Timer(const Timer&) = delete;
   Timer& operator=(const Timer&) = delete;
   ~Timer() { cancel(); }

   template <typename Rep, typename Period, typename F>
   void arm(std::chrono::duration<Rep, Period> delay, F&& on_expiry)
   {
      cancel();
      stop_.emplace();
      auto wait = [](std::chrono::steady_clock::duration delay,
                     std::function<void()> on_expiry) -> Task<void> {
         auto [ec] = co_await boost::corosio::delay(delay);
         // Hoisted out of the condition: GCC cannot parse a co_await inside a member call.
         const std::stop_token token = co_await boost::capy::this_coro::stop_token;
         if (!ec && !token.stop_requested())
            on_expiry();
      };
      boost::capy::run_async(
         executor_, stop_->get_token(), []() noexcept {},
         [](const std::exception_ptr&) noexcept {})(
         wait(std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay),
              std::function<void()>(std::forward<F>(on_expiry))));
   }

   void cancel()
   {
      if (stop_)
         stop_->request_stop();
      stop_.reset();
   }

private:
   Executor executor_;
   std::optional<std::stop_source> stop_;
};

} // namespace detail

} // namespace c_ares_async::inline corosio
