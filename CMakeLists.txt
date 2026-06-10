cmake_minimum_required(VERSION 3.16)
project(harp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

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
add_executable(harp-deviced device/harp-deviced.c)
target_link_libraries(harp-deviced harpcore m Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(harp-deviced PRIVATE device/ffs.c) # USB gadget transport (§4.3)
endif()

add_executable(harp-probe host/harp-probe.c)
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
