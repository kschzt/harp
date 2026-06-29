# harp-vst3-host — minimal CLI VST3 host for automated, agent-driven testing.
# Builds the VST3 SDK (hosting parts + example plugins for self-validation).
cmake_minimum_required(VERSION 3.20)
project(harp-vst3-host VERSION 0.1.0 LANGUAGES CXX C)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11) # host/freerun.c uses C11 <stdatomic.h>; matches the top-level
set(CMAKE_C_STANDARD_REQUIRED ON) # build. gcc/clang default to C11+; MSVC needs /std:c11.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS) # harp C sources use getenv/snprintf
endif()
# host/freerun.c uses C11 <stdatomic.h>, which MSVC gates behind /experimental:c11atomics
# (else "C atomic support is not enabled" even under /std:c11). The flag must land on
# THAT file specifically: a directory/target COMPILE_LANGUAGE genexp and CMAKE_C_FLAGS
# both fail to reach a C file inside the C++-dominant shell target under the VS
# generator, but a per-source COMPILE_OPTIONS is emitted into its own <AdditionalOptions>.
# Set once here (directory-scoped) so all five runtime targets that compile freerun.c
# pick it up. /std:c11 itself arrives via CMAKE_C_STANDARD (handled per-language).
if(MSVC)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../../host/freerun.c
    PROPERTIES COMPILE_OPTIONS "/experimental:c11atomics")
endif()

set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES ON CACHE BOOL "" FORCE) # adelay = test subject
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE) # no ~/Library symlink spam
# Skip the SDK's post-build validator: it loads the plugin before our libusb DLL
# is copied beside it, and we validate the shell with harp-vst3-host + a real DAW.
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk vst3sdk EXCLUDE_FROM_ALL)

