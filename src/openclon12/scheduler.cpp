// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "scheduler.hpp"

#ifdef _WIN32
#include <assert.h>
#include <strsafe.h>
#else
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#endif

namespace BackgroundTaskScheduler
{

//-------------------------------------------------------------------------------------------------
Scheduler::Scheduler()
{
    m_QueuedEvents.emplace_back(); // throw( bad_alloc )
    m_QueuedEventsPseudoEnd = m_QueuedEvents.begin();
}

//-------------------------------------------------------------------------------------------------
void Scheduler::QueueTask(Task task)
{
    bool bCancelTask = false;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        if (m_CurrentMode.NumThreads == 0 || m_bShutdown)
        {
            bCancelTask = true;
        }
        else
        {
            m_Tasks.push_back(QueuedTask{ task, m_QueuedEventsPseudoEnd }); // throw( bad_alloc )
        }
    }

    if (bCancelTask)
    {
        if (task.m_Cancel)
        {
            task.m_Cancel(task.m_pContext);
        }
    }
    else
    {
        m_CV.notify_one();
    }
}

inline auto PriorityToPlatformPriority(Priority p)
{
    switch (p)
    {
    default:
#ifdef _WIN32
    case Priority::Normal: return THREAD_PRIORITY_NORMAL;
    case Priority::Idle: return THREAD_PRIORITY_IDLE;
#else
    case Priority::Normal: return SCHED_OTHER;
    case Priority::Idle: return SCHED_IDLE;
#endif
    }
}

inline void SetPlatformThreadPriority(std::thread::native_handle_type t, int p)
{
#ifdef _WIN32
    SetThreadPriority(t, p);
#else
    pthread_setschedprio(t, p);
#endif
}

//-------------------------------------------------------------------------------------------------
void Scheduler::SetSchedulingModeImpl(SchedulingMode mode, std::unique_lock<std::mutex>& lock)
{
    assert(lock.owns_lock());
    std::vector<std::thread> ThreadsToWaitOn;

    SchedulingMode previousMode = m_EffectiveMode;
    m_EffectiveMode = mode;

    size_t NewNumThreads = mode.NumThreads;
    size_t PreviousNumThreads = m_Threads.size();

    // Adjust number of threads
    if (NewNumThreads > PreviousNumThreads)
    {
        m_Threads.resize(NewNumThreads); // throw( bad_alloc )
        for (auto i = PreviousNumThreads; i < NewNumThreads; ++i)
        {
            m_Threads[i] = std::thread([this, i]() { TaskThread((int)i); }); // throw (...)
        }
    }
    else if (NewNumThreads < PreviousNumThreads)
    {
        ThreadsToWaitOn.reserve(PreviousNumThreads - NewNumThreads); // throw( bad_alloc )
        for (size_t i = NewNumThreads; i < PreviousNumThreads; ++i)
        {
            if (m_Threads[i].joinable())
            {
                if (m_Threads[i].get_id() == std::this_thread::get_id())
                {
                    m_ExitingThreads.push_back(std::move(m_Threads[i])); // throw( bad_alloc )
                }
                else
                {
                    ThreadsToWaitOn.emplace_back(std::move(m_Threads[i]));
                }
            }
        }

        m_Threads.resize(NewNumThreads);
    }

    // Set priorities
    auto NewPriority = PriorityToPlatformPriority(mode.ThreadPriority);
    auto OldPriority = PriorityToPlatformPriority(previousMode.ThreadPriority);
    for (size_t i = 0; i < NewNumThreads; ++i)
    {
        auto ThisThreadOldPriority = (i < PreviousNumThreads) ? OldPriority : PriorityToPlatformPriority(Priority::Normal);
        if (NewPriority != ThisThreadOldPriority)
        {
            SetPlatformThreadPriority(m_Threads[i].native_handle(), NewPriority);
        }
    }
    // If we're increasing priority but lowering thread count, increase priority of threads we're going to wait on.
    if (NewPriority > OldPriority)
    {
        for (auto& thread : ThreadsToWaitOn)
        {
            SetPlatformThreadPriority(thread.native_handle(), NewPriority);
        }
    }

    // Wait for threads
    lock.unlock();
    if (!ThreadsToWaitOn.empty())
    {
        m_CV.notify_all();
        for (auto& thread : ThreadsToWaitOn)
        {
            thread.join();
        }
    }
}

