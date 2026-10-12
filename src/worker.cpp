#include "worker.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace gh2::worker
{
    namespace
    {
        struct Queue
        {
            std::mutex mutex;
            std::condition_variable waiting;
            std::deque<std::function<void()>> jobs;
        };

        // Never destroyed: the thread outlives main's statics.
        Queue &queue()
        {
            static Queue *const s_queue = []
            {
                Queue *made = new Queue;
                std::thread([made]
                {
                    for (;;)
                    {
                        std::unique_lock lock(made->mutex);
                        made->waiting.wait(lock, [made] { return !made->jobs.empty(); });
                        const auto job = std::move(made->jobs.front());
                        made->jobs.pop_front();
                        lock.unlock();
                        job();
                    }
                }).detach();
                return made;
            }();
            return *s_queue;
        }
    }

    void run(std::function<void()> job)
    {
        Queue &to = queue();
        {
            const std::lock_guard lock(to.mutex);
            to.jobs.push_back(std::move(job));
        }
        to.waiting.notify_one();
    }
}
