// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <c_ares_async/runtime.hpp>

namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME
{

/** The error category of the c-ares status codes.

    A lookup that fails says why with one of the `ARES_E...` status codes of
    `<ares.h>`, and this is the category they travel in as an
    @ref error_code. The messages are those of `ares_strerror()`.

    It is one category wherever it is used, shared libraries included,
    although the headers define it: on Asio it has an id, which
    Boost.System compares categories by, and the function is visible
    outside of the library it is compiled into, so that the dynamic linker
    makes one of its copies.

    @return The one object of the category, whose name is "c-ares".

    @see make_ares_error
 */
[[gnu::visibility("default")]] inline const error_category& ares_category() noexcept;

/** Makes an error code out of a c-ares status code.

    This is what to compare the outcome of an operation against:

    @code
    if (ec == c_ares_async::make_ares_error(ARES_ENODATA))
       ; // the name is there, a record of the type asked for is not
    @endcode

    `ARES_SUCCESS` is 0, so the code of an operation that went well tests false
    like any other.

    @param status One of the `ARES_E...` status codes, or `ARES_SUCCESS`.

    @return `status` as an error code in @ref ares_category.
 */
inline error_code make_ares_error(int status) noexcept;

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME

#include <c_ares_async/impl/error.ipp>
