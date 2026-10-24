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
target_compile_options(eb_sqlite PRIVATE /W0 $<$<CONFIG:Release>:/O1>)
target_compile_definitions(eb_sqlite PRIVATE SQLITE_THREADSAFE=2 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DQS=0
  SQLITE_LIKE_DOESNT_MATCH_BLOBS SQLITE_MAX_EXPR_DEPTH=0 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_SHARED_CACHE SQLITE_OMIT_PROGRESS_CALLBACK SQLITE_OMIT_JSON SQLITE_OMIT_UTF16
  SQLITE_OMIT_AUTHORIZATION SQLITE_OMIT_COMPLETE SQLITE_OMIT_GET_TABLE SQLITE_OMIT_TCL_VARIABLE SQLITE_OMIT_DECLTYPE
  SQLITE_OMIT_COMPILEOPTION_DIAGS SQLITE_OMIT_TRACE SQLITE_USE_ALLOCA)

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
  MINIZ_NO_STDIO MINIZ_NO_ZLIB_COMPATIBLE_NAMES _CRT_SECURE_NO_WARNINGS)
target_compile_options(eb_libmobi PRIVATE /W0 "/FI${CMAKE_SOURCE_DIR}/libs/eb_mupdf/eb_mobi_shim.h")
target_link_libraries(eb_libmobi PRIVATE eb_hardening)

add_library(eb_mupdf SHARED libs/eb_mupdf/eb_core.c libs/eb_mupdf/eb_render.c libs/eb_mupdf/eb_text.c libs/eb_mupdf/eb_alloc.c libs/eb_mupdf/eb_open.c
  libs/eb_mupdf/eb_mobi.c libs/eb_mupdf/eb_mobi_check.c libs/eb_mupdf/eb_cover.c libs/eb_mupdf/eb_embed.c libs/eb_mupdf/eb_font.c libs/eb_mupdf/eb_sfnt.c)
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

add_library(eb_lib STATIC src/lib/Db.cpp src/lib/Schema.cpp src/lib/Scanner.cpp src/lib/ScanWalk.cpp src/lib/ProgressStore.cpp)
target_link_libraries(eb_lib PUBLIC eb_base eb_sqlite)

add_library(eb_doc STATIC src/doc/TxtToFb2.cpp src/doc/TxtDecode.cpp)
target_link_libraries(eb_doc PUBLIC eb_base)

add_library(eb_tts STATIC src/tts/Segmenter.cpp src/tts/LangDetect.cpp src/tts/Utf8.cpp)
target_link_libraries(eb_tts PUBLIC eb_base)

add_executable(EbookReader WIN32 src/ui/main.cpp src/ui/App.cpp src/ui/MainWnd.cpp src/ui/Kv.cpp res/app.rc)
target_include_directories(EbookReader PRIVATE src/ui)
target_link_libraries(EbookReader PRIVATE eb_hardening eb_warnings eb_base eb_lib eb_mupdf comctl32 ole32 shell32 delayimp)
target_link_options(EbookReader PRIVATE /MANIFEST:NO /DELAYLOAD:eb_mupdf.dll)

target_sources(eb_lib PRIVATE src/lib/LibraryModel.cpp)

target_sources(EbookReader PRIVATE src/ui/MainWndLibrary.cpp src/ui/LibraryView.cpp src/ui/FolderTree.cpp src/ui/Covers.cpp
  src/ui/ScanWorker.cpp src/ui/LibrarySettings.cpp)
target_include_directories(EbookReader PRIVATE res)

add_library(eb_book STATIC src/lib/Metadata.cpp src/lib/CoverCache.cpp)
target_link_libraries(eb_book PUBLIC eb_lib eb_doc eb_tts eb_mupdf PRIVATE windowscodecs ole32 oleaut32 uuid)
target_link_libraries(EbookReader PRIVATE eb_book)

target_sources(EbookReader PRIVATE src/ui/ReaderLogic.cpp src/ui/ReaderWorker.cpp src/ui/ReaderCanvas.cpp src/ui/ReaderView.cpp
  src/ui/MainWndReader.cpp)
target_link_libraries(EbookReader PRIVATE eb_doc d2d1)
target_link_options(EbookReader PRIVATE /DELAYLOAD:d2d1.dll)

target_sources(eb_tts PRIVATE src/tts/winrt/WinRtTts.cpp)
target_link_libraries(eb_tts PUBLIC windowsapp)

target_sources(eb_base PRIVATE src/base/SecretStore.cpp src/base/PathSafety.cpp)
target_link_libraries(eb_base PRIVATE advapi32)
target_sources(eb_lib PRIVATE src/lib/AzureUsage.cpp)

target_sources(EbookReader PRIVATE src/ui/Tray.cpp src/ui/CloseAsk.cpp src/ui/SingleInstance.cpp src/ui/ExternalOpen.cpp
  src/ui/MainWndTray.cpp)

target_sources(EbookReader PRIVATE src/ui/Theme.cpp src/ui/FontPrefs.cpp src/ui/ReaderSettings.cpp)
target_link_libraries(EbookReader PRIVATE dwmapi comdlg32 uuid)
add_custom_command(TARGET EbookReader POST_BUILD COMMAND ${CMAKE_COMMAND}
  -DSRC=${CMAKE_SOURCE_DIR}/fonts -DDST=${CMAKE_BINARY_DIR}/fonts -P ${CMAKE_SOURCE_DIR}/cmake/copy_fonts.cmake)

target_sources(eb_tts PRIVATE src/tts/piper/PiperTts.cpp src/tts/piper/PiperProc.cpp src/tts/piper/PiperText.cpp
  src/tts/piper/player/PiperPlayer.cpp)
target_link_libraries(eb_tts PUBLIC advapi32)

target_sources(eb_tts PRIVATE src/tts/azure/AzureTts.cpp src/tts/azure/AzureHttp.cpp src/tts/azure/AzureProto.cpp
  src/tts/azure/AudioCache.cpp src/tts/azure/AzurePlayer.cpp)
target_link_libraries(eb_tts PUBLIC eb_lib winhttp PRIVATE bcrypt)

enable_testing()
add_subdirectory(tests)
