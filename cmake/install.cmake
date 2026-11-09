#
# Install rules, and the package that find_package(c_ares_async) picks up afterwards.
#
# There is nothing but headers to install, and they serve both runtimes, whichever this build is
# on. What is exported is c_ares_async::headers; the runtime targets are made by the package
# configuration file, as the components asked for, where the runtime is there -- an exported one
# would have to name the Capy and Corosio of this build, which are not installed.
#
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(C_ARES_ASYNC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_DATADIR}/cmake/c_ares_async"
    CACHE STRING "Where to install the CMake package of c_ares_async, relative to the prefix")

install(
    TARGETS c_ares_async_headers
    EXPORT c_ares_async-targets
    FILE_SET HEADERS COMPONENT c_ares_async_development)

install(
    EXPORT c_ares_async-targets
    NAMESPACE c_ares_async::
    DESTINATION "${C_ARES_ASYNC_INSTALL_CMAKEDIR}"
    COMPONENT c_ares_async_development)

configure_package_config_file(
    cmake/c_ares_async-config.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/c_ares_async-config.cmake"
    INSTALL_DESTINATION "${C_ARES_ASYNC_INSTALL_CMAKEDIR}"
    NO_SET_AND_CHECK_MACRO)

#
# 0.2 may well break what 0.1 promised, so a consumer asking for 0.1 does not get 0.2. Headers
# are the same for every architecture.
#
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/c_ares_async-config-version.cmake"
    COMPATIBILITY SameMinorVersion
    ARCH_INDEPENDENT)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/c_ares_async-config.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/c_ares_async-config-version.cmake"
    DESTINATION "${C_ARES_ASYNC_INSTALL_CMAKEDIR}"
    COMPONENT c_ares_async_development)

install(
    FILES LICENSE_1_0.txt
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    COMPONENT c_ares_async_development)
