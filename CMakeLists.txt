cmake_minimum_required(VERSION 3.16)
project(cassandra_function_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CASSANDRA REQUIRED IMPORTED_TARGET cassandra)

add_executable(cassandra_function_tests main.cpp)
target_link_libraries(cassandra_function_tests PRIVATE PkgConfig::CASSANDRA)
target_compile_definitions(cassandra_function_tests PRIVATE
  CASS_DRIVER_VERSION="${CASSANDRA_VERSION}")
target_compile_options(cassandra_function_tests PRIVATE -Wall -Wextra)

set_target_properties(cassandra_function_tests PROPERTIES
  BUILD_RPATH "${CASSANDRA_LIBDIR}"
  INSTALL_RPATH "${CASSANDRA_LIBDIR}")

enable_testing()
add_test(NAME function_tests COMMAND cassandra_function_tests)
