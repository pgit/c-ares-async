cmake_minimum_required(VERSION 3.25...4.4)

#
# The version is the one in include/c_ares_async/version.hpp, which is there for a consumer of the
# headers that has no CMake package to ask -- so that is where it is said, and read from here.
#
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/c_ares_async/version.hpp" c_ares_async_version_h
     REGEX "^#define C_ARES_ASYNC_VERSION_(MAJOR|MINOR|PATCH|STRING) ")
foreach (part IN ITEMS MAJOR MINOR PATCH)
    if (NOT c_ares_async_version_h MATCHES "C_ARES_ASYNC_VERSION_${part} ([0-9]+)")
        message(FATAL_ERROR "include/c_ares_async/version.hpp has no C_ARES_ASYNC_VERSION_${part}")
    endif()
    list(APPEND c_ares_async_version ${CMAKE_MATCH_1})
endforeach()
list(JOIN c_ares_async_version "." c_ares_async_version)
if (NOT c_ares_async_version_h MATCHES "C_ARES_ASYNC_VERSION_STRING \"${c_ares_async_version}\"")
    message(FATAL_ERROR "include/c_ares_async/version.hpp: C_ARES_ASYNC_VERSION_STRING is not "
                        "\"${c_ares_async_version}\", which its numbers say")
endif()

project(
    c_ares_async
    VERSION ${c_ares_async_version}
    DESCRIPTION "c-ares name resolution as asynchronous operations, on Asio or Corosio"
    HOMEPAGE_URL "https://github.com/pgit/c-ares-async"
    LANGUAGES CXX)

if (WIN32)
    message(FATAL_ERROR "c_ares_async waits on its sockets as POSIX descriptors, which Windows "
                        "does not have.")
endif()

# there are no C++ modules in here, so there is nothing for CMake to scan the sources for
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# --------------------------------------------------------------------------------------------------
#
# Options. Tests, examples and documentation are the business of whoever works on the library
# itself, so they are only on by default where this is the top-level project -- not where it came
# in through add_subdirectory() or FetchContent.
#
# Nothing in here sets a compiler, a standard library or warning flags: that is up to the one who
# builds, and CMakePresets.json has what this repository is developed with.
#

#
# The library is headers, and serves both runtimes: c_ares_async::asio and c_ares_async::corosio
# are there wherever their runtime is. What C_ARES_ASYNC_API chooses is the runtime that the
# tests, the examples and the code of the guide are built on, one per build tree: ASIO
# (completion tokens, on Asio) or COROSIO (coroutines only, on Corosio), and which of the two
# this build makes sure is there. A COROSIO build fetches Capy and Corosio, unless whoever brought
# this in has done so already. See doc/corosio.rst.
#
set(C_ARES_ASYNC_API "ASIO"
    CACHE STRING "I/O runtime to build the tests and examples on: ASIO or COROSIO")
set_property(CACHE C_ARES_ASYNC_API PROPERTY STRINGS ASIO COROSIO)

if (PROJECT_IS_TOP_LEVEL AND (NOT DEFINED BUILD_TESTING OR BUILD_TESTING))
    set(c_ares_async_tests_default ON)
else()
    set(c_ares_async_tests_default OFF)
endif()

option(C_ARES_ASYNC_BUILD_TESTS "Build the tests of c_ares_async" ${c_ares_async_tests_default})
option(C_ARES_ASYNC_BUILD_EXAMPLES "Build the examples of c_ares_async" ${PROJECT_IS_TOP_LEVEL})
option(C_ARES_ASYNC_BUILD_DOCS
       "Build the code of the guide, and add the 'docs' target where Doxygen and Sphinx are there"
       ${PROJECT_IS_TOP_LEVEL})
option(C_ARES_ASYNC_INSTALL "Generate the install rules of c_ares_async" ${PROJECT_IS_TOP_LEVEL})

