cmake_minimum_required(VERSION 3.16)
project(harp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Warnings + math lib: MSVC speaks neither -Wall/-Wextra nor a separate libm
# (math lives in the CRT). Keep the GNU/Clang flags for every other toolchain.
if(MSVC)
  add_compile_options(/W4 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  set(HARP_MATH_LIB "")
else()
  add_compile_options(-Wall -Wextra)
  set(HARP_MATH_LIB m)
endif()

# Platform shim (harp/plat.h): monotonic clock, hi-res sleep, UTC breakdown.
# Compiled into the host/shell targets — kept out of dependency-free harpcore.
if(WIN32)
  set(HARP_PLAT_SRC core/src/plat_win.c)
  set(HARP_PLAT_LIBS winmm)
else()
  set(HARP_PLAT_SRC core/src/plat_posix.c)
  set(HARP_PLAT_LIBS "")
endif()

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

# harp-deviced is the reference device daemon — it runs on the Pi (FunctionFS
# gadget) or any POSIX host (TCP simulator). It is pervasively pthread/POSIX and
# the real device is the Pi, so it is not part of the Windows build.
if(NOT WIN32)
  find_package(Threads REQUIRED)
  add_executable(harp-deviced
    device/harp-deviced.c # main + transports
    device/engine.c       # synth, event queue, audio threads
    device/state.c        # engine <-> content-addressed objects
    device/session.c      # protocol handlers + session loop
    device/panel.c        # front-panel unix-socket API
  )
  target_link_libraries(harp-deviced harpcore ${HARP_MATH_LIB} Threads::Threads)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(harp-deviced PRIVATE device/ffs.c) # USB gadget transport (§4.3)
  endif()
endif()

add_executable(harp-probe host/harp-probe.c host/client.c ${HARP_PLAT_SRC})
target_link_libraries(harp-probe harpcore ${HARP_MATH_LIB} ${HARP_PLAT_LIBS})
if(WIN32)
  target_link_libraries(harp-probe ws2_32) # Winsock for the TCP dev transport
endif()

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBUSB libusb-1.0)
endif()
if(NOT LIBUSB_FOUND)
  # No pkg-config (typical on Windows/MSVC): try a vendored prebuilt drop under
  # external/libusb-win, then any system install. Sets the same LIBUSB_* vars.
  # harp-probe (CLI) links the import lib and ships libusb-1.0.dll next to the
  # exe; the VST3 shell statically links libusb (tools/vst3-host) so the plugin
  # bundle is self-contained — that's why it hints a different MS64 sub-dir.
  find_path(LIBUSB_INCLUDE_DIRS
    NAMES libusb.h
    PATH_SUFFIXES libusb-1.0
    HINTS ${CMAKE_SOURCE_DIR}/external/libusb-win/include)
  find_library(LIBUSB_LIBRARIES
    NAMES libusb-1.0 usb-1.0
    HINTS ${CMAKE_SOURCE_DIR}/external/libusb-win/VS2022/MS64/dll
          ${CMAKE_SOURCE_DIR}/external/libusb-win/MS64/dll)
  find_file(HARP_LIBUSB_DLL
    NAMES libusb-1.0.dll
    HINTS ${CMAKE_SOURCE_DIR}/external/libusb-win/VS2022/MS64/dll
          ${CMAKE_SOURCE_DIR}/external/libusb-win/MS64/dll)
  if(LIBUSB_INCLUDE_DIRS AND LIBUSB_LIBRARIES)
    set(LIBUSB_FOUND TRUE)
  endif()
endif()
if(LIBUSB_FOUND)
  target_sources(harp-probe PRIVATE host/usb_io.c)
  target_compile_definitions(harp-probe PRIVATE HAVE_LIBUSB)
  target_include_directories(harp-probe PRIVATE ${LIBUSB_INCLUDE_DIRS})
  if(LIBUSB_LIBRARY_DIRS)
    target_link_directories(harp-probe PRIVATE ${LIBUSB_LIBRARY_DIRS})
  endif()
  target_link_libraries(harp-probe ${LIBUSB_LIBRARIES})
  message(STATUS "harp-probe: libusb found, USB transport enabled")
  # Windows: drop the libusb DLL next to the exe so it runs from the build tree.
  if(WIN32 AND HARP_LIBUSB_DLL)
    add_custom_command(TARGET harp-probe POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${HARP_LIBUSB_DLL}" "$<TARGET_FILE_DIR:harp-probe>")
  endif()
endif()

enable_testing()
add_executable(harp-tests tests/harp_tests.c)
target_link_libraries(harp-tests harpcore ${HARP_MATH_LIB})
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
