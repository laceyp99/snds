# JUCE UnitTest-based console runner for SNDS C++ code.
juce_add_console_app(snds_tests PRODUCT_NAME "SNDS Tests")

target_sources(snds_tests PRIVATE
    AppPathsTests.cpp
    LoggingTests.cpp
    OpenGLCapabilityTests.cpp
    ReadinessProbeTests.cpp
    TestMain.cpp
    TestUtilities.h)

target_link_libraries(snds_tests PRIVATE snds::core snds::compiler_options)
snds_set_runtime_output(snds_tests)
set_target_properties(snds_tests PROPERTIES FOLDER "SNDS")

add_test(NAME snds_cpp_unit_tests COMMAND snds_tests)
set_tests_properties(snds_cpp_unit_tests PROPERTIES TIMEOUT 120)
