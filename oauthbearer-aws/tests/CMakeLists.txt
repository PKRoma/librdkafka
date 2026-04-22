# Plugin-local unit tests.
#
# These link the plugin's STATIC archive (librdkafka-oauthbearer-aws.a)
# so they can call internal symbols that the shared variant hides.
# Wired into CTest: `cd build && ctest -V -R oauthbearer-aws`.

#------------------------------------------------------------------------------
# Mock HTTP server — reusable object library for test binaries.
#------------------------------------------------------------------------------
add_library(rdkafka-oba-test-mock OBJECT mock_http_server.cpp)
set_target_properties(rdkafka-oba-test-mock PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(rdkafka-oba-test-mock
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

#------------------------------------------------------------------------------
# test_config — pure-C unit tests of the config parser.
#------------------------------------------------------------------------------
add_executable(test_oba_config test_config.c)
target_link_libraries(test_oba_config PRIVATE rdkafka-oauthbearer-aws-static)
set_target_properties(test_oba_config PROPERTIES C_STANDARD 11)
add_test(NAME oauthbearer-aws::config COMMAND test_oba_config)

#------------------------------------------------------------------------------
# test_sts_provider — exercise the STS provider against a mock HTTP server.
#------------------------------------------------------------------------------
add_executable(test_oba_sts_provider
    test_sts_provider.cpp
    $<TARGET_OBJECTS:rdkafka-oba-test-mock>)

target_link_libraries(test_oba_sts_provider
    PRIVATE rdkafka-oauthbearer-aws-static)
target_include_directories(test_oba_sts_provider
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(test_oba_sts_provider PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

add_test(NAME oauthbearer-aws::sts_provider COMMAND test_oba_sts_provider)

#------------------------------------------------------------------------------
# test_real_aws — env-gated M9 validation.
#
# Prints a skip message and exits 0 when RD_UT_AWS_PLUGIN is unset, so
# it's safe to include in the default label. When the gate is set, runs
# against real AWS STS (intended for EC2 hosts with IAM role credentials
# available via IMDS and outbound identity federation enabled).
#------------------------------------------------------------------------------
add_executable(test_oba_real_aws test_real_aws.c)
target_link_libraries(test_oba_real_aws PRIVATE rdkafka-oauthbearer-aws-static)
set_target_properties(test_oba_real_aws PROPERTIES C_STANDARD 11)
add_test(NAME oauthbearer-aws::real_aws COMMAND test_oba_real_aws)

# Tell CTest the set of tests that belong to this plugin — useful for
# `ctest -L oauthbearer-aws`. The real-AWS test also gets a `real_aws`
# label so CI can isolate it (`ctest -L real_aws` to run only those,
# or `ctest -LE real_aws` to exclude them on non-AWS machines).
set_tests_properties(
    oauthbearer-aws::config
    oauthbearer-aws::sts_provider
    PROPERTIES
    LABELS "oauthbearer-aws")

set_tests_properties(
    oauthbearer-aws::real_aws
    PROPERTIES
    LABELS "oauthbearer-aws;real_aws")
