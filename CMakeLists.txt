cmake_minimum_required(VERSION 3.22)
project(LivePi VERSION 0.3.0 LANGUAGES C CXX)

# Build label shown on the web page (CI passes the run number)
set(LIVEPI_BUILD "dev" CACHE STRING "Build label")
set(LIVEPI_REPO "leonidis2013/LivePi" CACHE STRING "GitHub repo that online updates come from")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.6
  GIT_SHALLOW ON)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.18.3
  GIT_SHALLOW ON)
# LivePi serves plain HTTP only: keep httplib free of optional OpenSSL/zlib/Brotli dependencies
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(JUCE httplib)

find_package(Threads REQUIRED)

# GUI app target so plugin editor windows can be shown (on a virtual display).
# It still runs headless: windows are only created when the user asks for one.
juce_add_gui_app(LivePi
  PRODUCT_NAME "LivePi"
  COMPANY_NAME "Jerod Ulicki")

target_sources(LivePi PRIVATE
  Source/Main.cpp
  Source/HostEngine.cpp
  Source/WebServer.cpp
  Source/LiveView.cpp)


target_compile_definitions(LivePi PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_VST=0
  JUCE_PLUGINHOST_LADSPA=0
  JUCE_ALSA=1
  JUCE_JACK=1
  JUCE_STRICT_REFCOUNTEDPOINTER=1
  LIVEPI_BUILD_STR="${LIVEPI_BUILD}"
  LIVEPI_REPO_STR="${LIVEPI_REPO}")

target_link_libraries(LivePi PRIVATE
  ${CMAKE_DL_LIBS}
  juce::juce_audio_utils
  juce::juce_audio_devices
  juce::juce_audio_processors
  juce::juce_cryptography
  juce::juce_gui_basics
  juce::juce_events
  juce::juce_recommended_config_flags
  httplib::httplib
  Threads::Threads)

# Avoid depending on the build machine's libstdc++ version
if(UNIX AND NOT APPLE)
  target_link_options(LivePi PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Release tuning for the Pi: whole-program optimisation, and code scheduled for the Pi 4/5 cores
# (armv8-a so it still runs on a Pi 3 / Zero 2 with a 64-bit OS).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT LIVEPI_IPO OUTPUT _ipo_msg)
  if(LIVEPI_IPO)
    set_property(TARGET LivePi PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
  target_compile_options(LivePi PRIVATE -O3 -march=armv8-a+crc -mtune=cortex-a76 -fno-plt -fomit-frame-pointer)
  target_link_options(LivePi PRIVATE -Wl,-O1 -Wl,--as-needed -Wl,--gc-sections)
  target_compile_options(LivePi PRIVATE -ffunction-sections -fdata-sections)
endif()

# Ship the web UI next to the binary
add_custom_command(TARGET LivePi POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/web
          $<TARGET_FILE_DIR:LivePi>/web)