#
# c-ares has no DNS over TLS of its own -- see detail/tls_transport.hpp. Either library will do:
# the transport sticks to the OpenSSL API that AWS-LC implements as well, and tells the two apart
# by the headers it sees. What is chosen here is what the targets link.
#
set(C_ARES_ASYNC_TLS "OpenSSL" CACHE STRING "TLS library to link: OpenSSL or AWS-LC")
set_property(CACHE C_ARES_ASYNC_TLS PROPERTY STRINGS OpenSSL AWS-LC)

# --------------------------------------------------------------------------------------------------
#
# Dependencies. The versions are kept in variables because the package configuration file asks
# for the same ones again, on behalf of whoever uses the installed headers.
#
set(C_ARES_ASYNC_BOOST_VERSION 1.88)
set(C_ARES_ASYNC_CARES_VERSION 1.34) # ares_process_fds() was added in 1.34
set(C_ARES_ASYNC_OPENSSL_VERSION 3.2) # SSL_get0_group_name() was added in 3.2

find_package(Threads REQUIRED)
find_package(c-ares ${C_ARES_ASYNC_CARES_VERSION} CONFIG REQUIRED)

if (C_ARES_ASYNC_API STREQUAL "ASIO")
    find_package(Boost ${C_ARES_ASYNC_BOOST_VERSION} REQUIRED)
