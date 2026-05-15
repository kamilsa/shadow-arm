# Detect the machine architecture for the uname test
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(SHADOW_UNAME_MACHINE "aarch64")
else()
  set(SHADOW_UNAME_MACHINE "x86_64")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/unistd.yaml.in" "${CMAKE_CURRENT_BINARY_DIR}/unistd.yaml" @ONLY)

add_linux_tests(BASENAME unistd COMMAND sh -c "\
../../target/debug/test_unistd \"$(uname -s)\" \"$(uname -n)\" \"$(uname -r)\" \"$(uname -v)\" \"$(uname -m)\" \
")
add_shadow_tests(BASENAME unistd SHADOW_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/unistd.yaml")