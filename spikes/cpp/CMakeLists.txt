# Phase 0 spikes. These are experiments, not product code; they link snds_core so their findings
# exercise the same JUCE configuration and capability logic that the app uses.

# spike-opengl-tier: non-interactive OpenGL capability probe with CPU fallback.
# A GUI-subsystem app (JUCE attaches to the parent console for stdout). It cannot be a console app:
# JUCEApplicationBase::main(argc, argv) is only compiled when _CONSOLE is defined for juce_events,
# and the shared snds_juce library is compiled for GUI applications.
juce_add_gui_app(snds_gl_probe PRODUCT_NAME "snds_gl_probe" VERSION "${PROJECT_VERSION}")
target_sources(snds_gl_probe PRIVATE gl_probe/GlProbeMain.cpp)
target_link_libraries(snds_gl_probe PRIVATE snds::core snds::compiler_options)
snds_set_runtime_output(snds_gl_probe)
set_target_properties(snds_gl_probe PROPERTIES FOLDER "Spikes")
