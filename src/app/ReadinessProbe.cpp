#include "app/ReadinessProbe.h"

namespace snds
{

namespace
{
constexpr int maxParentLevelsToSearch = 8;

bool isWorkerProject (const juce::File& directory)
{
    return directory.getChildFile ("pyproject.toml").existsAsFile();
}

ReadinessCheck makeCheck (const char* id, const char* title)
{
    ReadinessCheck check;
    check.id = id;
    check.title = title;
    return check;
}
} // namespace

const char* toString (ReadinessStatus status) noexcept
{
    switch (status)
    {
        case ReadinessStatus::ok:
            return "ok";
        case ReadinessStatus::warning:
            return "warning";
        case ReadinessStatus::missing:
            return "missing";
    }

    return "unknown";
}

bool ReadinessReport::isSeparationAvailable() const noexcept
{
    if (checks.empty())
        return false;

    for (const auto& check : checks)
        if (check.status == ReadinessStatus::missing)
            return false;

    return true;
}

const ReadinessCheck* ReadinessReport::find (const juce::String& id) const noexcept
{
    for (const auto& check : checks)
        if (check.id == id)
            return &check;

    return nullptr;
}

//==============================================================================
LocalEnvironment LocalEnvironment::capture (const AppPaths& paths)
{
    LocalEnvironment environment;
    environment.executableFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    environment.workerDirOverride = juce::SystemStats::getEnvironmentVariable (workerDirectoryVariable, {});
    environment.pathVariable = juce::SystemStats::getEnvironmentVariable ("PATH", {});
    environment.modelsDirectory = paths.getDirectory (AppDirectory::models);
    return environment;
}

//==============================================================================
LocalReadinessProbe::LocalReadinessProbe (LocalEnvironment environmentIn)
    : environment (std::move (environmentIn))
{
}

WorkerLocation LocalReadinessProbe::locateWorkerDirectory (const LocalEnvironment& env)
{
    const auto overrideValue = env.workerDirOverride.trim().unquoted();

    if (overrideValue.isNotEmpty())
    {
        if (! juce::File::isAbsolutePath (overrideValue))
            return { {},
                     juce::String (LocalEnvironment::workerDirectoryVariable) + " must be an absolute path (got \""
                         + overrideValue + "\")." };

        const juce::File directory (overrideValue);

        if (! isWorkerProject (directory))
            return { {},
                     juce::String (LocalEnvironment::workerDirectoryVariable) + " points to "
                         + directory.getFullPathName() + ", which has no pyproject.toml." };

        return { directory, juce::String ("from ") + LocalEnvironment::workerDirectoryVariable };
    }

    if (env.executableFile == juce::File())
        return { {}, "The executable location is unknown." };

    auto directory = env.executableFile.getParentDirectory();

    for (auto level = 0; level < maxParentLevelsToSearch; ++level)
    {
        const auto candidate = directory.getChildFile ("worker");

        if (isWorkerProject (candidate))
            return { candidate, "found next to the source checkout" };

        const auto parent = directory.getParentDirectory();

        if (parent == directory)
            break;

        directory = parent;
    }

    return { {},
             "No source checkout with a worker\\pyproject.toml was found above " + env.executableFile.getFullPathName()
                 + "." };
}

juce::File LocalReadinessProbe::findOnPath (const juce::String& pathVariable, const juce::String& executableName)
{
    juce::StringArray entries;
    entries.addTokens (pathVariable, ";", "\"");

    for (auto entry : entries)
    {
        entry = entry.trim().unquoted().trim();

        if (entry.isEmpty() || ! juce::File::isAbsolutePath (entry))
            continue;

        const auto candidate = juce::File (entry).getChildFile (executableName);

        if (candidate.existsAsFile())
            return candidate;
    }

    return {};
}

juce::File LocalReadinessProbe::getWorkerPython (const juce::File& workerDirectory)
{
    return workerDirectory.getChildFile (".venv").getChildFile ("Scripts").getChildFile ("python.exe");
}

ReadinessReport LocalReadinessProbe::run()
{
    ReadinessReport report;
    const auto worker = locateWorkerDirectory (environment);

    {
        auto check = makeCheck ("worker-project", "Worker project");

        if (worker.directory != juce::File())
        {
            check.status = ReadinessStatus::ok;
            check.detail = worker.directory.getFullPathName() + " (" + worker.source + ")";
        }
        else
        {
            check.detail = worker.source;
            check.action = juce::String ("Run SNDS from a source checkout (scripts\\run.ps1), or set ")
                           + LocalEnvironment::workerDirectoryVariable + " to the checkout's worker folder.";
        }

        report.checks.push_back (check);
    }

    {
        auto check = makeCheck ("uv", "uv package manager");
        const auto uv = findOnPath (environment.pathVariable, "uv.exe");

        if (uv != juce::File())
        {
            check.status = ReadinessStatus::ok;
            check.detail = uv.getFullPathName();
        }
        else
        {
            check.detail = "uv.exe was not found on PATH.";
            check.action = juce::String ("Install uv (https://docs.astral.sh/uv/), open a new terminal, then run ")
                           + bootstrapCommand + ".";
        }

        report.checks.push_back (check);
    }

    {
        auto check = makeCheck ("worker-environment", "Worker Python environment");

        if (worker.directory == juce::File())
        {
            check.detail = "Unknown until the worker project is found.";
            check.action = "Resolve the worker project first.";
        }
        else if (const auto python = getWorkerPython (worker.directory); python.existsAsFile())
        {
            check.status = ReadinessStatus::ok;
            check.detail = python.getFullPathName();
        }
        else
        {
            check.detail = python.getFullPathName() + " does not exist.";
            check.action = juce::String ("Run ") + bootstrapCommand
                           + " (it detects CUDA automatically; add -Compute cpu to force the CPU build).";
        }

        report.checks.push_back (check);
    }

    {
        auto check = makeCheck ("model-directory", "Separation model folder");

        if (environment.modelsDirectory.isDirectory())
        {
            check.status = ReadinessStatus::ok;
            check.detail = environment.modelsDirectory.getFullPathName();
        }
        else
        {
            check.detail = environment.modelsDirectory.getFullPathName() + " does not exist yet.";
            check.action =
                juce::String ("Run ") + bootstrapCommand
                + " to download and verify the pinned separation model. Separation stays unavailable until then.";
        }

        report.checks.push_back (check);
    }

    return report;
}

} // namespace snds