//-------------------------------------------------------------------------------------------------
void Scheduler::SetSchedulingMode(SchedulingMode mode)
{
    std::unique_lock<std::mutex> lock(m_Lock);
    if (m_bShutdown)
    {
        // If we're shut down, ignore requests to spin back up.
        return;
    }
    if (mode == m_CurrentMode)
    {
        return;
    }

    if (m_CurrentMode == m_EffectiveMode &&
        (mode > m_EffectiveMode || IsSchedulerIdle(lock)))
    {
        // Increasing number or priority of threads OR there's nothing currently executing - do it immediately
        m_CurrentMode = mode;
        SetSchedulingModeImpl(mode, lock); // Releases lock
    }
    else
    {
        // Decreasing number or priority of threads, or there's already a pending mode
        // change, so just queue the mode change

        if (m_CurrentMode.NumThreads == 0)
        {
            // There's a task queued which will drop the mode down to no threads
            // Since we'll refuse to queue tasks when in this mode, this task should
            // be the last one - replace it with our new task
            m_Tasks.pop_back();
        }

        QueueSetSchedulingModeTask(mode, lock);
        lock.unlock();
        m_CV.notify_one();
    }
}

//-------------------------------------------------------------------------------------------------
void Scheduler::SignalEventOnCompletionOfCurrentTasks(XPlatHelpers::Event hEvent, SchedulingMode modeAfterSignal)
{
    {
        std::unique_lock<std::mutex> lock(m_Lock);

        // Tasks won't execute - just set the event
        if (m_EffectiveMode.NumThreads == 0 || IsSchedulerIdle(lock))
        {
            XPlatHelpers::SetEvent(hEvent);
            m_CurrentMode = modeAfterSignal;
            SetSchedulingModeImpl(modeAfterSignal, lock);
            return;
        }

        // Update the entry that existing tasks will signal
        QueuedEventSignal& signal = m_QueuedEvents.back();
        signal.m_RefCount = (long)(m_Tasks.size() + m_TasksInProgress);
        signal.m_Event = XPlatHelpers::unique_event(hEvent, XPlatHelpers::unique_event::copy_tag{});
        // Add a new entry that new tasks will reference
        m_QueuedEvents.emplace_back();
        ++m_QueuedEventsPseudoEnd;

        // If we want to end up in a different mode, then queue a task to put us there
        if (modeAfterSignal != m_CurrentMode)
        {
            QueueSetSchedulingModeTask(modeAfterSignal, lock);
        }
    }

    m_CV.notify_one();
}

//-------------------------------------------------------------------------------------------------
void Scheduler::TaskThread(int ThreadID) noexcept
{
    {
#if _WIN32
        wchar_t ThreadName[64];
        if (SUCCEEDED(StringCchPrintfW(ThreadName, _countof(ThreadName), L"D3D Background Thread %d", ThreadID)))
        {
            SetThreadDescription(GetCurrentThread(), ThreadName);
        }
#endif
    }

    std::unique_lock<std::mutex> lock(m_Lock);
    while (true)
    {
        QueuedTask task = {};
        while (true)
        {
            if (ThreadID >= (int)m_EffectiveMode.NumThreads)
            {
                // This thread is done
                return;
            }
            if (!m_Tasks.empty())
            {
                // Pop the front task and exit the loop, this thread will now do work
                task = m_Tasks.front();
                m_Tasks.pop_front();
                ++m_TasksInProgress;
                break;
            }

            // Not supposed to exit yet, and nothing to do - wait for a notification
            m_CV.wait(lock);
        }

        // Do the work
        lock.unlock();
        task.m_Callback(task.m_pContext);
        lock.lock();

        RetireTask(task, lock);
        --m_TasksInProgress;
    }
}

