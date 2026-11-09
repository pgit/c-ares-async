#
# A consumer of c_ares_async, standing in for a project that has nothing to do with this one. It
# sets no language standard and names no dependency of the library: all of that has to come with
# the one target it links, of the runtime it asks for. See ../cmake_test.cmake for how it is run.
#
cmake_minimum_required(VERSION 3.25...4.4)

project(c_ares_async_cmake_test LANGUAGES CXX)

set(C_ARES_ASYNC_CONSUME "find_package" CACHE STRING "find_package or add_subdirectory")
set(C_ARES_ASYNC_RUNTIME "asio" CACHE STRING "asio or corosio")

#
# This runs next to the other tests, so it takes no more compilers at a time than the memory has
# room for.
#
set_property(GLOBAL APPEND PROPERTY JOB_POOLS consumer=4)
set(CMAKE_JOB_POOL_COMPILE consumer)
set(CMAKE_JOB_POOL_LINK consumer)

#
# Corosio has no installed package to find yet, so whoever uses it builds it as part of their own
# build. Here that is from the sources the library's own build fetched, which
# FETCHCONTENT_SOURCE_DIR_CAPY and FETCHCONTENT_SOURCE_DIR_COROSIO point at: nothing is downloaded.
#
if (C_ARES_ASYNC_RUNTIME STREQUAL "corosio")
    include(FetchContent)
    foreach (option TESTS EXAMPLES BENCH)
        set(BOOST_CAPY_BUILD_${option} OFF CACHE BOOL "" FORCE)
        set(BOOST_COROSIO_BUILD_${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(capy GIT_REPOSITORY https://github.com/cppalliance/capy.git
                         EXCLUDE_FROM_ALL)
    FetchContent_Declare(corosio GIT_REPOSITORY https://github.com/cppalliance/corosio.git
                         EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(capy corosio)
endif()

if (C_ARES_ASYNC_CONSUME STREQUAL "find_package")
    find_package(c_ares_async 0.1 CONFIG REQUIRED COMPONENTS ${C_ARES_ASYNC_RUNTIME})
elseif (C_ARES_ASYNC_CONSUME STREQUAL "add_subdirectory")
    string(TOUPPER ${C_ARES_ASYNC_RUNTIME} C_ARES_ASYNC_API)
    add_subdirectory(../.. c_ares_async)
else()
    message(FATAL_ERROR "C_ARES_ASYNC_CONSUME is '${C_ARES_ASYNC_CONSUME}'")
endif()

add_executable(consumer ${C_ARES_ASYNC_RUNTIME}.cpp)
target_link_libraries(consumer PRIVATE c_ares_async::${C_ARES_ASYNC_RUNTIME})

enable_testing()
add_test(NAME consumer COMMAND consumer)
