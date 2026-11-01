# lib-wii-gamecube-kit-tests builds synthetic GameCube/Wii discs with Dolphin's own code, then
# exercises the C API (linked statically) on them.
add_executable(lib-wii-gamecube-kit-tests lib-wii-gamecube-kit-tests.cpp synthetic_discs.cpp)
target_link_libraries(lib-wii-gamecube-kit-tests PRIVATE lib-wii-gamecube-kit-static)
add_test(NAME lib-wii-gamecube-kit-tests
  COMMAND lib-wii-gamecube-kit-tests "${CMAKE_CURRENT_BINARY_DIR}/work")
set_tests_properties(lib-wii-gamecube-kit-tests PROPERTIES FIXTURES_SETUP synthetic_discs TIMEOUT 600)

# The shared library from plain C: the header works from C and the library
# exports what it declares.
add_executable(lib-wii-gamecube-kit-shared-smoke shared_smoke.c)
target_link_libraries(lib-wii-gamecube-kit-shared-smoke PRIVATE lib-wii-gamecube-kit)
add_test(NAME lib-wii-gamecube-kit-shared-smoke COMMAND lib-wii-gamecube-kit-shared-smoke
  "${CMAKE_CURRENT_BINARY_DIR}/work/synthetic-gc.iso")
set_tests_properties(lib-wii-gamecube-kit-shared-smoke PROPERTIES FIXTURES_REQUIRED synthetic_discs)
if(WIN32)
  set_tests_properties(lib-wii-gamecube-kit-shared-smoke PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:lib-wii-gamecube-kit>")
endif()

if(TARGET lib-wii-gamecube-kit-cli)
  add_test(NAME lib-wii-gamecube-kit-cli-convert COMMAND lib-wii-gamecube-kit-cli convert -q -y
    -i "${CMAKE_CURRENT_BINARY_DIR}/work/synthetic-wii.iso"
    -o "${CMAKE_CURRENT_BINARY_DIR}/work/cli.rvz")
  add_test(NAME lib-wii-gamecube-kit-cli-probe COMMAND lib-wii-gamecube-kit-cli probe "${CMAKE_CURRENT_BINARY_DIR}/work/cli.rvz")
  set_tests_properties(lib-wii-gamecube-kit-cli-convert PROPERTIES FIXTURES_REQUIRED synthetic_discs
    FIXTURES_SETUP cli_rvz)
  set_tests_properties(lib-wii-gamecube-kit-cli-probe PROPERTIES FIXTURES_REQUIRED cli_rvz
    PASS_REGULAR_EXPRESSION "container: +RVZ.*platform: +Wii")
endif()
