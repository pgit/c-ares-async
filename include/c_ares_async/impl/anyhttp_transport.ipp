// Copyright (c) 2026 Peter Eisenlohr
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <c_ares_async/runtime.hpp>

#if C_ARES_ASYNC_ANYHTTP // as detail/anyhttp_transport.hpp: empty without

#include <c_ares_async/detail/anyhttp_bridge.hpp>
#include <c_ares_async/detail/anyhttp_transport.hpp> // NOLINT(misc-header-include-cycle)
#include <c_ares_async/detail/doh_bridge.hpp>
#include <c_ares_async/detail/doq_bridge.hpp>
#include <c_ares_async/detail/event_loop.hpp>
#include <c_ares_async/detail/socket_functions.hpp>

#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail
{

inline anyhttp_transport::descriptor::descriptor(std::shared_ptr<anyhttp_bridge> bridge) noexcept
   : m_bridge(std::move(bridge))
{
}

inline void anyhttp_transport::descriptor::detach()
{
   if (m_bridge)
      std::exchange(m_bridge, nullptr)->cancel_wait();
}

inline void anyhttp_transport::descriptor::wait(wait_type type,
                                                std::function<void(const error_code&)> on_ready)
{
   if (m_bridge)
      m_bridge->wait(type, std::move(on_ready));
}

// --------------------------------------------------------------------------------------------------

inline anyhttp_transport::anyhttp_transport(Executor executor, tls_options options,
                                            const log_function& logger)
   : m_executor(std::move(executor)), m_options(std::move(options)), m_logger(logger)
{
}

inline anyhttp_transport::~anyhttp_transport()
{
   for (auto& [fd, bridge] : m_bridges)
      bridge->stop();
}

inline void anyhttp_transport::install(ares_channel_t* channel)
{
   if (const auto status = ares_set_socket_functions_ex(channel, &functions(), this);
       status != ARES_SUCCESS)
      throw std::runtime_error(std::string("ares_set_socket_functions_ex: ") +
                               ares_strerror(static_cast<int>(status)));
}

inline std::optional<anyhttp_transport::descriptor>
anyhttp_transport::find_descriptor(ares_socket_t fd) const
{
   const auto it = m_bridges.find(fd);
   if (it == m_bridges.end())
      return std::nullopt;

   return descriptor(it->second);
}

inline std::shared_ptr<anyhttp_bridge> anyhttp_transport::make_bridge(ares_socket_t fd,
                                                                      int domain) const
{
   if (m_options.https)
      return std::make_shared<doh_bridge>(m_executor, fd, domain, m_options, m_logger);

   return std::make_shared<doq_bridge>(m_executor, fd, domain, m_options, m_logger);
}

inline anyhttp_bridge* anyhttp_transport::find(ares_socket_t fd) const
{
   const auto it = m_bridges.find(fd);
   return it == m_bridges.end() ? nullptr : it->second.get();
}

// --------------------------------------------------------------------------------------------------

inline ares_socket_t anyhttp_transport::on_socket(int domain, int type, int protocol)
{
   //
   // With ARES_FLAG_USEVC, c-ares sends no query over UDP. It does make UDP sockets all the same:
   // ares_getaddrinfo() sorts the addresses it has found (RFC 6724) by the source address the
   // system would send to each from, which it learns by connecting a UDP socket to it and asking
   // for its local address -- nothing is sent. Those are passed through, untouched, as the TLS
   // transport does.
   //
   if (type != SOCK_STREAM)
   {
      const int fd = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
      return fd < 0 ? ARES_SOCKET_BAD : fd;
   }

   //
   // A socket of ours is no socket of the system's, but its number has to be one that none of
   // those can have while it is ours: c-ares tells its sockets apart by it, and the UDP ones above
   // are the system's. A descriptor that is open has a number of its own, and nothing is ever done
   // on this one -- c-ares calls the functions below for everything, and the resolver waits on
   // the bridge.
   //
   const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return ARES_SOCKET_BAD;

   //
   // Nothing is to be thrown back into c-ares, which is C.
   //
   try
   {
      m_bridges.emplace(fd, make_bridge(fd, domain));
   }
   catch (...)
   {
      ::close(fd);
      errno = ENOMEM;
      return ARES_SOCKET_BAD;
   }

   return fd;
}

inline int anyhttp_transport::on_close(ares_socket_t fd)
{
   if (const auto it = m_bridges.find(fd); it != m_bridges.end())
   {
      it->second->stop();
      m_bridges.erase(it);
   }

   return ::close(fd);
}

//
// A socket of ours has none of the options c-ares sets: what they are for is anyhttp's socket,
// which takes none of them. That turns fast open down as well, which is what c-ares takes ENOSYS
// for, see socket_functions.hpp -- and there is no connection that it could take for one that is
// up too early, as the connect is through at once.
//
inline int anyhttp_transport::on_setsockopt(ares_socket_t fd, ares_socket_opt_t opt,
                                            const void* value, ares_socklen_t size)
{
   if (!find(fd))
      return set_socket_option(fd, opt, value, size);

   errno = ENOSYS;
   return -1;
}

//
// To c-ares, the connect is through at once; anyhttp's connection takes its time behind it, and
// the queries wait in the bridge until it is there.
//
inline int anyhttp_transport::on_connect(ares_socket_t fd, const struct sockaddr* address,
                                         ares_socklen_t length)
{
   auto* bridge = find(fd);
   if (!bridge)
      return ::connect(fd, address, length);

   const auto server = make_endpoint(address, length);
   if (!server)
   {
      errno = EINVAL;
      return -1;
   }

   bridge->start(*server);
   return 0;
}

inline ares_ssize_t anyhttp_transport::on_recv(ares_socket_t fd, void* buffer, size_t length)
{
   if (auto* bridge = find(fd))
      return bridge->receive(buffer, length);

   return ::recv(fd, buffer, length, 0);
}

//
// A socket of the system's may be closed by the time c-ares writes to it, and that is to be a
// failed write, not a SIGPIPE.
//
inline ares_ssize_t anyhttp_transport::on_send(ares_socket_t fd, const void* buffer, size_t length,
                                               int flags)
{
   if (auto* bridge = find(fd))
      return bridge->send(buffer, length);

   return ::send(fd, buffer, length, flags | MSG_NOSIGNAL);
}

//
// The local address of a socket of ours is that of the socket that anyhttp makes for the
// connection, which is not known yet when c-ares asks. What it gets is the family it asked for.
//
inline int anyhttp_transport::on_getsockname(ares_socket_t fd, struct sockaddr* address,
                                             ares_socklen_t* length)
{
   auto* bridge = find(fd);
   if (!bridge)
      return ::getsockname(fd, address, length);

   const auto size = bridge->domain() == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
   if (!address || !length || *length < size)
   {
      errno = EINVAL;
      return -1;
   }

   std::memset(address, 0, size);
   address->sa_family = static_cast<sa_family_t>(bridge->domain());
   *length = static_cast<ares_socklen_t>(size);
   return 0;
}

//
// c-ares binds a socket to the channel's local address of its family, see resolver.hpp, and for
// one of ours that is where the connection comes from.
//
inline int anyhttp_transport::on_bind(ares_socket_t fd, const struct sockaddr* address,
                                      ares_socklen_t length)
{
   if (auto* bridge = find(fd))
      return bridge->bind(address, length);

   return ::bind(fd, address, length);
}

// --------------------------------------------------------------------------------------------------

//
// The C side of it.
//
inline const struct ares_socket_functions_ex& anyhttp_transport::functions()
{
   static const struct ares_socket_functions_ex table{
      .version = 1,
      .flags = ARES_SOCKFUNC_FLAG_NONBLOCKING,

      .asocket = [](int domain, int type, int protocol, void* data) -> ares_socket_t {
         return static_cast<anyhttp_transport*>(data)->on_socket(domain, type, protocol);
      },

      .aclose = [](ares_socket_t fd, void* data) -> int {
         return static_cast<anyhttp_transport*>(data)->on_close(fd);
      },

      .asetsockopt = [](ares_socket_t fd, ares_socket_opt_t opt, const void* value,
                        ares_socklen_t size, void* data) -> int {
         return static_cast<anyhttp_transport*>(data)->on_setsockopt(fd, opt, value, size);
      },

      .aconnect = [](ares_socket_t fd, const struct sockaddr* address, ares_socklen_t length,
                     unsigned int, void* data) -> int {
         return static_cast<anyhttp_transport*>(data)->on_connect(fd, address, length);
      },

      .arecvfrom = [](ares_socket_t fd, void* buffer, size_t length, int, struct sockaddr* address,
                      ares_socklen_t* address_len, void* data) -> ares_ssize_t {
         if (address && address_len)
            *address_len = 0;
         return static_cast<anyhttp_transport*>(data)->on_recv(fd, buffer, length);
      },

      .asendto = [](ares_socket_t fd, const void* buffer, size_t length, int flags,
                    const struct sockaddr*, ares_socklen_t, void* data) -> ares_ssize_t {
         return static_cast<anyhttp_transport*>(data)->on_send(fd, buffer, length, flags);
      },

      .agetsockname = [](ares_socket_t fd, struct sockaddr* address, ares_socklen_t* length,
                         void* data) -> int { //
         return static_cast<anyhttp_transport*>(data)->on_getsockname(fd, address, length);
      },

      .abind = [](ares_socket_t fd, unsigned int, const struct sockaddr* address,
                  ares_socklen_t length, void* data) -> int { //
         return static_cast<anyhttp_transport*>(data)->on_bind(fd, address, length);
      },

      .aif_nametoindex = [](const char* name, void*) -> unsigned int {
         return ::if_nametoindex(name);
      },

      .aif_indextoname = [](unsigned int index, char* buffer, size_t size, void*) -> const char* {
         if (size < IF_NAMESIZE)
            return nullptr;
         return ::if_indextoname(index, buffer);
      },
   };

   return table;
}

} // namespace c_ares_async::inline C_ARES_ASYNC_RUNTIME::detail

#endif // C_ARES_ASYNC_ANYHTTP
