#pragma once

#include "app/AppPaths.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace snds
{

enum class ReadinessStatus
{
    ok,
    warning, ///< Usable, but something deserves attention.
    missing, ///< Blocks separation until the user acts.
};

const char* toString (ReadinessStatus status) noexcept;

/** One environment check with user-facing guidance. */
struct ReadinessCheck
{
    juce::String id;    ///< Stable identifier, e.g. "worker-environment".
    juce::String title; ///< Short user-facing name.
    ReadinessStatus status = ReadinessStatus::missing;
    juce::String detail; ///< What was found (or not found), including relevant paths.
    juce::String action; ///< What the user should do next; empty when nothing is needed.
};

struct ReadinessReport
{
    std::vector<ReadinessCheck> checks;

    /** True when no check is missing, i.e. the environment could run separation. */
    bool isSeparationAvailable() const noexcept;

    const ReadinessCheck* find (const juce::String& id) const noexcept;
};

/**
    Produces a ReadinessReport. Implementations may block (file system now, a worker process later),
    so run() must be called off the message thread. Implementations must be safe to call repeatedly.

    Phase 2 adds a worker-backed probe that runs the worker's versioned readiness command and appends
    its checks (packages, model hash, CUDA health) to the local checks below.
*/
class ReadinessProbe
{
public:
    virtual ~ReadinessProbe() = default;

    virtual ReadinessReport run() = 0;
};

/** Inputs for LocalReadinessProbe, captured once so the probe itself is deterministic and testable. */
struct LocalEnvironment
{
    static constexpr const char* workerDirectoryVariable = "SNDS_WORKER_DIR";

    juce::File executableFile;      ///< Used to find a source checkout above the build output.
    juce::String workerDirOverride; ///< Value of SNDS_WORKER_DIR (may be empty).
    juce::String pathVariable;      ///< Value of PATH.
    juce::File modelsDirectory;     ///< AppPaths models directory.

    static LocalEnvironment capture (const AppPaths& paths);
};

/** Result of locating the worker project directory. */
struct WorkerLocation
{
    juce::File directory; ///< Empty if not found.
    juce::String source;  ///< How it was found, or why it was not.
};

/**
    File-system readiness checks that need no Python: worker project, uv, worker virtual environment,
    and model directory.
*/
class LocalReadinessProbe : public ReadinessProbe
{
public:
    static constexpr const char* bootstrapCommand = "scripts\\bootstrap.ps1";

    explicit LocalReadinessProbe (LocalEnvironment environment);

    ReadinessReport run() override;

    /** SNDS_WORKER_DIR if set, otherwise the nearest "worker" project above the executable. */
    static WorkerLocation locateWorkerDirectory (const LocalEnvironment& environment);

    /** Searches a Windows PATH value (';'-separated, entries may be quoted) for an executable. */
    static juce::File findOnPath (const juce::String& pathVariable, const juce::String& executableName);

    /** Interpreter path inside the worker's uv-managed virtual environment. */
    static juce::File getWorkerPython (const juce::File& workerDirectory);

private:
    LocalEnvironment environment;
};

} // namespace snds
