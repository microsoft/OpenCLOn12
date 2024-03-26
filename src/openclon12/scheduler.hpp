// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <list>
#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <XPlatHelpers.h>

namespace BackgroundTaskScheduler
{
    enum class Priority { Idle, Normal };
    struct SchedulingMode
    {
        uint32_t NumThreads;
        Priority ThreadPriority;
        bool operator==(SchedulingMode const& b) { return NumThreads == b.NumThreads && ThreadPriority == b.ThreadPriority; }
        bool operator!=(SchedulingMode const& b) { return !(*this == b); }
        bool operator>(SchedulingMode const& b) { return NumThreads > b.NumThreads || (int)ThreadPriority > (int)b.ThreadPriority; }
    };

    struct Task
    {
        using FnType = void(APIENTRY*)(void* pContext);
        FnType m_Callback;
        FnType m_Cancel;
        void* m_pContext;
    };

    class Scheduler
    {
    protected:
        struct QueuedEventSignal
        {
            std::atomic<long> m_RefCount;
            XPlatHelpers::unique_event m_Event;
        };
        std::list<QueuedEventSignal> m_QueuedEvents;
        std::list<QueuedEventSignal>::iterator m_QueuedEventsPseudoEnd;

        struct QueuedTask : Task
        {
            std::list<QueuedEventSignal>::iterator m_QueuedEventsAtTimeOfTaskSubmission;
            QueuedTask() = default;
            QueuedTask(Task const& t, decltype(m_QueuedEventsAtTimeOfTaskSubmission) iter)
                : Task(t), m_QueuedEventsAtTimeOfTaskSubmission(iter)
            {
            }
            QueuedTask(QueuedTask const&) = default;
            QueuedTask(QueuedTask&&) = default;
            QueuedTask& operator=(QueuedTask const&) = default;
            QueuedTask& operator=(QueuedTask&&) = default;
        };

        // These are the tasks that are waiting for a thread to consume them.
        std::deque<QueuedTask> m_Tasks;
        // This is a counter of how many tasks are currently being processed by
        // worker threads. Adding this to the size of m_Tasks enables determining
        // the total number of currently not-completed tasks.
        uint32_t m_TasksInProgress = 0;
        std::vector<std::thread> m_Threads;
        std::vector<std::thread> m_ExitingThreads;
        mutable std::mutex m_Lock;
        std::condition_variable m_CV;

        SchedulingMode m_CurrentMode = { 0, Priority::Idle };
        SchedulingMode m_EffectiveMode = { 0, Priority::Idle };
        bool m_bShutdown = false;

        // These methods require the lock to be held.
        // Const-ref methods just require it, non-const-ref methods may release it.
        bool IsSchedulerIdle(std::unique_lock<std::mutex> const&) const noexcept { return m_Tasks.empty() && m_TasksInProgress == 0; }
        void SetSchedulingModeImpl(SchedulingMode mode, std::unique_lock<std::mutex>& lock); // Releases lock
        void QueueSetSchedulingModeTask(SchedulingMode mode, std::unique_lock<std::mutex> const&);
        void RetireTask(QueuedTask const& task, std::unique_lock<std::mutex> const&) noexcept;

        // These methods will take the lock.
        void SetSchedulingModeTask(SchedulingMode mode) noexcept;
        static void __stdcall SetSchedulingModeTaskStatic(void* pContext);
        void TaskThread(int ThreadID) noexcept;

    public:
        Scheduler();
        ~Scheduler() { Shutdown(); }

        void SetSchedulingMode(SchedulingMode mode);
        void QueueTask(Task task);
        void SignalEventOnCompletionOfCurrentTasks(XPlatHelpers::Event hEvent, SchedulingMode modeAfterSignal);
        void CancelExistingTasks() noexcept;
        void Shutdown() noexcept;

        SchedulingMode GetCurrentMode() const
        {
            std::lock_guard<std::mutex> lock(m_Lock);
            return m_CurrentMode;
        }
        SchedulingMode GetEffectiveMode() const
        {
            std::lock_guard<std::mutex> lock(m_Lock);
            return m_EffectiveMode;
        }
    };
}