# harp-vst3-host — minimal CLI VST3 host for automated, agent-driven testing.
# Builds the VST3 SDK (hosting parts + example plugins for self-validation).
cmake_minimum_required(VERSION 3.20)
project(harp-vst3-host VERSION 0.1.0 LANGUAGES CXX C)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES ON CACHE BOOL "" FORCE) # adelay = test subject
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE) # no ~/Library symlink spam

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk vst3sdk EXCLUDE_FROM_ALL)

set(SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk)
add_executable(harp-vst3-host main.cpp
  ${SDK_ROOT}/public.sdk/source/common/memorystream.cpp
  ${SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp)
if(APPLE)
  target_sources(harp-vst3-host PRIVATE ${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm)
  set_source_files_properties(${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
elseif(UNIX)
  target_sources(harp-vst3-host PRIVATE ${SDK_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()
target_link_libraries(harp-vst3-host PRIVATE sdk_hosting sdk_common base pluginterfaces)
if(APPLE)
  target_link_libraries(harp-vst3-host PRIVATE "-framework CoreFoundation" "-framework Foundation")
endif()
target_include_directories(harp-vst3-host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ---- ring unit + stress tests (no SDK, no hardware) ----
add_executable(ring-tests ring_tests.cpp)
target_include_directories(ring-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../shell)
enable_testing()
add_test(NAME rings COMMAND ring-tests)

# ---- the HARP shell plugin (shell/) — needs libusb ----
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBUSB libusb-1.0)
endif()
if(LIBUSB_FOUND)
  set(HARP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  # VST3 plugin entry point is per-platform (macmain/linuxmain/dllmain).
  if(APPLE)
    set(SHELL_MAIN ${SDK_ROOT}/public.sdk/source/main/macmain.cpp)
  elseif(WIN32)
    set(SHELL_MAIN ${SDK_ROOT}/public.sdk/source/main/dllmain.cpp)
  else()
    set(SHELL_MAIN ${SDK_ROOT}/public.sdk/source/main/linuxmain.cpp)
  endif()
  smtg_add_vst3plugin(harp-shell
    ${SHELL_MAIN}
    ${HARP_ROOT}/shell/plugin.cpp
    ${HARP_ROOT}/shell/runtime.cpp
    ${HARP_ROOT}/host/usb_io.c
    ${HARP_ROOT}/host/client.c
    ${HARP_ROOT}/core/src/cbor.c
    ${HARP_ROOT}/core/src/sha256.c
    ${HARP_ROOT}/core/src/frame.c
    ${HARP_ROOT}/core/src/object.c
    ${HARP_ROOT}/core/src/store.c
    ${HARP_ROOT}/core/src/envelope.c
    ${HARP_ROOT}/core/src/link.c
    ${HARP_ROOT}/core/src/audio.c
  )
  target_include_directories(harp-shell PRIVATE
    ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
    ${LIBUSB_INCLUDE_DIRS})
  target_compile_definitions(harp-shell PRIVATE HAVE_LIBUSB)
  target_link_directories(harp-shell PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(harp-shell PRIVATE sdk ${LIBUSB_LIBRARIES})
  message(STATUS "harp-shell: building VST3 shell with libusb ${LIBUSB_VERSION}")

  # SMTG signs at link time, then moduleinfotool writes into Resources and
  # breaks the seal ("code has no resources but signature indicates they
  # must be present" — Live refuses such bundles). Re-sign as the last step.
  set(HARP_SHELL_BUNDLE ${CMAKE_BINARY_DIR}/VST3/Release/harp-shell.vst3)
  if(APPLE)
    add_custom_command(TARGET harp-shell POST_BUILD
      COMMAND codesign --force --deep -s - "${HARP_SHELL_BUNDLE}"
      COMMENT "Re-sealing harp-shell.vst3 signature")
  endif()

  # `cmake --build build-vst --target install-live` -> user VST3 folder
  add_custom_target(install-live
    COMMAND rm -rf "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/harp-shell.vst3"
    COMMAND cp -R "${HARP_SHELL_BUNDLE}" "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/"
    COMMAND codesign --force --deep -s -
            "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/harp-shell.vst3"
    DEPENDS harp-shell
    COMMENT "Installing harp-shell.vst3 for Ableton Live")

  # Linux: VST3 bundles land in ~/.vst3 (Bitwig, Reaper, Ardour pick it up)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_target(install-linux
      COMMAND rm -rf "$ENV{HOME}/.vst3/harp-shell.vst3"
      COMMAND mkdir -p "$ENV{HOME}/.vst3"
      COMMAND cp -R "${HARP_SHELL_BUNDLE}" "$ENV{HOME}/.vst3/"
      DEPENDS harp-shell
      COMMENT "Installing harp-shell.vst3 to ~/.vst3")
  endif()

  # ---- the Audio Unit shell (macOS): same runtime, second format ----
  if(APPLE)
    add_library(harp-au MODULE
      ${HARP_ROOT}/shell/au/harp_au.mm
      ${HARP_ROOT}/shell/runtime.cpp
      ${HARP_ROOT}/host/usb_io.c
      ${HARP_ROOT}/host/client.c
      ${HARP_ROOT}/core/src/cbor.c
      ${HARP_ROOT}/core/src/sha256.c
      ${HARP_ROOT}/core/src/frame.c
      ${HARP_ROOT}/core/src/object.c
      ${HARP_ROOT}/core/src/store.c
      ${HARP_ROOT}/core/src/envelope.c
      ${HARP_ROOT}/core/src/link.c
      ${HARP_ROOT}/core/src/audio.c)
    target_include_directories(harp-au PRIVATE
      ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
      ${LIBUSB_INCLUDE_DIRS})
    # os_workgroup_t must mean the same thing in runtime.cpp (C++) and
    # harp_au.mm (ObjC++): force the C-struct flavor of OS objects
    target_compile_definitions(harp-au PRIVATE HAVE_LIBUSB OS_OBJECT_USE_OBJC=0)
    target_link_directories(harp-au PRIVATE ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(harp-au PRIVATE ${LIBUSB_LIBRARIES}
      "-framework AudioToolbox" "-framework CoreFoundation" "-framework Foundation")
    set_target_properties(harp-au PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION "component"
      OUTPUT_NAME "harp-au"
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/../au-host/au-info.plist.in)

    add_executable(au-host ${CMAKE_CURRENT_SOURCE_DIR}/../au-host/main.mm)
    target_include_directories(au-host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(au-host PRIVATE
      "-framework AudioToolbox" "-framework CoreFoundation")

    add_custom_target(install-au
      COMMAND rm -rf "$ENV{HOME}/Library/Audio/Plug-Ins/Components/harp-au.component"
      COMMAND cp -R "$<TARGET_BUNDLE_DIR:harp-au>"
              "$ENV{HOME}/Library/Audio/Plug-Ins/Components/"
      COMMAND codesign --force --deep -s -
              "$ENV{HOME}/Library/Audio/Plug-Ins/Components/harp-au.component"
      DEPENDS harp-au
      COMMENT "Installing harp-au.component")
  endif()
else()
  message(STATUS "harp-shell: libusb not found, shell not built")
endif()
