add_executable(eb_tests unit/test_main.cpp integration/test_eb_mupdf.cpp)
target_link_libraries(eb_tests PRIVATE eb_hardening eb_warnings eb_sqlite eb_mupdf eb_base)
target_include_directories(eb_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(eb_tests PRIVATE EB_MUPDF_DLL="$<TARGET_FILE:eb_mupdf>" EB_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
add_test(NAME eb_tests COMMAND eb_tests)
set_tests_properties(eb_tests PROPERTIES
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:eb_mupdf>")
target_sources(eb_tests PRIVATE unit/test_base_text.cpp unit/test_base_io.cpp unit/test_base_dll.cpp)
