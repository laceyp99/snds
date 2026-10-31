# The `...4.3` upper bound opts into every policy up to CMake 4.3 (for example CMP0168:
# FetchContent downloads directly instead of through a sub-build).
cmake_minimum_required(VERSION 3.22...4.3)

project(SNDS VERSION 0.0.1 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SNDS supports 64-bit builds only. With Visual Studio, configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SndsJuce)
include(SndsWarnings)

enable_testing()

# --- snds_core: SNDS logic plus the JUCE modules, compiled once -----------------------------
add_library(snds_core STATIC)

snds_target_sources(snds_core
    src/app/AppInfo.cpp
    src/app/AppInfo.h
    src/app/AppOptions.cpp
    src/app/AppOptions.h
    src/app/AsyncTask.h
    src/app/AudioService.cpp
    src/app/AudioService.h
    src/app/CacheStore.cpp
    src/app/CacheStore.h
    src/app/DiagnosticsExport.cpp
    src/app/DiagnosticsExport.h
    src/app/CacheOverview.cpp
    src/app/CacheOverview.h
    src/app/DataPaths.cpp
    src/app/ErrorReport.cpp
    src/app/ErrorReport.h
    src/app/ProgressText.cpp
    src/app/ProgressText.h
    src/app/RecentSongs.cpp
    src/app/RecentSongs.h
    src/app/WindowAttention.cpp
    src/app/WindowAttention.h
    src/app/DataPaths.h
    src/app/Logging.cpp
    src/app/Logging.h
    src/app/OriginalLocations.cpp
    src/app/OriginalLocations.h
    src/app/MixService.cpp
    src/app/MixService.h
    src/app/PlaybackController.cpp
    src/app/PlaybackController.h
    src/app/Readiness.cpp
    src/app/Readiness.h
    src/app/Settings.cpp
    src/app/Settings.h
    src/app/SongPackageEdit.cpp
    src/app/SongPackageEdit.h
    src/app/SongSession.cpp
    src/app/SongSession.h
    src/app/WorkerLocation.cpp
    src/app/WorkerLocation.h
    src/app/WorkerProcess.cpp
    src/app/WorkerProcess.h
    src/audio/AudioDeviceController.cpp
    src/audio/AudioDeviceController.h
    src/audio/FrameSource.cpp
    src/audio/FrameSource.h
    src/audio/Mp3Gapless.cpp
    src/audio/Mp3Gapless.h
    src/audio/OriginalAlignment.cpp
    src/audio/OriginalAlignment.h
    src/audio/PlaybackEngine.cpp
    src/audio/PlaybackEngine.h
    src/audio/PrefetchStream.cpp
    src/audio/PrefetchStream.h
    src/audio/Resampler.cpp
    src/audio/Resampler.h
    src/audio/SeekTarget.h
    src/audio/StemMixStage.h
    src/dsp/MasterStage.cpp
    src/dsp/MasterStage.h
    src/dsp/MixMeters.cpp
    src/dsp/MixMeters.h
    src/dsp/MixParameters.cpp
    src/dsp/MixParameters.h
    src/dsp/MixProcessor.cpp
    src/dsp/MixProcessor.h
    src/dsp/ParameterStore.cpp
    src/dsp/ParameterStore.h
    src/dsp/Sanitize.h
    src/dsp/SharedReverb.cpp
    src/dsp/SharedReverb.h
    src/dsp/StemFilter.cpp
    src/dsp/StemFilter.h
    src/dsp/StemLevelStage.cpp
    src/dsp/StemLevelStage.h
    src/dsp/StemSends.cpp
    src/dsp/StemSends.h
    src/dsp/TempoDelay.cpp
    src/dsp/TempoDelay.h
    src/dsp/VisualAnalysis.cpp
    src/dsp/VisualAnalysis.h
    src/model/Artwork.cpp
    src/model/Artwork.h
    src/model/SongImport.cpp
    src/model/SongImport.h
    src/model/SongMetadata.cpp
    src/model/SongMetadata.h
    src/model/SourceFingerprint.cpp
    src/model/SourceFingerprint.h
    src/model/WorkerProtocol.cpp
    src/model/WorkerProtocol.h
    src/rendering/ArtworkPalette.cpp
    src/rendering/ArtworkPalette.h
    src/rendering/CpuRenderer.cpp
    src/rendering/CpuRenderer.h
    src/rendering/FramePacing.h
    src/rendering/GlCapability.cpp
    src/rendering/GlCapability.h
    src/rendering/GlRenderer.cpp
    src/rendering/GlRenderer.h
    src/rendering/RenderOverrides.cpp
    src/rendering/RenderOverrides.h
    src/rendering/RenderTypes.cpp
    src/rendering/RenderTypes.h
    src/rendering/StaticRenderer.cpp
    src/rendering/StaticRenderer.h
    src/rendering/VisualController.cpp
    src/rendering/VisualController.h
    src/rendering/VisualHost.cpp
    src/rendering/VisualHost.h
    src/rendering/VisualMapping.cpp
    src/rendering/VisualMapping.h
    src/rendering/VisualRenderer.h
    src/rendering/VisualStatus.cpp
    src/rendering/VisualStatus.h
    src/ui/ArtworkConfirmationPanel.cpp
    src/ui/ArtworkConfirmationPanel.h
    src/ui/AudioSettingsDialog.cpp
    src/ui/AudioSettingsDialog.h
    src/ui/FullscreenVisuals.cpp
    src/ui/FullscreenVisuals.h
    src/ui/CacheDialog.cpp
    src/ui/CacheDialog.h
    src/ui/ErrorBanner.cpp
    src/ui/ErrorBanner.h
    src/ui/ImportPanel.cpp
    src/ui/ImportPanel.h
    src/ui/RecentList.cpp
    src/ui/RecentList.h
    src/ui/MainComponent.cpp
    src/ui/MainComponent.h
    src/ui/MasterSection.cpp
    src/ui/MasterSection.h
    src/ui/MixControls.cpp
    src/ui/MixControls.h
    src/ui/MixPanel.cpp
    src/ui/MixPanel.h
    src/ui/ReadinessPanel.cpp
    src/ui/ReadinessPanel.h
    src/ui/SongPanel.cpp
    src/ui/StemCard.cpp
    src/ui/StemCard.h
    src/ui/SongPanel.h
    src/ui/TempoFxSection.cpp
    src/ui/TempoFxSection.h
    src/ui/TransportBar.cpp
    src/ui/TransportBar.h)

