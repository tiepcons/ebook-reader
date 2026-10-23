add_executable(eb_tests unit/test_main.cpp)
target_link_libraries(eb_tests PRIVATE eb_hardening eb_warnings eb_sqlite)
target_include_directories(eb_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
add_test(NAME eb_tests COMMAND eb_tests)
