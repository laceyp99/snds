#pragma once

#include <juce_core/juce_core.h>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace snds::test
{
/** A new, empty folder under the system temp folder, deleted with its contents afterwards. */
class ScopedTempDirectory
{
public:
    ScopedTempDirectory()
        : directory (
              juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getNonexistentChildFile (
                      "snds-test-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt()), {}, false))
    {
        directory.createDirectory();
    }

    ~ScopedTempDirectory() { directory.deleteRecursively(); }

    const juce::File& get() const noexcept { return directory; }
    juce::File child (const juce::String& relativePath) const { return directory.getChildFile (relativePath); }

private:
    const juce::File directory;

    JUCE_DECLARE_NON_COPYABLE (ScopedTempDirectory)
};

/** A ScopedTempDirectory that is made on first use. A test object that is a global (they all are) must
    hold this, not a ScopedTempDirectory: every process that loads the test executable constructs every
    global, including the fake workers the tests start as child processes, and a child that is killed
    never runs its destructor, so an eager folder would be left behind once per child. */
class LazyTempDirectory
{
public:
    const juce::File& get() { return directory().get(); }
    juce::File child (const juce::String& relativePath) { return directory().child (relativePath); }

private:
    ScopedTempDirectory& directory()
    {
        if (made == nullptr)
            made = std::make_unique<ScopedTempDirectory>();

        return *made;
    }

    std::unique_ptr<ScopedTempDirectory> made;
};

/** Stands in for the message thread: background threads post closures with dispatcher(), and the
    test thread runs them in order with pumpUntil(). */
class DispatchQueue
{
public:
    std::function<void (std::function<void()>)> dispatcher()
    {
        return [this] (std::function<void()> work)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            items.push_back (std::move (work));
        };
    }

    /** Runs queued closures until done() holds or timeoutMs passes. Returns done(). */
    template <typename Predicate>
    bool pumpUntil (Predicate&& done, int timeoutMs = 30000)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

        for (;;)
        {
            runQueued();

            if (done())
                return true;

            if (juce::Time::getMillisecondCounterHiRes() > deadline)
                return false;

            juce::Thread::sleep (5);
        }
    }

    void runQueued()
    {
        for (;;)
        {
            std::function<void()> work;

            {
                const std::lock_guard<std::mutex> lock (mutex);

                if (items.empty())
                    return;

                work = std::move (items.front());
                items.pop_front();
            }

            work();
        }
    }

private:
    std::mutex mutex;
    std::deque<std::function<void()>> items;
};
} // namespace snds::test
