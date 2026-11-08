// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The I/O runtime the library is built on, and the vocabulary its interface is written in: Asio
// with completion tokens (C_ARES_ASYNC_ASIO=1), or Corosio with coroutines only
// (C_ARES_ASYNC_COROSIO=1). A build tree is one or the other, see doc/corosio.rst.
//
// This is anyhttp's runtime layer (include/anyhttp/runtime.hpp there), as far as this library
// uses it: the same names, defined the same way, in the same sections. What is not part of the
// interface is in detail:: here. asio/runtime.hpp documents each, corosio/runtime.hpp follows it:
//
//    types        Task<T>, Executor, IpAddress, TcpEndpoint, error_code, results<Ts...>,
//                 IoTask<Ts...>; error_category, which anyhttp has no need for
//    errors       errc, errors::canceled, system_error, detail::last_error()
//    completions  detail::Completion<Signature>, detail::complete(), detail::on_cancel(),
//                 detail::clear_on_cancel(), which anyhttp has no need for, and
//                 detail::initiate<Signature>()
//    scheduling   detail::yield_now()
//    engines      detail::Timer
//
// The implementation reaches the event loop itself through src/event_loop.hpp, which nothing
// public includes.
//
#pragma once

#include <c_ares_async/config.hpp>

#if C_ARES_ASYNC_ASIO == C_ARES_ASYNC_COROSIO
#error "exactly one of C_ARES_ASYNC_ASIO and C_ARES_ASYNC_COROSIO has to be 1"
#endif

#if C_ARES_ASYNC_COROSIO
#include <c_ares_async/corosio/runtime.hpp>
#else
#include <c_ares_async/asio/runtime.hpp>
#endif
