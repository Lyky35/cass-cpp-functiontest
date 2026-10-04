cmake_minimum_required(VERSION 3.16)
project(cassandra_function_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Where the cpp-driver is installed. Default: whatever pkg-config sees.
# Point it at the Lyky35 proto-v5 fork install:
#   cmake -S . -B build -DCASSANDRA_DRIVER_ROOT=/path/to/cassandra-cpp-driver/_install
set(CASSANDRA_DRIVER_ROOT "" CACHE PATH
    "cpp-driver install prefix (e.g. Lyky35/cassandra-cpp-driver branch proto-v5)")

if(CASSANDRA_DRIVER_ROOT)
  foreach(_pcdir lib/pkgconfig lib64/pkgconfig lib/x86_64-linux-gnu/pkgconfig)
    if(EXISTS "${CASSANDRA_DRIVER_ROOT}/${_pcdir}/cassandra.pc")
      set(ENV{PKG_CONFIG_PATH}
          "${CASSANDRA_DRIVER_ROOT}/${_pcdir}:$ENV{PKG_CONFIG_PATH}")
    endif()
  endforeach()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CASSANDRA REQUIRED IMPORTED_TARGET cassandra)
message(STATUS "cpp-driver: ${CASSANDRA_VERSION} (${CASSANDRA_LIBDIR})")

add_executable(cassandra_function_tests main.cpp)
target_link_libraries(cassandra_function_tests PRIVATE PkgConfig::CASSANDRA)
target_compile_definitions(cassandra_function_tests PRIVATE
  CASS_DRIVER_VERSION="${CASSANDRA_VERSION}")
target_compile_options(cassandra_function_tests PRIVATE -Wall -Wextra)

# build tree: link against the driver install prefix
# install tree: resolve libcassandra.so next to the binary ($ORIGIN/lib)
set_target_properties(cassandra_function_tests PROPERTIES
  BUILD_RPATH "${CASSANDRA_LIBDIR}"
  INSTALL_RPATH "\$ORIGIN/lib"
  INSTALL_RPATH_USE_LINK_PATH FALSE)

install(TARGETS cassandra_function_tests RUNTIME DESTINATION .)
install(FILES cassandra.conf DESTINATION .)

file(GLOB CASSANDRA_BUNDLED_LIBS "${CASSANDRA_LIBDIR}/libcassandra.so*")
install(FILES ${CASSANDRA_BUNDLED_LIBS} DESTINATION lib)

enable_testing()
add_test(NAME function_tests COMMAND cassandra_function_tests)
