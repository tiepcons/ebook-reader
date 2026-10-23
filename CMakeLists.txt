cmake_minimum_required(VERSION 3.28)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(EbookReader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
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

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(EB_MU_CFG Release)
else()
  set(EB_MU_CFG Debug)
endif()
set(EB_MU_LIBS libmupdf libthirdparty libresources libharfbuzz libextract libpkcs7)
add_library(eb_mupdf_engine INTERFACE)
foreach(lib IN LISTS EB_MU_LIBS)
  set(path "${CMAKE_SOURCE_DIR}/build/mupdf/${EB_MU_CFG}/${lib}.lib")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Missing ${path}; run tools/build_mupdf.ps1 -Config ${EB_MU_CFG}")
  endif()
  target_link_libraries(eb_mupdf_engine INTERFACE "${path}")
endforeach()
target_link_libraries(eb_mupdf_engine INTERFACE user32 gdi32 advapi32 shell32)

set(EB_MOBI_DIR "${CMAKE_SOURCE_DIR}/third_party/libmobi/src")
set(EB_MOBI_SRC buffer compression debug index memory meta parse_rawml read structure util write xmlwriter opf miniz)
list(TRANSFORM EB_MOBI_SRC PREPEND "${EB_MOBI_DIR}/")
list(TRANSFORM EB_MOBI_SRC APPEND ".c")
add_library(eb_libmobi STATIC ${EB_MOBI_SRC})
target_include_directories(eb_libmobi SYSTEM PUBLIC "${EB_MOBI_DIR}")
target_compile_definitions(eb_libmobi PRIVATE USE_XMLWRITER USE_MINIZ MOBI_INLINE=__inline
  MINIZ_NO_STDIO MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_compile_options(eb_libmobi PRIVATE /W0 /wd4146 /wd4703)
set_source_files_properties("${EB_MOBI_DIR}/read.c" PROPERTIES
  COMPILE_OPTIONS "/FI${CMAKE_SOURCE_DIR}/libs/eb_mupdf/eb_mobi_io.h")
target_link_libraries(eb_libmobi PRIVATE eb_hardening)

add_library(eb_mupdf SHARED libs/eb_mupdf/eb_core.c libs/eb_mupdf/eb_render.c libs/eb_mupdf/eb_text.c libs/eb_mupdf/eb_alloc.c libs/eb_mupdf/eb_open.c
  libs/eb_mupdf/eb_mobi.c libs/eb_mupdf/eb_cover.c)
set_target_properties(eb_mupdf PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libs")
target_compile_definitions(eb_mupdf PRIVATE EB_MUPDF_BUILD)
target_include_directories(eb_mupdf PRIVATE libs/eb_mupdf)
target_include_directories(eb_mupdf SYSTEM INTERFACE libs/eb_mupdf)
target_include_directories(eb_mupdf SYSTEM PRIVATE third_party/mupdf/include)
target_link_libraries(eb_mupdf PRIVATE eb_hardening eb_warnings eb_libmobi eb_mupdf_engine)

add_library(eb_base STATIC src/base/Log.cpp src/base/Text.cpp src/base/Paths.cpp src/base/AtomicFile.cpp
  src/base/Hash.cpp src/base/DllLoader.cpp src/base/CrashGuard.cpp)
target_include_directories(eb_base PUBLIC src)
target_link_libraries(eb_base PUBLIC eb_hardening eb_warnings PRIVATE bcrypt normaliz dbghelp)

add_library(eb_lib STATIC src/lib/Db.cpp src/lib/Schema.cpp)
target_link_libraries(eb_lib PUBLIC eb_base eb_sqlite)

add_library(eb_doc STATIC src/doc/TxtToFb2.cpp)
target_link_libraries(eb_doc PUBLIC eb_base)

add_executable(EbookReader WIN32 src/ui/main.cpp res/app.rc)
target_link_libraries(EbookReader PRIVATE eb_hardening eb_warnings)
target_link_options(EbookReader PRIVATE /MANIFEST:NO)

enable_testing()
add_subdirectory(tests)
