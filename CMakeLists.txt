cmake_minimum_required(VERSION 3.28)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(EbookReader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EB_CLANG_TIDY "Run clang-tidy during the build" OFF)
option(EB_ASAN "Build with AddressSanitizer" OFF)

if(EB_CLANG_TIDY)
  set(CMAKE_CXX_CLANG_TIDY clang-tidy --extra-arg=/EHsc)
endif()

add_library(eb_hardening INTERFACE)
target_compile_definitions(eb_hardening INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(eb_hardening INTERFACE
  /utf-8 /sdl /guard:cf
  $<$<CONFIG:Release>:/GL /Gy /Gw>
  $<$<BOOL:${EB_ASAN}>:/fsanitize=address>)
target_link_options(eb_hardening INTERFACE
  /guard:cf /CETCOMPAT
  $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
  $<$<BOOL:${EB_ASAN}>:/INCREMENTAL:NO>)

add_library(eb_warnings INTERFACE)
target_compile_options(eb_warnings INTERFACE
  /W4 /WX /permissive-
  $<$<COMPILE_LANGUAGE:CXX>:/EHsc /Zc:__cplusplus>)

if(EB_ASAN)
  string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

add_library(eb_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(eb_sqlite SYSTEM PUBLIC third_party/sqlite)
target_link_libraries(eb_sqlite PRIVATE eb_hardening)
target_compile_options(eb_sqlite PRIVATE /W0)
target_compile_definitions(eb_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)

add_executable(EbookReader WIN32 src/ui/main.cpp res/app.rc)
target_link_libraries(EbookReader PRIVATE eb_hardening eb_warnings)
target_link_options(EbookReader PRIVATE /MANIFEST:NO)

enable_testing()
add_subdirectory(tests)
