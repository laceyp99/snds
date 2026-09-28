# snds_core: all SNDS code shared by the app, the unit test runner, and spikes.
add_library(snds_core STATIC
    app/AppPaths.cpp
    app/AppPaths.h
    app/Logging.cpp
    app/Logging.h
    app/MainComponent.cpp
    app/MainComponent.h
    app/ReadinessProbe.cpp
    app/ReadinessProbe.h
    model/AppState.h
    rendering/OpenGLCapability.cpp
    rendering/OpenGLCapability.h
    ui/PlaceholderScreen.cpp
    ui/PlaceholderScreen.h
    ui/ReadinessScreen.cpp
    ui/ReadinessScreen.h)

add_library(snds::core ALIAS snds_core)

target_include_directories(snds_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(snds_core PUBLIC SNDS_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(snds_core
    PUBLIC snds::juce
    PRIVATE snds::compiler_options)
set_target_properties(snds_core PROPERTIES FOLDER "SNDS")

# The desktop application (product name "SNDS" -> SNDS.exe).
juce_add_gui_app(snds_app
    PRODUCT_NAME "SNDS"
    VERSION "${PROJECT_VERSION}"
    COMPANY_NAME "SNDS"
    DESCRIPTION "Local visual stem-remix instrument")

target_sources(snds_app PRIVATE app/Main.cpp)
target_link_libraries(snds_app PRIVATE snds::core snds::compiler_options)
snds_set_runtime_output(snds_app)
set_target_properties(snds_app PROPERTIES FOLDER "SNDS")
