add_executable(eb_tests unit/test_main.cpp integration/test_eb_mupdf.cpp integration/test_eb_mobi.cpp integration/test_eb_bookmark.cpp integration/test_eb_embed.cpp integration/test_eb_fonts.cpp integration/test_eb_sfnt.cpp ${CMAKE_SOURCE_DIR}/libs/eb_mupdf/eb_sfnt.c)
target_compile_definitions(eb_tests PRIVATE EB_FONTS_DIR="${CMAKE_SOURCE_DIR}/fonts")
target_link_libraries(eb_tests PRIVATE eb_hardening eb_warnings eb_sqlite eb_mupdf eb_base)
target_include_directories(eb_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(eb_tests PRIVATE EB_MUPDF_DLL="$<TARGET_FILE:eb_mupdf>" EB_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
add_test(NAME eb_tests COMMAND eb_tests)
set_tests_properties(eb_tests PROPERTIES
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:eb_mupdf>")
target_sources(eb_tests PRIVATE unit/test_base_text.cpp unit/test_base_io.cpp unit/test_base_dll.cpp)
target_sources(eb_tests PRIVATE unit/test_crash.cpp unit/test_lib_db.cpp)
target_link_libraries(eb_tests PRIVATE eb_lib)
target_sources(eb_tests PRIVATE unit/test_doc_txt.cpp)
target_link_libraries(eb_tests PRIVATE eb_doc ole32 oleaut32 uuid)
target_sources(eb_tests PRIVATE unit/test_lib_scanner.cpp)
target_sources(eb_tests PRIVATE unit/test_lib_progress.cpp)

add_test(NAME ui_smoke COMMAND EbookReader --smoke)
set_tests_properties(ui_smoke PROPERTIES TIMEOUT 10)

add_library(eb_badlib SHARED ui/badlib.c)
target_link_libraries(eb_badlib PRIVATE eb_hardening)
set_target_properties(eb_badlib PROPERTIES OUTPUT_NAME eb_badlib RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_dependencies(eb_tests eb_badlib EbookReader)
add_test(NAME ui_badlib COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:EbookReader> -DDLL=$<TARGET_FILE:eb_badlib>
  -DWORK=${CMAKE_BINARY_DIR}/tests -P ${CMAKE_CURRENT_SOURCE_DIR}/ui/badlib.cmake)
set_tests_properties(ui_badlib PROPERTIES TIMEOUT 15)
target_sources(eb_tests PRIVATE unit/test_tts_segmenter.cpp unit/test_tts_lang.cpp)
target_link_libraries(eb_tests PRIVATE eb_tts)
target_sources(eb_tests PRIVATE unit/test_lib_model.cpp)

target_sources(eb_tests PRIVATE unit/test_reader_logic.cpp ${CMAKE_SOURCE_DIR}/src/ui/ReaderLogic.cpp)

add_executable(eb_dbcount ui/dbcount.cpp)
target_link_libraries(eb_dbcount PRIVATE eb_hardening eb_warnings eb_lib)
set_target_properties(eb_dbcount PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_dependencies(eb_tests eb_dbcount)
add_test(NAME ui_smoke_library COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:EbookReader> -DDLL=$<TARGET_FILE:eb_mupdf>
  -DDBCOUNT=$<TARGET_FILE:eb_dbcount> -DCORPUS=${CMAKE_SOURCE_DIR}/tests/corpus -DWORK=${CMAKE_BINARY_DIR}/tests
  -P ${CMAKE_CURRENT_SOURCE_DIR}/ui/library.cmake)
set_tests_properties(ui_smoke_library PROPERTIES TIMEOUT 60)
add_test(NAME ui_smoke_reader COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:EbookReader> -DDLL=$<TARGET_FILE:eb_mupdf>
  -DDBCOUNT=$<TARGET_FILE:eb_dbcount> -DBOOK=${CMAKE_SOURCE_DIR}/tests/corpus/good/epub/pg11.epub
  -DWORK=${CMAKE_BINARY_DIR}/tests -P ${CMAKE_CURRENT_SOURCE_DIR}/ui/reader.cmake)
set_tests_properties(ui_smoke_reader PROPERTIES TIMEOUT 100)
target_sources(eb_tests PRIVATE unit/test_lib_meta.cpp unit/test_lib_cover.cpp)
target_link_libraries(eb_tests PRIVATE eb_book)
target_sources(eb_tests PRIVATE unit/test_tts_winrt.cpp)
target_sources(eb_tests PRIVATE unit/test_lib_slots.cpp)
target_sources(eb_tests PRIVATE unit/test_secret_store.cpp unit/test_azure_usage.cpp)

target_sources(eb_tests PRIVATE unit/test_tray.cpp unit/test_external_open.cpp unit/test_path_safety.cpp ${CMAKE_SOURCE_DIR}/src/ui/Tray.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/CloseAsk.cpp ${CMAKE_SOURCE_DIR}/src/ui/Kv.cpp ${CMAKE_SOURCE_DIR}/src/ui/SingleInstance.cpp)
target_link_libraries(eb_tests PRIVATE comctl32 shell32 delayimp)
target_link_options(eb_tests PRIVATE /DELAYLOAD:comctl32.dll)
if(EB_ASAN)
  set(EB_TRAY_BUDGET_KB 0)
else()
  set(EB_TRAY_BUDGET_KB 20480)
endif()
add_test(NAME ui_smoke_tray COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:EbookReader> -DDLL=$<TARGET_FILE:eb_mupdf>
  -DBUDGET=${EB_TRAY_BUDGET_KB} -DWORK=${CMAKE_BINARY_DIR}/tests -P ${CMAKE_CURRENT_SOURCE_DIR}/ui/tray.cmake)
set_tests_properties(ui_smoke_tray PROPERTIES TIMEOUT 40)
add_test(NAME ui_single_instance COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:EbookReader> -DDLL=$<TARGET_FILE:eb_mupdf>
  -DBOOK=${CMAKE_SOURCE_DIR}/tests/corpus/good/epub/pg11.epub -DWORK=${CMAKE_BINARY_DIR}/tests
  -P ${CMAKE_CURRENT_SOURCE_DIR}/ui/single_instance.cmake)
set_tests_properties(ui_single_instance PROPERTIES TIMEOUT 40)
target_sources(eb_tests PRIVATE unit/test_theme.cpp ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp ${CMAKE_SOURCE_DIR}/src/ui/FontPrefs.cpp)
target_sources(eb_tests PRIVATE unit/test_tts_piper.cpp)
target_compile_definitions(eb_tests PRIVATE EB_PIPER_DIR="${CMAKE_SOURCE_DIR}/build/spike/piper/bin/piper"
  EB_PIPER_VOICES="${CMAKE_SOURCE_DIR}/build/spike/piper/voices")
