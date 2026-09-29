cmake_minimum_required(VERSION 3.15)

project(transit_format_c VERSION 0.8.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# When used as a subproject (add_subdirectory or FetchContent), build just the
# library and leave the build type to the parent project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(TRANSIT_TOP_LEVEL ON)
else()
  set(TRANSIT_TOP_LEVEL OFF)
endif()

option(TRANSIT_BUILD_TESTS "Build the tests" ${TRANSIT_TOP_LEVEL})
option(TRANSIT_BUILD_TOOLS "Build transit-roundtrip" ${TRANSIT_TOP_LEVEL})
option(TRANSIT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(TRANSIT_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  # fopen, getenv and friends are standard C; don't warn about them
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  set(TRANSIT_WARNINGS /W4)
  if(TRANSIT_WARNINGS_AS_ERRORS)
    list(APPEND TRANSIT_WARNINGS /WX)
  endif()
else()
  set(TRANSIT_WARNINGS -Wall -Wextra -pedantic -Wshadow -Wconversion -Wno-sign-conversion)
  if(TRANSIT_WARNINGS_AS_ERRORS)
    list(APPEND TRANSIT_WARNINGS -Werror)
  endif()
endif()

add_library(transit
  src/transit_json.c
  src/transit_msgpack.c
  src/transit_read.c
  src/transit_util.c
  src/transit_value.c
  src/transit_write.c)
add_library(transit::transit ALIAS transit)
target_include_directories(transit PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_options(transit PRIVATE ${TRANSIT_WARNINGS})
# Export everything from a Windows DLL (the API is all of transit.h).
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
set_target_properties(transit PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  C_VISIBILITY_PRESET default)
if(NOT MSVC)
  find_library(MATH_LIBRARY m)
  if(MATH_LIBRARY)
    target_link_libraries(transit PUBLIC ${MATH_LIBRARY})
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS transit EXPORT transitTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TRANSIT_BUILD_TOOLS)
  add_executable(transit-roundtrip tools/transit_roundtrip.c)
  target_link_libraries(transit-roundtrip PRIVATE transit)
  target_compile_options(transit-roundtrip PRIVATE ${TRANSIT_WARNINGS})
  install(TARGETS transit-roundtrip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES include/transit.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT transitTargets NAMESPACE transit:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/transit
  FILE transitConfig.cmake)

if(TRANSIT_BUILD_TESTS)
  enable_testing()
  add_executable(transit-tests tests/test_transit.c)
  target_link_libraries(transit-tests PRIVATE transit)
  target_compile_options(transit-tests PRIVATE ${TRANSIT_WARNINGS})
  set(TRANSIT_FORMAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../transit-format" CACHE PATH
      "A checkout of transit-format, for the exemplar tests")
  add_test(NAME transit-tests COMMAND transit-tests)
  set_tests_properties(transit-tests PROPERTIES ENVIRONMENT "TRANSIT_FORMAT_DIR=${TRANSIT_FORMAT_DIR}")
endif()
