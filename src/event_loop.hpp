// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// The event loop itself, as the implementation reaches it -- what the public runtime layer
// (c_ares_async/runtime.hpp) does not need to show anybody:
//
//    wait_type, Descriptor   -- readiness on a descriptor of c-ares', which it keeps owning
//    address_text()          -- the text of an address that c-ares hands over in its wire format
//    make_endpoint()         -- a TcpEndpoint of the runtime's own, from a sockaddr of c-ares'
//
// The timer that ares_timeout() asks for is the runtime layer's, detail::Timer.
//
// asio/event_loop.hpp has the documentation of each, and corosio/event_loop.hpp the same
// definitions in the same order.
//
#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_COROSIO
#include "corosio/event_loop.hpp"
#else
#include "asio/event_loop.hpp"
#endif
