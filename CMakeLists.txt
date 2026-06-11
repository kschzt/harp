cmake_minimum_required(VERSION 3.16)
project(harp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

# Fuzz tree (spec §16 / T9): cmake -B build-fuzz -DHARP_FUZZ=ON.
# ASan goes tree-wide (every binary in this tree is sanitized — the abuse
# test runs the sanitized daemon); the libFuzzer engine is added per
# fuzz target below, on toolchains that ship it.
option(HARP_FUZZ "Build fuzz targets for the wire-facing parsers" OFF)
if(HARP_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "HARP_FUZZ needs clang")
  endif()
  add_compile_options(-fsanitize=address -g -O1)
  add_link_options(-fsanitize=address)
endif()

add_library(harpcore STATIC
  core/src/cbor.c
  core/src/sha256.c
  core/src/frame.c
  core/src/object.c
  core/src/store.c
  core/src/envelope.c
  core/src/link.c
  core/src/audio.c
)
target_include_directories(harpcore PUBLIC core/include)

find_package(Threads REQUIRED)
add_executable(harp-deviced
  device/harp-deviced.c # main + transports
  device/engine.c       # synth, event queue, audio threads
  device/state.c        # engine <-> content-addressed objects
  device/session.c      # protocol handlers + session loop
  device/panel.c        # front-panel unix-socket API
)
target_link_libraries(harp-deviced harpcore m Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(harp-deviced PRIVATE device/ffs.c) # USB gadget transport (§4.3)
endif()

add_executable(harp-probe host/harp-probe.c host/client.c)
target_link_libraries(harp-probe harpcore m)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBUSB libusb-1.0)
endif()
if(LIBUSB_FOUND)
  target_sources(harp-probe PRIVATE host/usb_io.c)
  target_compile_definitions(harp-probe PRIVATE HAVE_LIBUSB)
  target_include_directories(harp-probe PRIVATE ${LIBUSB_INCLUDE_DIRS})
  target_link_directories(harp-probe PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(harp-probe ${LIBUSB_LIBRARIES})
  message(STATUS "harp-probe: libusb ${LIBUSB_VERSION} found, USB transport enabled")
endif()

enable_testing()
add_executable(harp-tests tests/harp_tests.c)
target_link_libraries(harp-tests harpcore m)
add_test(NAME core COMMAND harp-tests)

# Fuzz targets. Apple clang ships no libFuzzer runtime -> a standalone
# corpus-replay driver stands in there; coverage-guided runs happen on
# Linux (CI). gen-corpus seeds both with valid wire bytes.
if(HARP_FUZZ)
  if(APPLE)
    set(FUZZ_DRIVER fuzz/standalone_driver.c)
  else()
    set(FUZZ_DRIVER "")
    target_compile_options(harpcore PRIVATE -fsanitize=fuzzer-no-link)
  endif()
  foreach(t cbor envelope object link audio)
    add_executable(fuzz-${t} fuzz/fuzz_${t}.c ${FUZZ_DRIVER})
    target_link_libraries(fuzz-${t} harpcore m)
    if(NOT APPLE)
      target_compile_options(fuzz-${t} PRIVATE -fsanitize=fuzzer)
      target_link_options(fuzz-${t} PRIVATE -fsanitize=fuzzer)
    endif()
  endforeach()
  add_executable(gen-corpus fuzz/gen_corpus.c)
  target_link_libraries(gen-corpus harpcore m)
endif()
