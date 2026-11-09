#
# GoogleTest comes from wherever it is installed, and from its release archive where it is not.
# Nothing of it is installed along with the library either way.
#
include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    FIND_PACKAGE_ARGS NAMES GTest)

set(INSTALL_GTEST OFF)
FetchContent_MakeAvailable(googletest)

# --------------------------------------------------------------------------------------------------
#
# The tests ask a DNS server of their own on the loopback interface, see fake_dns_server.hpp, so
# they need no network and say the same wherever they run.
#
# Most of them are written in the spelling that both runtimes have (resolver::resolve(), see
# resolver_fixture.hpp) and are built on either. What is about one runtime only -- completion
# tokens here, stop tokens there -- is in asio/ or corosio/, next to the event loop fixture of
# that runtime, and only the one of this build is compiled. The fake DNS server is Asio's
# in both.
#
add_executable(
    c_ares_async_tests
    error_test.cpp
    event_loop_fixture.hpp
    fake_dns_server.cpp
    fake_dns_server.hpp
    https_test.cpp
    resolver_fixture.hpp
    resolver_test.cpp
    server_list_test.cpp
    shared_library.hpp
    shared_library_test.cpp
    test_certificate.cpp
    test_certificate.hpp
    tls_test.cpp)

if (c_ares_async_runtime STREQUAL "corosio")
    target_sources(c_ares_async_tests PRIVATE corosio/event_loop_fixture.hpp corosio/stop_test.cpp)
else()
    target_sources(c_ares_async_tests PRIVATE asio/async_test.cpp asio/event_loop_fixture.hpp
                                              asio/run.hpp)
endif()

target_compile_definitions(c_ares_async_tests PRIVATE BOOST_ASIO_NO_DEPRECATED)

#
# The fake DNS server is written in Asio whichever runtime the tests are on, so the tests of a
# COROSIO build need Boost as well -- the library does not.
#
if (c_ares_async_runtime STREQUAL "corosio")
    find_package(Boost ${C_ARES_ASYNC_BOOST_VERSION} REQUIRED)
endif()

# the fake server speaks TLS as well, with the same library the resolver links
target_link_libraries(
    c_ares_async_tests PRIVATE c_ares_async::${c_ares_async_runtime} Boost::headers GTest::gmock
                               GTest::gtest_main ${c_ares_async_tls_libraries} Threads::Threads)

#
# The headers compiled into a shared library of their own, which shows nothing but what it has to
# -- the way a library that uses c_ares_async would be built. What comes out of it has to be the
# same as what the tests make themselves, see shared_library_test.cpp.
#
add_library(c_ares_async_test_shared_library SHARED shared_library.cpp shared_library.hpp)
target_link_libraries(c_ares_async_test_shared_library
                      PRIVATE c_ares_async::${c_ares_async_runtime})
target_compile_definitions(c_ares_async_test_shared_library PRIVATE BOOST_ASIO_NO_DEPRECATED)
set_target_properties(c_ares_async_test_shared_library PROPERTIES CXX_VISIBILITY_PRESET hidden
                                                                  VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(c_ares_async_tests PRIVATE c_ares_async_test_shared_library)

#
# Both runtimes in one program, which a COROSIO build has the dependencies of: one lookup compiled
# with c_ares_async::asio, next to the tests that are compiled with c_ares_async::corosio. See
# corosio/mixed_runtimes_test.cpp.
#
if (c_ares_async_runtime STREQUAL "corosio")
    add_library(c_ares_async_test_asio_lookup STATIC corosio/asio_lookup.cpp
                                                     corosio/asio_lookup.hpp)
    target_link_libraries(c_ares_async_test_asio_lookup PRIVATE c_ares_async::asio)
    target_compile_definitions(c_ares_async_test_asio_lookup PRIVATE BOOST_ASIO_NO_DEPRECATED)

    target_sources(c_ares_async_tests PRIVATE corosio/mixed_runtimes_test.cpp)
    target_link_libraries(c_ares_async_tests PRIVATE c_ares_async_test_asio_lookup)
endif()

include(GoogleTest)
gtest_discover_tests(c_ares_async_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)

# --------------------------------------------------------------------------------------------------

#
# Every header has to compile when it is the only one included, with the runtime target of this
# build: each of them in a translation unit of its own, made here. Those that are for the other
# runtime only are left out. A pool of four keeps the compilers that run at the same time to what
# the memory takes, while the other tests run as well.
#
get_target_property(c_ares_async_header_set c_ares_async_headers HEADER_SET)
set(c_ares_async_header_sources)
foreach (header IN LISTS c_ares_async_header_set)
    file(RELATIVE_PATH include "${PROJECT_SOURCE_DIR}/include" "${header}")
    if (include MATCHES "^c_ares_async/(asio|corosio)/"
        AND NOT include MATCHES "^c_ares_async/${c_ares_async_runtime}/")
        continue()
    endif()

    string(MAKE_C_IDENTIFIER "${include}" name)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${name}.cpp")
    file(CONFIGURE OUTPUT "${source}" CONTENT "#include <${include}>\n")
    list(APPEND c_ares_async_header_sources "${source}")
endforeach()

add_library(c_ares_async_header_test OBJECT EXCLUDE_FROM_ALL ${c_ares_async_header_sources})
target_link_libraries(c_ares_async_header_test PRIVATE c_ares_async::${c_ares_async_runtime})
target_compile_definitions(c_ares_async_header_test PRIVATE BOOST_ASIO_NO_DEPRECATED)

set_property(GLOBAL APPEND PROPERTY JOB_POOLS c_ares_async_header_test=4)
set_target_properties(c_ares_async_header_test PROPERTIES JOB_POOL_COMPILE c_ares_async_header_test)

add_test(NAME headers.self_contained
         COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --config $<CONFIG> --target
                 c_ares_async_header_test)

if (C_ARES_ASYNC_INSTALL)
    include(cmake_test.cmake)
endif()
