# JUCE UnitTest-based console runner for SNDS C++ code.
juce_add_console_app(snds_tests PRODUCT_NAME "SNDS Tests")

target_sources(snds_tests PRIVATE
    AppPathsTests.cpp
    EmbeddedArtworkTests.cpp
    ImportFixtures.h
    LoggingTests.cpp
    OpenGLCapabilityTests.cpp
    ReadinessProbeTests.cpp
    SongImportTests.cpp
    SourceFingerprintTests.cpp
    TestMain.cpp
    TestUtilities.h)

target_link_libraries(snds_tests PRIVATE snds::core snds::compiler_options)

# Committed fixtures (testdata/import/*.mp3) are read in place from the source tree.
target_compile_definitions(snds_tests PRIVATE SNDS_TESTDATA_DIR="${PROJECT_SOURCE_DIR}/testdata")
snds_set_runtime_output(snds_tests)
set_target_properties(snds_tests PROPERTIES FOLDER "SNDS")

add_test(NAME snds_cpp_unit_tests COMMAND snds_tests)
set_tests_properties(snds_cpp_unit_tests PROPERTIES TIMEOUT 120)
