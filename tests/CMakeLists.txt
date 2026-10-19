# drvz_tests builds synthetic GameCube/Wii discs with Dolphin's own code, then
# exercises the C API (linked statically) on them.
add_executable(drvz_tests drvz_tests.cpp synthetic_discs.cpp)
target_link_libraries(drvz_tests PRIVATE dolphinrvz_static)
add_test(NAME drvz_tests
  COMMAND drvz_tests "${CMAKE_CURRENT_BINARY_DIR}/work")
set_tests_properties(drvz_tests PROPERTIES FIXTURES_SETUP synthetic_discs TIMEOUT 600)

# The shared library from plain C: the header works from C and the library
# exports what it declares.
add_executable(drvz_shared_smoke shared_smoke.c)
target_link_libraries(drvz_shared_smoke PRIVATE dolphinrvz)
add_test(NAME drvz_shared_smoke COMMAND drvz_shared_smoke
  "${CMAKE_CURRENT_BINARY_DIR}/work/synthetic-gc.iso")
set_tests_properties(drvz_shared_smoke PROPERTIES FIXTURES_REQUIRED synthetic_discs)
if(WIN32)
  set_tests_properties(drvz_shared_smoke PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:dolphinrvz>")
endif()

if(TARGET drvz-cli)
  add_test(NAME drvz_cli_convert COMMAND drvz-cli convert -q -y
    -i "${CMAKE_CURRENT_BINARY_DIR}/work/synthetic-wii.iso"
    -o "${CMAKE_CURRENT_BINARY_DIR}/work/cli.rvz")
  add_test(NAME drvz_cli_probe COMMAND drvz-cli probe "${CMAKE_CURRENT_BINARY_DIR}/work/cli.rvz")
  set_tests_properties(drvz_cli_convert PROPERTIES FIXTURES_REQUIRED synthetic_discs
    FIXTURES_SETUP cli_rvz)
  set_tests_properties(drvz_cli_probe PROPERTIES FIXTURES_REQUIRED cli_rvz
    PASS_REGULAR_EXPRESSION "container: +RVZ.*platform: +Wii")
endif()
