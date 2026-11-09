// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

//
// What resolver::resolve_https() comes back with. It is the same on either runtime, so it is in
// the namespace itself rather than in the inline one of a runtime.
//
#pragma once

#include <string>
#include <vector>

namespace c_ares_async
{

/** One HTTPS resource record (RFC 9460).

    The record that tells a client which protocols an endpoint speaks before
    it connects. It comes in two forms: with a @ref priority of 0 it is an
    alias, where @ref target names the host to go on with, and with any other
    it describes a service, where @ref params says how to reach it.

    @see resolver::resolve_https
 */
struct https_record
{
   /** 0 for an alias. For a service, its place among the records of the
       name: a client tries the lowest first.
    */
   unsigned short priority = 0;

   /** The name to go on with. Empty, or ".", where that is the name the
       record was found under.
    */
   std::string target;

   /** The service parameters, in presentation form.

       One string for each parameter, the way a zone file has it:
       "alpn=h3,h2", "port=8443", "ipv4hint=192.0.2.1,192.0.2.2" -- or the
       key alone for one that has no value, like "no-default-alpn". Every key
       has a value syntax of its own and c-ares hands them over as opaque
       bytes, so there is not much to be gained from carrying them around
       any further apart.

       A key that c-ares has no name for reads "key" and its number, and a
       value it has no format for is given in hexadecimal, like that of
       "ech".
    */
   std::vector<std::string> params;
};

} // namespace c_ares_async
