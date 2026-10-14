cmake_minimum_required(VERSION 3.20)
project(MooseShim CXX)

# --- product identity --------------------------------------------------------
# SHIM_NAME is the DLL filename, the INI filename beside it, the registry key
# SDA enumerates and the tag in every log line. Those four must agree, so they
# all derive from here and renaming the shim costs one line.
#
# The name is deliberately not the author's: it ends up in SDA's hardware list,
# where the operator needs to read what the device is. Authorship lives in the
# version resource (cmake/version.rc.in) and in LICENSE.
set(SHIM_NAME "MooseShim" CACHE STRING "Product name: DLL, INI, registry key, log tag")
set(SHIM_VERSION "0.4.0")
set(SHIM_AUTHOR "Sirse")
set(SHIM_COPYRIGHT "Copyright (c) 2026 Sirse")

string(TOUPPER "${SHIM_NAME}" SHIM_NAME_UPPER)
string(REPLACE "." "," SHIM_VERSION_COMMA "${SHIM_VERSION}")
set(SHIM_VERSION_COMMA "${SHIM_VERSION_COMMA},0")

set(SHIM_GENERATED "${CMAKE_BINARY_DIR}/generated")
configure_file(cmake/product.h.in  "${SHIM_GENERATED}/product.h"        @ONLY)
configure_file(cmake/version.rc.in "${SHIM_GENERATED}/version.rc"       @ONLY)
configure_file(cmake/exports.def.in "${SHIM_GENERATED}/${SHIM_NAME}.def" @ONLY)
configure_file(shim.ini            "${SHIM_GENERATED}/${SHIM_NAME}.ini" COPYONLY)

# SDA is a 32-bit VB6/COM process: everything here must be x86.
# Configure with:  cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
if(MSVC AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "${SHIM_NAME} must be built for Win32 (x86). Re-run cmake with -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: the DLL is loaded into someone else's fragile process and must
# not depend on a redistributable runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# --- the shim itself ---------------------------------------------------------
# The decision layer is a separate object library so the unit tests can link it
# without going through the DLL.
add_library(shim_core OBJECT
  shim/intercept.cpp
  shim/config.cpp
  shim/log.cpp
  shim/d2/d2_transport.cpp
)
target_include_directories(shim_core PUBLIC shim "${SHIM_GENERATED}")

add_library(shim_dll SHARED
  shim/exports.cpp
  shim/target.cpp
  "${SHIM_GENERATED}/version.rc"
  $<TARGET_OBJECTS:shim_core>
)
target_include_directories(shim_dll PRIVATE shim "${SHIM_GENERATED}")
set_target_properties(shim_dll PROPERTIES
  OUTPUT_NAME "${SHIM_NAME}"
  LINK_FLAGS "/DEF:\"${SHIM_GENERATED}/${SHIM_NAME}.def\""
)

# The INI is copied next to the DLL so a fresh build is runnable.
add_custom_command(TARGET shim_dll POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${SHIM_GENERATED}/${SHIM_NAME}.ini"
          "$<TARGET_FILE_DIR:shim_dll>/${SHIM_NAME}.ini")

# --- mock target driver ------------------------------------------------------
add_library(mockj2534 SHARED mock/mockj2534.cpp)
set_target_properties(mockj2534 PROPERTIES
  OUTPUT_NAME "mockj2534"
  LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/mock/mockj2534.def\""
)

# --- tests -------------------------------------------------------------------
# Never touch real hardware: the target driver is pinned to the mock, both here
# and in the test binary itself.
add_executable(shim_tests
  tests/doctest_main.cpp
  tests/test_config.cpp
  tests/test_d2.cpp
  tests/test_intercept.cpp
  tests/test_shim.cpp
  $<TARGET_OBJECTS:shim_core>
)
target_include_directories(shim_tests
  PRIVATE tests shim "${SHIM_GENERATED}")
add_dependencies(shim_tests shim_dll mockj2534)

# The integration tests load the shim and mockj2534.dll by name from the
# working directory, so they have to sit together.
add_custom_command(TARGET shim_tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:shim_dll>" "$<TARGET_FILE_DIR:shim_tests>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:mockj2534>" "$<TARGET_FILE_DIR:shim_tests>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${SHIM_GENERATED}/${SHIM_NAME}.ini"
          "$<TARGET_FILE_DIR:shim_tests>/${SHIM_NAME}.ini")

if(MSVC)
  # doctest is third-party; do not hold it to /W4.
  set_source_files_properties(tests/doctest_main.cpp PROPERTIES
    COMPILE_FLAGS "/W1")
endif()

enable_testing()
add_test(NAME shim_tests
         COMMAND shim_tests
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:shim_tests>")
set_tests_properties(shim_tests PROPERTIES
  ENVIRONMENT "${SHIM_NAME_UPPER}_TARGET=mockj2534.dll")

add_executable(d2_integration_tests
  tests/doctest_main.cpp
  tests/test_d2_integration.cpp
  $<TARGET_OBJECTS:shim_core>
)
target_include_directories(d2_integration_tests
  PRIVATE tests shim "${SHIM_GENERATED}")
add_dependencies(d2_integration_tests shim_dll mockj2534)
add_custom_command(TARGET d2_integration_tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:shim_dll>" "$<TARGET_FILE_DIR:d2_integration_tests>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:mockj2534>" "$<TARGET_FILE_DIR:d2_integration_tests>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${SHIM_GENERATED}/${SHIM_NAME}.ini"
          "$<TARGET_FILE_DIR:d2_integration_tests>/${SHIM_NAME}.ini")
add_test(NAME d2_integration_tests
         COMMAND d2_integration_tests
         WORKING_DIRECTORY "$<TARGET_FILE_DIR:d2_integration_tests>")
set_tests_properties(d2_integration_tests PROPERTIES
  ENVIRONMENT "${SHIM_NAME_UPPER}_TARGET=mockj2534.dll;${SHIM_NAME_UPPER}_D2_ROUTE=can")