//-------------------------------------------------------------------------------------------------
void Scheduler::SetSchedulingModeTask(SchedulingMode mode) noexcept
{
    std::unique_lock<std::mutex> lock(m_Lock);
    SetSchedulingModeImpl(mode, lock); // Releases lock
}

struct SetSchedulingModeTaskContext
{
    Scheduler* pThis;
    SchedulingMode mode;
};
//-------------------------------------------------------------------------------------------------
void __stdcall Scheduler::SetSchedulingModeTaskStatic(void* pContext)
{
    std::unique_ptr<SetSchedulingModeTaskContext> spContext(static_cast<SetSchedulingModeTaskContext*>(pContext));
    spContext->pThis->SetSchedulingModeTask(spContext->mode);
}

//-------------------------------------------------------------------------------------------------
void Scheduler::QueueSetSchedulingModeTask(SchedulingMode mode, std::unique_lock<std::mutex> const& lock)
{
    assert(lock.owns_lock()); UNREFERENCED_PARAMETER(lock);
    m_CurrentMode = mode;
    std::unique_ptr<SetSchedulingModeTaskContext> spContext(new SetSchedulingModeTaskContext{ this, mode }); // throw
    m_Tasks.push_back(QueuedTask{
        Task{
            SetSchedulingModeTaskStatic,
            mode.NumThreads == 0 ? SetSchedulingModeTaskStatic : nullptr,
            spContext.get()
        },
        m_QueuedEventsPseudoEnd }); // throw
    spContext.release();
}

//-------------------------------------------------------------------------------------------------
void Scheduler::CancelExistingTasks() noexcept
{
    decltype(m_Tasks) TasksToCancel;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        std::swap(TasksToCancel, m_Tasks);
    }

    for (auto& task : TasksToCancel)
    {
        if (task.m_Cancel)
        {
            task.m_Cancel(task.m_pContext);
        }
    }
    std::unique_lock<std::mutex> lock(m_Lock);
    for (auto& task : TasksToCancel)
    {
        RetireTask(task, lock);
    }
}

//-------------------------------------------------------------------------------------------------
void Scheduler::Shutdown() noexcept
{
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_bShutdown = true;
    }

    CancelExistingTasks();

    std::unique_lock<std::mutex> lock(m_Lock);
    m_CurrentMode = { 0, Priority::Idle };
    SetSchedulingModeImpl(m_CurrentMode, lock); // Releases lock
    assert(m_Threads.empty());

    // The SetSchedulingMode call either waited for all threads to exit, or a thread processed
    // a task to wait for all other threads to exit, and then it will exit itself.
    // In the latter case, it would've added itself to this list. We need to make sure it's
    // actually exited before allowing this object to be destroyed, so wait for that now.
    //
    // It's safe to read this without a lock, because we know the only threads still running
    // won't modify this - they're already past the point where that's possible.
    for (auto& t : m_ExitingThreads)
    {
        if (t.get_id() != std::this_thread::get_id())
        {
            t.join();
        }
    }
}

//-------------------------------------------------------------------------------------------------
void Scheduler::RetireTask(QueuedTask const& task, std::unique_lock<std::mutex> const& lock) noexcept
{
    assert(lock.owns_lock()); UNREFERENCED_PARAMETER(lock);
    for (auto iter = task.m_QueuedEventsAtTimeOfTaskSubmission; iter->m_Event;)
    {
        int refcount = --iter->m_RefCount;
        if (refcount == 0)
        {
            iter->m_Event.set();
            iter = m_QueuedEvents.erase(iter);
            continue;
        }
        ++iter;
    }
    assert(m_QueuedEvents.size() > 0);
}

}