elseif (C_ARES_ASYNC_API STREQUAL "COROSIO")
    #
    # Asio is not what this build is about, but c_ares_async::asio is there wherever Boost is.
    #
    find_package(Boost ${C_ARES_ASYNC_BOOST_VERSION} QUIET)

    #
    # Pinned: both are pre-release and change their API on 'develop' without notice, so moving a
    # pin is a commit of its own. They are built as part of this build, from their sources, and
    # are not installed along with the headers -- a consumer of the installed package that uses
    # Corosio brings its own.
    #
    if (NOT TARGET Boost::corosio OR NOT TARGET Boost::capy)
        include(FetchContent)
        foreach (option TESTS EXAMPLES BENCH)
            set(BOOST_CAPY_BUILD_${option} OFF CACHE BOOL "" FORCE)
            set(BOOST_COROSIO_BUILD_${option} OFF CACHE BOOL "" FORCE)
        endforeach()
        FetchContent_Declare(
            capy
            GIT_REPOSITORY https://github.com/cppalliance/capy.git
            GIT_TAG bdadc6adef8c8eb79da6454373175bb1428be524
            EXCLUDE_FROM_ALL)
        FetchContent_Declare(
            corosio
            GIT_REPOSITORY https://github.com/cppalliance/corosio.git
            GIT_TAG fd8e95a7125047d5385068a4c640da8232b9588e
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(capy corosio)

        #
        # The headers may well be compiled into a shared library -- the tests make one -- and
        # Capy and Corosio go in there with them, so they have to be position-independent code,
        # which a static library is not by default.
        #
        set_target_properties(boost_capy boost_corosio PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
else()
    message(FATAL_ERROR "C_ARES_ASYNC_API is '${C_ARES_ASYNC_API}', expected ASIO or COROSIO")
endif()

#
# https://github.com/c-ares/c-ares/security/advisories/GHSA-6wfj-rwm7-3542 is a use-after-free in
# the completion of a query, which an application calling ares_cancel() can trigger -- and
# cancelling an operation of the resolver does exactly that.
#
if (c-ares_VERSION VERSION_LESS 1.34.7)
    message(WARNING "c-ares ${c-ares_VERSION} is affected by GHSA-6wfj-rwm7-3542, a use-after-free "
                    "that cancelling a lookup can trigger. It is fixed in c-ares 1.34.7.")
endif()

if (C_ARES_ASYNC_TLS STREQUAL "OpenSSL")
    find_package(OpenSSL ${C_ARES_ASYNC_OPENSSL_VERSION} REQUIRED)
    set(c_ares_async_tls_libraries OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "c_ares_async: using TLS library OpenSSL ${OPENSSL_VERSION}")
elseif (C_ARES_ASYNC_TLS STREQUAL "AWS-LC")
    # ssl_ROOT says where it is installed, and finds 'crypto' for ssl-config.cmake as well
    find_package(ssl CONFIG REQUIRED)
    set(c_ares_async_tls_libraries AWS::ssl)
    message(STATUS "c_ares_async: using TLS library AWS-LC (${ssl_DIR})")
else()
    message(FATAL_ERROR "C_ARES_ASYNC_TLS is '${C_ARES_ASYNC_TLS}', expected OpenSSL or AWS-LC")
endif()

# --------------------------------------------------------------------------------------------------
#
# The library: headers, and nothing to build. c_ares_async::headers is what either runtime needs
# of them -- the headers, c-ares, the TLS library, threads and C++20 -- and the runtime targets add
# their runtime to it, and define the macro that selects it in c_ares_async/runtime.hpp. Those are
# what a program links: c_ares_async::asio or c_ares_async::corosio.
#
add_library(c_ares_async_headers INTERFACE)
add_library(c_ares_async::headers ALIAS c_ares_async_headers)
set_target_properties(c_ares_async_headers PROPERTIES EXPORT_NAME headers)

target_sources(
    c_ares_async_headers
    INTERFACE FILE_SET HEADERS
              BASE_DIRS include
              FILES include/c_ares_async.hpp
                    include/c_ares_async/asio/event_loop.hpp
                    include/c_ares_async/asio/runtime.hpp
                    include/c_ares_async/corosio/event_loop.hpp
                    include/c_ares_async/corosio/runtime.hpp
                    include/c_ares_async/detail/event_loop.hpp
                    include/c_ares_async/detail/log.hpp
                    include/c_ares_async/detail/server_list.hpp
                    include/c_ares_async/detail/socket_functions.hpp
                    include/c_ares_async/detail/tls_transport.hpp
                    include/c_ares_async/error.hpp
                    include/c_ares_async/https_record.hpp
                    include/c_ares_async/impl/error.ipp
                    include/c_ares_async/impl/resolver.ipp
                    include/c_ares_async/impl/server_list.ipp
                    include/c_ares_async/impl/socket_functions.ipp
                    include/c_ares_async/impl/tls_transport.ipp
                    include/c_ares_async/logging.hpp
                    include/c_ares_async/resolver.hpp
                    include/c_ares_async/runtime.hpp
                    include/c_ares_async/tls_options.hpp
                    include/c_ares_async/version.hpp)

target_compile_features(c_ares_async_headers INTERFACE cxx_std_20)
target_link_libraries(c_ares_async_headers INTERFACE c-ares::cares ${c_ares_async_tls_libraries}
                                                     Threads::Threads)

if (TARGET Boost::headers)
    add_library(c_ares_async_asio INTERFACE)
    add_library(c_ares_async::asio ALIAS c_ares_async_asio)
    target_link_libraries(c_ares_async_asio INTERFACE c_ares_async_headers Boost::headers)
    target_compile_definitions(c_ares_async_asio INTERFACE C_ARES_ASYNC_ASIO=1)
endif()

if (TARGET Boost::corosio AND TARGET Boost::capy)
    add_library(c_ares_async_corosio INTERFACE)
    add_library(c_ares_async::corosio ALIAS c_ares_async_corosio)
    target_link_libraries(c_ares_async_corosio INTERFACE c_ares_async_headers Boost::corosio
                                                         Boost::capy)
    target_compile_definitions(c_ares_async_corosio INTERFACE C_ARES_ASYNC_COROSIO=1)
endif()

#
# What the tests, the examples and the snippets link: the runtime of this build tree, which is
# also the name of the directories that what belongs to one runtime only lives in.
#
string(TOLOWER ${C_ARES_ASYNC_API} c_ares_async_runtime)

foreach (runtime IN ITEMS asio corosio)
    if (TARGET c_ares_async::${runtime})
        message(STATUS "c_ares_async: c_ares_async::${runtime} is there")
    endif()
endforeach()

# --------------------------------------------------------------------------------------------------

if (C_ARES_ASYNC_INSTALL)
    include(cmake/install.cmake)
endif()

if (C_ARES_ASYNC_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

if (C_ARES_ASYNC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if (C_ARES_ASYNC_BUILD_DOCS)
    add_subdirectory(doc)
endif()
