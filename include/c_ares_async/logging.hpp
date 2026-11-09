// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// Where the messages of a resolver go. It is the same on either runtime, so it is in the namespace
// itself rather than in the inline one of a runtime.
//
#pragma once

#include <functional>
#include <string_view>

namespace c_ares_async
{

/** How much a message from a @ref c_ares_async::resolver matters.

    @see log_function
 */
enum class log_level
{
   /** What is going on: queries, the sockets of c-ares, timeouts. */
   debug,

   /** What may be worth knowing: a TLS session and whom it is with. */
   info,

   /** What did not go as it should, but did go on. */
   warning,

   /** What went wrong, and why: the reason a TLS handshake failed is one
       that only ever shows here.
    */
   error
};

/** Receives the messages of a @ref resolver.

    The library does not write anywhere by itself. What it has to say goes
    to a function like this one, with the level of the message and its text,
    and is not even put together where there is none.

    @see resolver::set_logger
 */
using log_function = std::function<void(log_level, std::string_view)>;

} // namespace c_ares_async