# Build metadata is attached to the one source that needs it. A target-level definition would
# make every version bump recompile all of JUCE.
set_property(SOURCE src/app/AppInfo.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    SNDS_VERSION_STRING="${PROJECT_VERSION}"
    SNDS_JUCE_VERSION_STRING="${SNDS_JUCE_VERSION}")

# Source-first alpha: the app finds the worker project of the checkout it was built from.
set_property(SOURCE src/app/WorkerLocation.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    SNDS_DEFAULT_WORKER_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/worker")

target_include_directories(snds_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(snds_core PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

# Read sources as UTF-8 regardless of the machine's code page.
target_compile_options(snds_core PUBLIC $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

snds_share_juce_modules(snds_core
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_opengl)

target_link_libraries(snds_core PUBLIC
    bcrypt
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# --- SNDS: the desktop app -------------------------------------------------------------------
juce_add_gui_app(SNDS
    PRODUCT_NAME "SNDS"
    VERSION "${PROJECT_VERSION}"
    COMPANY_NAME "SNDS"
    COMPANY_COPYRIGHT "Copyright (c) 2026 Patrick Lacey"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

snds_target_sources(SNDS src/app/Main.cpp)
target_link_libraries(SNDS PRIVATE snds_core)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT SNDS)

# --- snds_tests: JUCE UnitTestRunner console app, registered with CTest ----------------------
juce_add_console_app(snds_tests
    PRODUCT_NAME "SNDS Tests"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

snds_target_sources(snds_tests
    tests/cpp/AppTests.cpp
    tests/cpp/AudioCallbackAuditTests.cpp
    tests/cpp/ArtworkTests.cpp
    tests/cpp/CacheStoreTests.cpp
    tests/cpp/DataPathsTests.cpp
    tests/cpp/DeviceTests.cpp
    tests/cpp/FakeAudioDevice.h
    tests/cpp/DspHeavy.h
    tests/cpp/DspMasterTests.cpp
    tests/cpp/DspMixProcessorTests.cpp
    tests/cpp/DspParameterTests.cpp
    tests/cpp/DspReverbSendTests.cpp
    tests/cpp/DspStemFilterTests.cpp
    tests/cpp/DspStressTests.cpp
    tests/cpp/DspStemLevelTests.cpp
    tests/cpp/DspTempoDelayTests.cpp
    tests/cpp/EndToEndTests.cpp
    tests/cpp/FixtureTests.cpp
    tests/cpp/ErrorReportTests.cpp
    tests/cpp/CacheUiTests.cpp
    tests/cpp/DiagnosticsExportTests.cpp
    tests/cpp/Phase6JourneyTests.cpp
    tests/cpp/ProgressTextTests.cpp
    tests/cpp/RecentSongsTests.cpp
    tests/cpp/LoggingTests.cpp
    tests/cpp/MixResetTests.cpp
    tests/cpp/MixUiTests.cpp
    tests/cpp/OriginalTests.cpp
    tests/cpp/Phase3ValidationTests.cpp
    tests/cpp/Phase4ValidationTests.cpp
    tests/cpp/PlaybackControllerTests.cpp
    tests/cpp/PlaybackTestKit.h
    tests/cpp/PlaybackTests.cpp
    tests/cpp/RenderCapabilityTests.cpp
    tests/cpp/RenderFallbackTests.cpp
    tests/cpp/RenderHostTests.cpp
    tests/cpp/ReadinessTests.cpp
    tests/cpp/SettingsTests.cpp
    tests/cpp/SongImportTests.cpp
    tests/cpp/SongFixtures.h
    tests/cpp/SongMetadataTests.cpp
    tests/cpp/SongSessionTests.cpp
    tests/cpp/SourceFingerprintTests.cpp
    tests/cpp/TestHelpers.h
    tests/cpp/TestMain.cpp
    tests/cpp/VisualAnalysisTests.cpp
    tests/cpp/TransportTests.cpp
    tests/cpp/VisualMappingTests.cpp
    tests/cpp/VisualPaletteTests.cpp
    tests/cpp/VisualPanelTests.cpp
    tests/cpp/VisualValidationTests.cpp
    tests/cpp/WorkerProcessTests.cpp
    tests/cpp/WorkerProtocolTests.cpp)

target_link_libraries(snds_tests PRIVATE snds_core)
target_compile_definitions(snds_tests PRIVATE SNDS_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

# The suite is split into shards that run as separate processes (`ctest --parallel`, which scripts/test.ps1
# passes), so the default run takes about a third of the time. `snds_tests` with no argument still runs
# everything in one process. SNDS_HOME points into the build tree, so tests can never touch the real data folder.
set(SNDS_TEST_SHARDS 4)
math(EXPR SNDS_LAST_SHARD "${SNDS_TEST_SHARDS} - 1")
foreach(shard RANGE ${SNDS_LAST_SHARD})
    add_test(NAME snds_tests_${shard} COMMAND snds_tests --shard ${shard}/${SNDS_TEST_SHARDS})
    set_tests_properties(snds_tests_${shard} PROPERTIES
        TIMEOUT 300
        ENVIRONMENT "SNDS_HOME=${CMAKE_BINARY_DIR}/test-home")
endforeach()