set(SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk)
add_executable(harp-vst3-host main.cpp
  ${SDK_ROOT}/public.sdk/source/common/memorystream.cpp
  ${SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp)
if(APPLE)
  target_sources(harp-vst3-host PRIVATE ${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm)
  set_source_files_properties(${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
elseif(WIN32)
  target_sources(harp-vst3-host PRIVATE ${SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp)
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
set_tests_properties(rings PROPERTIES LABELS unit)

# ---- the HARP shell plugin (shell/) — needs libusb ----
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBUSB libusb-1.0)
endif()
if(NOT LIBUSB_FOUND)
  # No pkg-config (typical on Windows/MSVC): vendored prebuilt under
  # external/libusb-win. Use the STATIC lib so the plugin is fully self-contained
  # — no libusb DLL to place beside the plugin, the host, AND the SDK's
  # moduleinfo/validator tools (all of which LoadLibrary() the plugin at build).
  set(_HARP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  find_path(LIBUSB_INCLUDE_DIRS NAMES libusb.h PATH_SUFFIXES libusb-1.0
    HINTS ${_HARP_ROOT}/external/libusb-win/include)
  find_library(LIBUSB_LIBRARIES NAMES libusb-1.0 usb-1.0
    HINTS ${_HARP_ROOT}/external/libusb-win/VS2022/MS64/static
          ${_HARP_ROOT}/external/libusb-win/MS64/static)
  if(LIBUSB_INCLUDE_DIRS AND LIBUSB_LIBRARIES)
    set(LIBUSB_FOUND TRUE)
  endif()
endif()

# ---- libsamplerate (host/freerun.c — §8.7 ASRC for a device that can't rate-lock) ----
# System lib via pkg-config where present (macOS/Linux); else build it from source
# (pinned 0.2.2), so Windows and a bare runner work with no host package. The harp_asrc
# INTERFACE target carries the include dir + the lib so each runtime target just links
# it. Unconditional: the ASRC fallback path is compiled into every shell.
find_package(PkgConfig QUIET) # idempotent; Linux/macOS have it, Windows does not
if(PKG_CONFIG_FOUND)
  pkg_check_modules(SAMPLERATE samplerate)
endif()
if(NOT SAMPLERATE_FOUND)
  include(FetchContent)
  set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5) # libsamplerate 0.2.2 ships a pre-3.5 cmake_minimum_required
  FetchContent_Declare(libsamplerate
    GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
    GIT_TAG 0.2.2 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(libsamplerate)
  set(SAMPLERATE_LIBRARIES SampleRate::samplerate)
endif()
add_library(harp_asrc INTERFACE)
target_link_libraries(harp_asrc INTERFACE ${SAMPLERATE_LIBRARIES})
if(SAMPLERATE_INCLUDE_DIRS)
  target_include_directories(harp_asrc INTERFACE ${SAMPLERATE_INCLUDE_DIRS})
endif()
if(SAMPLERATE_LIBRARY_DIRS)
  target_link_directories(harp_asrc INTERFACE ${SAMPLERATE_LIBRARY_DIRS})
endif()

# §4.4.3/§6.1 shell-side mDNS discovery (selectDevice browses `_harp._tcp`). A small static
# lib wraps host/mdns.c with the dns_sd link so every runtime target links it uniformly:
# dns_sd is native on macOS (libSystem), avahi-compat's libdns_sd on Linux; where neither is
# present (Windows, a bare runner) harp_mdns_discover() is a -1 stub and the shell falls back
# to USB / an explicit HARP_ETH_DEVICE. tools/vst3-host is a separate project(), so it needs
# its OWN check_include_file — the root CMakeLists' result is not inherited here.
include(CheckIncludeFile)
check_include_file(dns_sd.h HARP_VST3_HAVE_DNS_SD_HDR)
add_library(harp_mdns STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../../host/mdns.c)
target_include_directories(harp_mdns PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../host)
if(HARP_VST3_HAVE_DNS_SD_HDR)
  if(APPLE)
    target_compile_definitions(harp_mdns PRIVATE HAVE_DNS_SD)
    message(STATUS "harp_mdns: dns_sd (macOS native), shell mDNS discovery enabled")
  else()
    find_library(HARP_VST3_DNSSD_LIB dns_sd)
    if(HARP_VST3_DNSSD_LIB)
      target_compile_definitions(harp_mdns PRIVATE HAVE_DNS_SD)
      target_link_libraries(harp_mdns PUBLIC ${HARP_VST3_DNSSD_LIB})
      message(STATUS "harp_mdns: dns_sd (${HARP_VST3_DNSSD_LIB}), shell mDNS discovery enabled")
    endif()
  endif()
endif()

if(LIBUSB_FOUND)
  set(HARP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  # Platform shim (harp/plat.h) compiled into the shell/AU like the core .c files.
  if(WIN32)
    set(HARP_PLAT_SRC ${HARP_ROOT}/core/src/plat_win.c)
    # ws2_32: the host stack (host/sock_io.c) is Winsock; harp-shell/clap get it
    # transitively via the VST3 `sdk` target, but a host-stack exe that doesn't link
    # `sdk` (registry-tests) needs it here — it's a plat lib the host stack requires.
    set(HARP_PLAT_LIBS winmm ws2_32)
  else()
    set(HARP_PLAT_SRC ${HARP_ROOT}/core/src/plat_posix.c)
    set(HARP_PLAT_LIBS "")
  endif()
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
    ${HARP_ROOT}/shell/runtime_registry.cpp
    ${HARP_ROOT}/host/usb_io.c
    ${HARP_ROOT}/host/sock_io.c
    ${HARP_ROOT}/host/rtp.c
    ${HARP_ROOT}/host/freerun.c
    ${HARP_ROOT}/host/client.c
    ${HARP_ROOT}/core/src/cbor.c
    ${HARP_ROOT}/core/src/sha256.c
    ${HARP_ROOT}/core/src/frame.c
    ${HARP_ROOT}/core/src/object.c
    ${HARP_ROOT}/core/src/store.c
    ${HARP_ROOT}/core/src/envelope.c
    ${HARP_ROOT}/core/src/link.c
    ${HARP_ROOT}/core/src/audio.c
    ${HARP_PLAT_SRC}
  )
  target_include_directories(harp-shell PRIVATE
    ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
    ${LIBUSB_INCLUDE_DIRS})
  target_compile_definitions(harp-shell PRIVATE HAVE_LIBUSB)
  target_link_directories(harp-shell PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(harp-shell PRIVATE sdk ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS} harp_asrc harp_mdns)
  if(WIN32)
    # static libusb pulls in the Win device-enumeration + COM GUID APIs; ws2_32 is
    # Winsock for the §8.7 Ethernet control + RTP sockets (host/sock_io.c +
    # shell/eth_transport.h — added with the EthTransport binding).
    target_link_libraries(harp-shell PRIVATE setupapi ole32 ws2_32)
  endif()
  message(STATUS "harp-shell: building VST3 shell with libusb")

  # ---- the HARP FX shell (shell/fx_plugin.cpp) — §8.8 audio.fx EFFECT variant ----
  # A SEPARATE VST3 (its own factory + frozen UIDs, category kFxReverb) built from
  # the SAME embedded HarpRuntime as the instrument shell. The ONLY source difference
  # is fx_plugin.cpp in place of plugin.cpp: it adds a stereo INPUT bus and opts in to
  # the runtime's §8.8 host->device input path (setFxInputSlots/writeFxInput) to route
  # the track audio H->D to an `audio.fx` device (harp-fx-deviced) and return the wet.
  # The instrument shell (plugin.cpp) and its golden test are untouched.
  smtg_add_vst3plugin(harp-fx-shell
    ${SHELL_MAIN}
    ${HARP_ROOT}/shell/fx_plugin.cpp
    ${HARP_ROOT}/shell/runtime.cpp
    ${HARP_ROOT}/shell/runtime_registry.cpp
    ${HARP_ROOT}/host/usb_io.c
    ${HARP_ROOT}/host/sock_io.c
    ${HARP_ROOT}/host/rtp.c
    ${HARP_ROOT}/host/freerun.c
    ${HARP_ROOT}/host/client.c
    ${HARP_ROOT}/core/src/cbor.c
    ${HARP_ROOT}/core/src/sha256.c
    ${HARP_ROOT}/core/src/frame.c
    ${HARP_ROOT}/core/src/object.c
    ${HARP_ROOT}/core/src/store.c
    ${HARP_ROOT}/core/src/envelope.c
    ${HARP_ROOT}/core/src/link.c
    ${HARP_ROOT}/core/src/audio.c
    ${HARP_PLAT_SRC}
  )
  target_include_directories(harp-fx-shell PRIVATE
    ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
    ${LIBUSB_INCLUDE_DIRS})
  target_compile_definitions(harp-fx-shell PRIVATE HAVE_LIBUSB)
  target_link_directories(harp-fx-shell PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(harp-fx-shell PRIVATE sdk ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS} harp_asrc harp_mdns)
  if(WIN32)
    target_link_libraries(harp-fx-shell PRIVATE setupapi ole32 ws2_32)
  endif()
  if(APPLE)
    # same re-seal rationale as harp-shell (moduleinfotool breaks the link seal)
    add_custom_command(TARGET harp-fx-shell POST_BUILD
      COMMAND codesign --force --deep -s -
              "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/harp-fx-shell.vst3"
      COMMENT "Re-sealing harp-fx-shell.vst3 signature")
  endif()
  message(STATUS "harp-fx-shell: building §8.8 audio.fx EFFECT VST3 variant")

  # ---- harp-runtime-probe: headless HarpRuntime driver (test tool, no VST3 host) ----
  # Drives the SAME runtime core Live uses (connect, audio.start, note, pullAudio) so the
  # shell's eth audio path can be tested end-to-end without a DAW.
  find_package(Threads REQUIRED)
  add_executable(harp-runtime-probe
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime-probe.cpp
    ${HARP_ROOT}/shell/runtime.cpp
    ${HARP_ROOT}/shell/runtime_registry.cpp
    ${HARP_ROOT}/host/usb_io.c
    ${HARP_ROOT}/host/sock_io.c
    ${HARP_ROOT}/host/rtp.c
    ${HARP_ROOT}/host/freerun.c
    ${HARP_ROOT}/host/client.c
    ${HARP_ROOT}/core/src/cbor.c
    ${HARP_ROOT}/core/src/sha256.c
    ${HARP_ROOT}/core/src/frame.c
    ${HARP_ROOT}/core/src/object.c
    ${HARP_ROOT}/core/src/store.c
    ${HARP_ROOT}/core/src/envelope.c
    ${HARP_ROOT}/core/src/link.c
    ${HARP_ROOT}/core/src/audio.c
    ${HARP_PLAT_SRC})
  target_include_directories(harp-runtime-probe PRIVATE
    ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell ${LIBUSB_INCLUDE_DIRS})
  target_compile_definitions(harp-runtime-probe PRIVATE HAVE_LIBUSB)
  target_link_directories(harp-runtime-probe PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(harp-runtime-probe PRIVATE ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS} harp_asrc harp_mdns Threads::Threads)

  # ---- the HARP CLAP shell (shell/clap) — the SAME runtime core, CLAP-only ----
  # A third shell over HarpRuntime (no VST3 SDK; CLAP is a header-only ABI). Built
  # as a MODULE named harp-clap.clap that the host dlopen()s. CLAP's per-note
  # PARAM_MOD maps straight to §9.5 per-voice modulation (shell/clap/harp_clap.cpp).
  add_library(harp-clap MODULE
    ${HARP_ROOT}/shell/clap/harp_clap.cpp
    ${HARP_ROOT}/shell/runtime.cpp
    ${HARP_ROOT}/shell/runtime_registry.cpp
    ${HARP_ROOT}/host/usb_io.c
    ${HARP_ROOT}/host/sock_io.c
    ${HARP_ROOT}/host/rtp.c
    ${HARP_ROOT}/host/freerun.c
    ${HARP_ROOT}/host/client.c
    ${HARP_ROOT}/core/src/cbor.c
    ${HARP_ROOT}/core/src/sha256.c
    ${HARP_ROOT}/core/src/frame.c
    ${HARP_ROOT}/core/src/object.c
    ${HARP_ROOT}/core/src/store.c
    ${HARP_ROOT}/core/src/envelope.c
    ${HARP_ROOT}/core/src/link.c
    ${HARP_ROOT}/core/src/audio.c
    ${HARP_PLAT_SRC})
  target_include_directories(harp-clap PRIVATE
    ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
    ${HARP_ROOT}/external/clap/include ${LIBUSB_INCLUDE_DIRS})
  target_compile_definitions(harp-clap PRIVATE HAVE_LIBUSB)
  target_link_directories(harp-clap PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(harp-clap PRIVATE ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS} harp_asrc harp_mdns)
  if(APPLE)
    # macOS DAWs load a .clap as a CFBundle (Contents/MacOS/<exe> + Info.plist),
    # exactly like the AU .component below — NOT a flat dylib. Mirror harp-au.
    set_target_properties(harp-clap PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION "clap"
      OUTPUT_NAME "harp-clap"
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/../clap-host/clap-info.plist.in)
    target_link_libraries(harp-clap PRIVATE # libusb on macOS pulls these in
      "-framework CoreFoundation" "-framework IOKit")
    # re-seal the bundle (like harp-shell.vst3 / harp-au): a broken/absent seal
    # makes the hardened loader / some DAWs refuse it.
    add_custom_command(TARGET harp-clap POST_BUILD
      COMMAND codesign --force -s - "$<TARGET_BUNDLE_DIR:harp-clap>"
      COMMENT "Ad-hoc signing harp-clap.clap")
  else()
    # Linux/Windows: a .clap is a plain shared library (no bundle).
    set_target_properties(harp-clap PROPERTIES PREFIX "" OUTPUT_NAME "harp-clap" SUFFIX ".clap")
  endif()
  if(WIN32) # libusb -> Win device-enum + COM GUID APIs; ws2_32 -> §8.7 Ethernet sockets (as harp-shell)
    target_link_libraries(harp-clap PRIVATE setupapi ole32 ws2_32)
  endif()
  message(STATUS "harp-clap: building CLAP shell with libusb")

  # ---- install-clap: stage the .clap in the OS plugin dir for a real DAW ----
  # (the clap-test gate runs against the build tree, not this; this is for DAWs.)
  if(APPLE)
    set(HARP_CLAP_DEST "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP")
  elseif(WIN32)
    set(HARP_CLAP_DEST "$ENV{CommonProgramW6432}/CLAP")
  else()
    set(HARP_CLAP_DEST "$ENV{HOME}/.clap")
  endif()
  if(NOT HARP_CLAP_DEST OR HARP_CLAP_DEST MATCHES "^/+CLAP$" OR HARP_CLAP_DEST STREQUAL "/.clap")
    set(HARP_CLAP_DEST "${CMAKE_BINARY_DIR}/dist/CLAP") # env unset — never write to root
  endif()
  if(APPLE) # the bundle is a directory: copy it whole, then re-sign (copy can break the seal)
    add_custom_target(install-clap
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${HARP_CLAP_DEST}/harp-clap.clap"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${HARP_CLAP_DEST}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:harp-clap>"
              "${HARP_CLAP_DEST}/harp-clap.clap"
      COMMAND codesign --force -s - "${HARP_CLAP_DEST}/harp-clap.clap"
      DEPENDS harp-clap
      COMMENT "Installing harp-clap.clap -> ${HARP_CLAP_DEST}")
  else() # bare shared library: a single file (rm the FILE, never rm -rf the shared dir)
    add_custom_target(install-clap
      COMMAND ${CMAKE_COMMAND} -E rm -f "${HARP_CLAP_DEST}/harp-clap.clap"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${HARP_CLAP_DEST}"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:harp-clap>"
              "${HARP_CLAP_DEST}/harp-clap.clap"
      DEPENDS harp-clap
      COMMENT "Installing harp-clap.clap -> ${HARP_CLAP_DEST}")
  endif()

  # ---- clap-host: minimal CLI CLAP host (the CLAP analogue of harp-vst3-host) ----
  # dlopen()s the .clap, drives notes/params/per-note PARAM_MOD, hashes the owner
  # main mix — the byte-exact golden gate for the CLAP shell. No runtime link
  # (the runtime lives inside the .clap); just the CLAP headers + dlopen.
  add_executable(clap-host ${CMAKE_CURRENT_SOURCE_DIR}/../clap-host/main.cpp)
  target_include_directories(clap-host PRIVATE
    ${HARP_ROOT}/external/clap/include ${HARP_ROOT}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..) # tools/ -> render_check.h (shared harp_fnv1a)
  target_link_libraries(clap-host PRIVATE ${CMAKE_DL_LIBS})

  # tsan-host (debt #17): the shell runtime, statically linked into a TSan
  # main image and driven directly (no plugin boundary — TSan must be in
  # the main image, not a dlopen'd bundle). Built only with -DHARP_TSAN=ON;
  # run against a real device via scripts/tsan-shell.sh.
  option(HARP_TSAN "Build tsan-host: ThreadSanitize the shell runtime" OFF)
  # The ThreadSanitizer instrumentation is separable from the multi-instance
  # harness itself. session-share-test.sh builds this SAME host with
  # -DHARP_TSAN_SANITIZE=OFF — it checks session SHARING (one claim for N
  # instances), not race-freedom, and a TSan binary aborts at startup on recent
  # Linux kernels (the vm.mmap_rnd_bits ASLR mapping issue), which has no place
  # in a plain hardware test. tsan-shell.sh keeps it ON for race detection.
  option(HARP_TSAN_SANITIZE "Instrument tsan-host with ThreadSanitizer" ON)
  if(HARP_TSAN)
    add_executable(tsan-host
      ${CMAKE_CURRENT_SOURCE_DIR}/../tsan-host/main.cpp
      ${HARP_ROOT}/shell/runtime.cpp
      ${HARP_ROOT}/shell/runtime_registry.cpp
      ${HARP_ROOT}/host/usb_io.c
      ${HARP_ROOT}/host/sock_io.c
      ${HARP_ROOT}/host/rtp.c
      ${HARP_ROOT}/host/freerun.c
      ${HARP_ROOT}/host/client.c
      ${HARP_ROOT}/core/src/cbor.c
      ${HARP_ROOT}/core/src/sha256.c
      ${HARP_ROOT}/core/src/frame.c
      ${HARP_ROOT}/core/src/object.c
      ${HARP_ROOT}/core/src/store.c
      ${HARP_ROOT}/core/src/envelope.c
      ${HARP_ROOT}/core/src/link.c
      ${HARP_ROOT}/core/src/audio.c
      ${HARP_PLAT_SRC})
    target_include_directories(tsan-host PRIVATE
      ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
      ${CMAKE_CURRENT_SOURCE_DIR}/.. ${LIBUSB_INCLUDE_DIRS})
    target_compile_definitions(tsan-host PRIVATE HAVE_LIBUSB)
    if(HARP_TSAN_SANITIZE)
      target_compile_options(tsan-host PRIVATE -fsanitize=thread -g -O1)
      target_link_options(tsan-host PRIVATE -fsanitize=thread)
    else()
      target_compile_options(tsan-host PRIVATE -g -O1)
    endif()
    if(LIBUSB_LIBRARY_DIRS)
      target_link_directories(tsan-host PRIVATE ${LIBUSB_LIBRARY_DIRS})
    endif()
    target_link_libraries(tsan-host ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS} harp_asrc harp_mdns)
    message(STATUS "tsan-host: multi-instance shell-runtime harness (TSan: ${HARP_TSAN_SANITIZE})")
  endif()

  # registry-tests (P4): the process-global runtime registry's keying +
  # refcount logic (shell/runtime_registry.*), unit-tested WITHOUT a device.
  # The registry never start()s a runtime, and an un-started HarpRuntime opens
  # no USB and spawns no threads (ctor = on-disk store only; stop()/dtor early-
  # return), so acquire()/release() exercise the table bookkeeping against real
  # HarpRuntime objects with zero hardware — no test hook in shell/* needed.
  # It still links runtime.cpp (the registry news/deletes it) and thus the C
  # core/host sources + libusb runtime.cpp references, so it lives here in the
  # LIBUSB_FOUND block beside tsan-host rather than in the harpcore-only
  # harp-tests. Runs in plain CI — it is the cloud-side complement to the
  # session-share HARDWARE test, which proves the same sharing on the bus.
  # ALL 3 OSes (incl. MSVC): the source list below is exactly what harp-shell
  # compiles+links under MSVC — runtime.cpp + the host stack + harp_asrc, whose
  # libsamplerate is FetchContent'd on Windows (see above) — and freerun.c's
  # /experimental:c11atomics flag is a directory-scoped source property that applies
  # to this target too, so it builds on Windows with no extra wiring. This is the
  # ONLY place the registry keying/refcount runs under MSVC, whose process-global
  # threading model is the most divergent of the three — exactly where a
  # Windows-specific session-sharing bug could otherwise hide. (The hermetic
  # temp-HOME's mkdtemp/setenv are #ifndef _WIN32'd; on Windows the default store
  # dir is used — harmless on CI, and the test never reads the store.)
  add_executable(registry-tests
    ${HARP_ROOT}/tests/registry_tests.cpp
    ${HARP_ROOT}/shell/runtime.cpp
    ${HARP_ROOT}/shell/runtime_registry.cpp
    ${HARP_ROOT}/host/usb_io.c
    ${HARP_ROOT}/host/sock_io.c
    ${HARP_ROOT}/host/rtp.c
    ${HARP_ROOT}/host/freerun.c
    ${HARP_ROOT}/host/client.c
    ${HARP_ROOT}/core/src/cbor.c
    ${HARP_ROOT}/core/src/sha256.c
    ${HARP_ROOT}/core/src/frame.c
    ${HARP_ROOT}/core/src/object.c
    ${HARP_ROOT}/core/src/store.c
    ${HARP_ROOT}/core/src/envelope.c
    ${HARP_ROOT}/core/src/link.c
    ${HARP_ROOT}/core/src/audio.c
    ${HARP_PLAT_SRC})
  target_include_directories(registry-tests PRIVATE
    ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
    ${LIBUSB_INCLUDE_DIRS})
  target_compile_definitions(registry-tests PRIVATE HAVE_LIBUSB)
  target_link_directories(registry-tests PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(registry-tests ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS} harp_asrc harp_mdns)
  add_test(NAME registry COMMAND registry-tests)
  set_tests_properties(registry PROPERTIES LABELS unit)
  message(STATUS "registry-tests: runtime registry keying/refcount unit test")

  # runtime-units-tests (audit #2): the shell runtime's host-side FUNCTIONAL core —
  # the param/ramp/note/mod event QUEUE logic + §9.4 per-part channel targeting, the
  # NoteVoiceMap (host noteId -> §9.5 voice key), and setStateBundle malformed-bundle
  # REJECTION — all exercised on a constructed HarpRuntime with NO start()/device, the
  # same hermetic zero-hardware seam registry-tests uses (queue* push to the owner ring,
  # the test drains it). harp_voice_pick is already in harp-engine-logic-tests; not here.
  # Same source set + 3-OS reach as registry-tests above.
  add_executable(runtime-units-tests
    ${HARP_ROOT}/tests/runtime_units_tests.cpp
    ${HARP_ROOT}/shell/runtime.cpp
    ${HARP_ROOT}/shell/runtime_registry.cpp
    ${HARP_ROOT}/host/usb_io.c
    ${HARP_ROOT}/host/sock_io.c
    ${HARP_ROOT}/host/rtp.c
    ${HARP_ROOT}/host/freerun.c
    ${HARP_ROOT}/host/client.c
    ${HARP_ROOT}/core/src/cbor.c
    ${HARP_ROOT}/core/src/sha256.c
    ${HARP_ROOT}/core/src/frame.c
    ${HARP_ROOT}/core/src/object.c
    ${HARP_ROOT}/core/src/store.c
    ${HARP_ROOT}/core/src/envelope.c
    ${HARP_ROOT}/core/src/link.c
    ${HARP_ROOT}/core/src/audio.c
    ${HARP_PLAT_SRC})
  target_include_directories(runtime-units-tests PRIVATE
    ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
    ${LIBUSB_INCLUDE_DIRS})
  target_compile_definitions(runtime-units-tests PRIVATE HAVE_LIBUSB)
  target_link_directories(runtime-units-tests PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(runtime-units-tests ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS} harp_asrc harp_mdns)
  add_test(NAME runtime_units COMMAND runtime-units-tests)
  set_tests_properties(runtime_units PROPERTIES LABELS unit)
  message(STATUS "runtime-units-tests: shell runtime queue/channel/NoteVoiceMap/bundle unit test")

  # SMTG emits the bundle under VST3/<config>/ ($<CONFIG> = multi-config safe).
  set(HARP_SHELL_BUNDLE ${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/harp-shell.vst3)
  if(APPLE)
    # SMTG signs at link, then moduleinfotool rewrites Resources and breaks the
    # seal ("code has no resources but signature indicates they must be present"
    # — Live refuses such bundles). Re-sign as the last step.
    add_custom_command(TARGET harp-shell POST_BUILD
      COMMAND codesign --force --deep -s - "${HARP_SHELL_BUNDLE}"
      COMMENT "Re-sealing harp-shell.vst3 signature")
  endif()

  # `cmake --build ... --target install-live` -> the OS VST3 folder. Uses
  # cmake -E (cross-platform) instead of rm/cp/mkdir. On Windows the system
  # VST3 dir lives under Program Files, so this may need an elevated shell.
  if(WIN32)
    set(HARP_VST3_DEST "$ENV{CommonProgramW6432}/VST3")
  elseif(APPLE)
    set(HARP_VST3_DEST "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
  else()
    set(HARP_VST3_DEST "$ENV{HOME}/.vst3")
  endif()
  if(NOT HARP_VST3_DEST OR HARP_VST3_DEST MATCHES "^/+VST3$")
    # the env var was unset — fall back to a build-local dir rather than risk
    # `rm -rf` against a bogus root path.
    set(HARP_VST3_DEST "${CMAKE_BINARY_DIR}/dist/VST3")
  endif()
  add_custom_target(install-live
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${HARP_VST3_DEST}/harp-shell.vst3"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HARP_VST3_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${HARP_SHELL_BUNDLE}" "${HARP_VST3_DEST}/harp-shell.vst3"
    DEPENDS harp-shell
    COMMENT "Installing harp-shell.vst3 to ${HARP_VST3_DEST}")
  if(APPLE)
    add_custom_command(TARGET install-live POST_BUILD
      COMMAND codesign --force --deep -s - "${HARP_VST3_DEST}/harp-shell.vst3")
  endif()
  # Back-compat alias referenced by docs/CI.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_target(install-linux DEPENDS install-live)
  endif()

  # `--target install-fx-live` -> the OS VST3 folder for the §8.8 FX variant.
  add_custom_target(install-fx-live
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${HARP_VST3_DEST}/harp-fx-shell.vst3"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HARP_VST3_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/harp-fx-shell.vst3"
            "${HARP_VST3_DEST}/harp-fx-shell.vst3"
    DEPENDS harp-fx-shell
    COMMENT "Installing harp-fx-shell.vst3 to ${HARP_VST3_DEST}")
  if(APPLE)
    add_custom_command(TARGET install-fx-live POST_BUILD
      COMMAND codesign --force --deep -s - "${HARP_VST3_DEST}/harp-fx-shell.vst3")
  endif()

  # ---- optional downstream shell variant (generic; no product strings here) ----
  # A product that ships this shell as its OWN plugin (own name/UIDs/param map/
  # device filter — see shell/shell_config.h) builds a SECOND VST3 from these SAME
  # sources by configuring with:
  #   -DHARP_SHELL_VARIANT_NAME=<target>      (CMake target + build-bundle name)
  #   -DHARP_SHELL_VARIANT_CONFIG=<config.h>  (header #defining HARP_SHELL_* macros)
  #   [-DHARP_SHELL_VARIANT_INCDIR=<dir>]     (dir of that header, on the include path)
  #   [-DHARP_SHELL_VARIANT_BUNDLE=<name>]    (installed bundle dir, default = NAME)
  # The config header lives OUTSIDE this repo, so harp carries no product-specific
  # strings — only this generic mechanism + the refdev default in shell_config.h.
  if(HARP_SHELL_VARIANT_NAME AND HARP_SHELL_VARIANT_CONFIG)
    smtg_add_vst3plugin(${HARP_SHELL_VARIANT_NAME}
      ${SHELL_MAIN}
      ${HARP_ROOT}/shell/plugin.cpp ${HARP_ROOT}/shell/runtime.cpp
      ${HARP_ROOT}/shell/runtime_registry.cpp
      ${HARP_ROOT}/host/usb_io.c ${HARP_ROOT}/host/sock_io.c ${HARP_ROOT}/host/rtp.c
      ${HARP_ROOT}/host/freerun.c ${HARP_ROOT}/host/client.c
      ${HARP_ROOT}/core/src/cbor.c ${HARP_ROOT}/core/src/sha256.c
      ${HARP_ROOT}/core/src/frame.c ${HARP_ROOT}/core/src/object.c
      ${HARP_ROOT}/core/src/store.c ${HARP_ROOT}/core/src/envelope.c
      ${HARP_ROOT}/core/src/link.c ${HARP_ROOT}/core/src/audio.c
      ${HARP_PLAT_SRC})
    target_include_directories(${HARP_SHELL_VARIANT_NAME} PRIVATE
      ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
      ${LIBUSB_INCLUDE_DIRS} ${HARP_SHELL_VARIANT_INCDIR})
    target_compile_definitions(${HARP_SHELL_VARIANT_NAME} PRIVATE HAVE_LIBUSB
      HARP_SHELL_CONFIG_HEADER="${HARP_SHELL_VARIANT_CONFIG}")
    target_link_directories(${HARP_SHELL_VARIANT_NAME} PRIVATE ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(${HARP_SHELL_VARIANT_NAME} PRIVATE sdk ${LIBUSB_LIBRARIES}
      ${HARP_PLAT_LIBS} harp_asrc harp_mdns)
    if(WIN32)
      target_link_libraries(${HARP_SHELL_VARIANT_NAME} PRIVATE setupapi ole32 ws2_32)
    endif()
    if(NOT HARP_SHELL_VARIANT_BUNDLE)
      set(HARP_SHELL_VARIANT_BUNDLE "${HARP_SHELL_VARIANT_NAME}")
    endif()
    message(STATUS "harp shell variant: ${HARP_SHELL_VARIANT_NAME} "
                   "-> '${HARP_SHELL_VARIANT_BUNDLE}.vst3' (config ${HARP_SHELL_VARIANT_CONFIG})")
    if(APPLE)
      add_custom_command(TARGET ${HARP_SHELL_VARIANT_NAME} POST_BUILD
        COMMAND codesign --force --deep -s - "$<TARGET_BUNDLE_DIR:${HARP_SHELL_VARIANT_NAME}>")
    endif()
    add_custom_target(install-${HARP_SHELL_VARIANT_NAME}
      COMMAND ${CMAKE_COMMAND} -E make_directory "${HARP_VST3_DEST}"
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${HARP_VST3_DEST}/${HARP_SHELL_VARIANT_BUNDLE}.vst3"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "$<TARGET_BUNDLE_DIR:${HARP_SHELL_VARIANT_NAME}>"
              "${HARP_VST3_DEST}/${HARP_SHELL_VARIANT_BUNDLE}.vst3"
      DEPENDS ${HARP_SHELL_VARIANT_NAME}
      COMMENT "Installing ${HARP_SHELL_VARIANT_BUNDLE}.vst3 to ${HARP_VST3_DEST}")
    if(APPLE)
      add_custom_command(TARGET install-${HARP_SHELL_VARIANT_NAME} POST_BUILD
        COMMAND codesign --force --deep -s - "${HARP_VST3_DEST}/${HARP_SHELL_VARIANT_BUNDLE}.vst3")
    endif()
  endif()

  # ---- the Audio Unit shell (macOS): same runtime, second format ----
  if(APPLE)
    add_library(harp-au MODULE
      ${HARP_ROOT}/shell/au/harp_au.mm
      ${HARP_ROOT}/shell/runtime.cpp
      ${HARP_ROOT}/shell/runtime_registry.cpp
      ${HARP_ROOT}/host/usb_io.c
      ${HARP_ROOT}/host/sock_io.c
      ${HARP_ROOT}/host/rtp.c
      ${HARP_ROOT}/host/freerun.c
      ${HARP_ROOT}/host/client.c
      ${HARP_ROOT}/core/src/cbor.c
      ${HARP_ROOT}/core/src/sha256.c
      ${HARP_ROOT}/core/src/frame.c
      ${HARP_ROOT}/core/src/object.c
      ${HARP_ROOT}/core/src/store.c
      ${HARP_ROOT}/core/src/envelope.c
      ${HARP_ROOT}/core/src/link.c
      ${HARP_ROOT}/core/src/audio.c
      ${HARP_PLAT_SRC})
    target_include_directories(harp-au PRIVATE
      ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
      ${LIBUSB_INCLUDE_DIRS})
    # os_workgroup_t must mean the same thing in runtime.cpp (C++) and
    # harp_au.mm (ObjC++): force the C-struct flavor of OS objects
    target_compile_definitions(harp-au PRIVATE HAVE_LIBUSB OS_OBJECT_USE_OBJC=0)
    target_link_directories(harp-au PRIVATE ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(harp-au PRIVATE ${LIBUSB_LIBRARIES} harp_asrc harp_mdns
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
  # Without libusb NONE of the shell targets (harp-shell, install-live, install-au,
  # install-clap, ...) are generated — so a later `--target install-live` fails with
  # the cryptic "project file does not exist". This build root exists ONLY to build
  # the shell, so fail fast at configure with the actual cause and the cure.
  message(FATAL_ERROR
    "libusb not found: the harp-shell plugin and its install-* targets cannot be "
    "built (this is why `cmake --build ... --target install-live` fails with "
    "\"project file does not exist\").\n"
    "  Windows: fetch the prebuilt MSVC libusb into external/libusb-win (README Path 3).\n"
    "  macOS:   brew install libusb\n"
    "  Linux:   apt install libusb-1.0-0-dev")
endif()
