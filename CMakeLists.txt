cmake_minimum_required(VERSION 3.20)
project(Telegram LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CURL_SOURCE_DIR "" CACHE PATH "Path to a curl source tree for a self-contained build")

if (CURL_SOURCE_DIR)
  set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CURL_STATIC_CRT ON CACHE BOOL "" FORCE)
  set(CURL_TARGET_WINDOWS_VERSION "0x0601" CACHE STRING "" FORCE)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
  set(CURL_ZLIB OFF CACHE STRING "" FORCE)
  set(CURL_BROTLI OFF CACHE STRING "" FORCE)
  set(CURL_ZSTD OFF CACHE STRING "" FORCE)
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(HTTP_ONLY ON CACHE BOOL "" FORCE)
  add_subdirectory("${CURL_SOURCE_DIR}" curl-build)
  set(TELEGRAM_CURL_TARGET libcurl)
else()
  find_package(CURL REQUIRED)
  set(TELEGRAM_CURL_TARGET CURL::libcurl)
endif()

add_library(reaper_Telegram SHARED src/telegram.cpp)
target_include_directories(reaper_Telegram PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(reaper_Telegram PRIVATE "${TELEGRAM_CURL_TARGET}")

if (WIN32)
  target_compile_definitions(reaper_Telegram PRIVATE
    _WIN32_WINNT=0x0601
    WINVER=0x0601
    NOMINMAX
  )
  target_link_libraries(reaper_Telegram PRIVATE ws2_32 crypt32 wldap32 normaliz)
endif()

set_target_properties(reaper_Telegram PROPERTIES
  PREFIX ""
  SUFFIX ".dll"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)
