// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The I/O runtime the library is used on, and the vocabulary its interface is written in: Asio
// with completion tokens (C_ARES_ASYNC_ASIO=1), or Corosio with coroutines only
// (C_ARES_ASYNC_COROSIO=1). The library is headers only, so it is whoever includes them who
// decides, by defining one of the two -- or neither, which is Asio. The CMake targets
// c_ares_async::asio and c_ares_async::corosio define the one they are named after.
//
// Everything is in the inline namespace c_ares_async::asio or c_ares_async::corosio, which
// C_ARES_ASYNC_RUNTIME names. Code that is written once for both runtimes opens it as
//
//    namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME
//
// That is what keeps the definitions of the two apart where they are in the same program: they
// are compiled from the same headers, so without it, the linker would keep one of each inline
// function and use it for both. Nobody has to spell the inline namespace, c_ares_async::resolver
// is the one of the runtime of the translation unit.
//
// This is anyhttp's runtime layer (include/anyhttp/runtime.hpp there), as far as this library
// uses it: the same names, defined the same way, in the same sections. What is not part of the
// interface is in detail:: here. asio/runtime.hpp documents each, corosio/runtime.hpp follows it:
//
//    types        Task<T>, Executor, IpAddress, TcpEndpoint, error_code, results<Ts...>,
//                 IoTask<Ts...>; error_category and detail::error_category_base<Id>, which
//                 anyhttp has no need for
//    errors       errc, errors::canceled, system_error, detail::last_error()
//    completions  detail::Completion<Signature>, detail::complete(), detail::on_cancel(),
//                 detail::clear_on_cancel(), which anyhttp has no need for, and
//                 detail::initiate<Signature>()
//    scheduling   detail::yield_now()
//    engines      detail::Timer
//
// The implementation reaches the event loop itself through detail/event_loop.hpp.
//
#pragma once

#ifndef C_ARES_ASYNC_COROSIO
#define C_ARES_ASYNC_COROSIO 0
#endif

#ifndef C_ARES_ASYNC_ASIO
#if C_ARES_ASYNC_COROSIO
#define C_ARES_ASYNC_ASIO 0
#else
#define C_ARES_ASYNC_ASIO 1
#endif
#endif

#if C_ARES_ASYNC_ASIO == C_ARES_ASYNC_COROSIO
#error "exactly one of C_ARES_ASYNC_ASIO and C_ARES_ASYNC_COROSIO has to be 1"
#endif

#if C_ARES_ASYNC_COROSIO
#define C_ARES_ASYNC_RUNTIME corosio
#include <c_ares_async/corosio/runtime.hpp>
#else
#define C_ARES_ASYNC_RUNTIME asio
#include <c_ares_async/asio/runtime.hpp>
#endif
