cmake_minimum_required(VERSION 3.22)
project(LiveHost VERSION 0.3.0 LANGUAGES C CXX)

# Build label shown on the web page (CI passes the run number)
set(LIVEHOST_BUILD "dev" CACHE STRING "Build label")

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
# LiveHost serves plain HTTP only: keep httplib free of optional OpenSSL/zlib/Brotli dependencies
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(JUCE httplib)

find_package(Threads REQUIRED)

# GUI app target so plugin editor windows can be shown (on a virtual display).
# It still runs headless: windows are only created when the user asks for one.
juce_add_gui_app(LiveHost
  PRODUCT_NAME "LiveHost"
  COMPANY_NAME "Jerod Ulicki")

target_sources(LiveHost PRIVATE
  Source/Main.cpp
  Source/HostEngine.cpp
  Source/WebServer.cpp)

target_compile_definitions(LiveHost PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_VST=0
  JUCE_PLUGINHOST_LADSPA=0
  JUCE_ALSA=1
  JUCE_JACK=1
  JUCE_STRICT_REFCOUNTEDPOINTER=1
  LIVEHOST_BUILD_STR="${LIVEHOST_BUILD}")

target_link_libraries(LiveHost PRIVATE
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
  target_link_options(LiveHost PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Ship the web UI next to the binary
add_custom_command(TARGET LiveHost POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/web
          $<TARGET_FILE_DIR:LiveHost>/web)
