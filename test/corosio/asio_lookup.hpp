// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// A lookup on Asio, in a test program that is on Corosio otherwise: asio_lookup.cpp is compiled
// with c_ares_async::asio, everything else in a COROSIO build with c_ares_async::corosio. See
// mixed_runtimes_test.cpp.
//
#pragma once

#include <string>
#include <vector>

//
// The endpoints that 'host' resolves to, as "address:port", or the message of the error.
//
std::vector<std::string> resolve_on_asio(const std::string& host, const std::string& service);
