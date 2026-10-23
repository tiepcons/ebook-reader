add_executable(eb_tests unit/test_main.cpp integration/test_eb_mupdf.cpp integration/test_eb_mobi.cpp integration/test_eb_bookmark.cpp)
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

add_executable(eb_dbcount ui/dbcount.cpp)
target_link_libraries(eb_dbcount PRIVATE eb_hardening eb_warnings eb_lib)
set_target_properties(eb_dbcount PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_dependencies(eb_tests eb_dbcount)
add_test(NAME ui_smoke_library COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:EbookReader> -DDLL=$<TARGET_FILE:eb_mupdf>
  -DDBCOUNT=$<TARGET_FILE:eb_dbcount> -DCORPUS=${CMAKE_SOURCE_DIR}/tests/corpus -DWORK=${CMAKE_BINARY_DIR}/tests
  -P ${CMAKE_CURRENT_SOURCE_DIR}/ui/library.cmake)
set_tests_properties(ui_smoke_library PROPERTIES TIMEOUT 60)
