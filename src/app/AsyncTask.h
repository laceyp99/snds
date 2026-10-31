#pragma once

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace snds
{
/** Hands a closure to the thread that owns the UI (by default the JUCE message thread). */
using Dispatcher = std::function<void (std::function<void()>)>;

inline Dispatcher messageThreadDispatcher()
{
    return [] (std::function<void()> work) { juce::MessageManager::callAsync (std::move (work)); };
}

/** Runs `work` on its own thread and delivers its result through the dispatcher. Destroying the
    object waits for `work` to return and guarantees `done` is not called afterwards, so `done`
    may capture the owner. `work` should be bounded; it cannot be interrupted.
*/
template <typename Result>
class AsyncTask
{
public:
    AsyncTask (Dispatcher dispatcher, std::function<Result()> work, std::function<void (Result)> done)
        : alive (std::make_shared<std::atomic<bool>> (true))
    {
        thread = std::thread (
            [alive = alive, dispatcher = std::move (dispatcher), work = std::move (work), done = std::move (done)]
            {
                auto result = work();
                dispatcher (
                    [alive, done, result = std::move (result)]() mutable
                    {
                        if (*alive)
                            done (std::move (result));
                    });
            });
    }

    ~AsyncTask()
    {
        *alive = false;
        thread.join();
    }

    AsyncTask (const AsyncTask&) = delete;
    AsyncTask& operator= (const AsyncTask&) = delete;

private:
    std::shared_ptr<std::atomic<bool>> alive;
    std::thread thread;
};
} // namespace snds
