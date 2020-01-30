#pragma once
#include "platform.hpp"
#include "context.hpp"
#include <mutex>
#include <future>

constexpr bool c_RecordCommandListsOnAppThreads = false;

// A task is an encapsulation of something that can be submitted to a command queue
// and/or something that can be waited on (i.e. cl_event).
//
// A task goes through several state transitions:
// 1. A task is created in the 'queued' state. This means it's in a command queue
//    but hasn't yet been submitted to the 'device.'
//    It goes through a state transition when its corresponding command queue is flushed.
// 2. After the queue is flushed, the task enteres the 'submitted' state. In this state,
//    the device needs to check conditions on the task before deciding to schedule it.
//    A task can have other tasks that it's dependent on. It stays in this state until
//    those dependencies are satisfied.
// 3. Once all dependencies are satisfied, the task enters the 'ready' state.
//    In the ready state, the work in the task is free to be scheduled by the device
//    whenever the device is ready to execute more work. Note that from the API there
//    is no distinction between the submitted and ready states.
//    Therefore, this state is more like a pseudo-state that only exists in theory.
//    This implementation does not have an explicit notion of this state.
// 4. Once the device has started working on a given task, it enters the 'running' state.
// 5. Once the device finishes all work associated with the task, but not necessarily
//    work that was submitted to a device-side queue by the task, it enters the 'ended' state.
// 6. Once all work associated with the task and its children are done, it enters the 'complete' state.
//    The distinction between ended and complete only matters for tasks which are kernel enqueues,
//    and which submit child kernel executions to a device queue.
//    This implementation does not support device queues.
//
// These state transitions are visible out of the API in two ways, both exposed via events.
// Every time a task is created, the developer has the opportunity to request an event for that task.
// 1. The event can be polled for the current state of the associated task, as well as have
//    callbacks registered on it for event state changes. Neither the 'ready' nor 'ended' state are visible here.
// 2. If the task was submitted to a queue with profiling enabled, then the event can also
//    be queried for profiling information: specifically, the timestamp at which point
//    the task entered a particular state. Note that the 'ready' state does not have a timestamp.
//
// This implementation implements tasks in the following way:
// - Any time an 'enqueue' API is called, a task object is created in the 'queued' state.
// - The task object implements the event API.
// - The task object sits in a command queue.
// - The task has a list of all other tasks that it is dependent on, as well as a list of tasks
//   that are dependent on it.
// --- The list of backward dependencies is pruned as dependencies are satisfied.
// --- The list of forwards dependencies is cleared upon task completion.
// - If a task is submitted to an in-order command queue (TBD whether out-of-order is even supported),
//   then the task immediately preceeding it in the queue is added as a dependency.
// - When a command queue is flushed, all tasks in the queue are submitted to the device and become 'submitted'.
// --- Note that the command queue needs to keep track of what tasks it has submitted but are not yet
//     complete, for the purposes of implementing 'finish', markers, and barriers, as well as specifically
//     the last task that was submitted for in-order queues.
// - Tasks that are submitted with no dependencies are considered 'ready'.
//
// - If c_RecordCommandListsOnAppThreads, ready tasks are recorded into a command list during Flush.
// --- When a task is recorded, all tasks dependent on it are notified, and a set of additional tasks may also become 'ready.'
// --- If profiling is enabled, these tasks are bracketed with timestamps.
// --- The act of 'recording' a task is done via virtual function, delegating to the specific type of task.
// --- Dependencies are inspected to determine if barriers are needed.
// --- This process repeats until all tasks that can be are recorded, and then the command list is submitted.
//     All tasks associated with that given command list enter the 'running' state.
// --- Note: as far as I can tell this is technically in violation of the OpenCL spec: tasks should not be considered
//     'ready' until all dependent tasks are 'complete.' In this implementation, you can have a task that is ready
//     when its dependencies are also only ready. This seems like a worthy improvement in throughput.
// --- When the command list completes, a worker thread is woken up. This worker is responsible for marking all
//     tasks from the command list as 'complete.' It's also responsible for copying timestamp information out of
//     query buffers into the corresponding tasks.
//
// - If not c_RecordCommandListsOnAppThreads, ready tasks are added to a list.
// --- At the end of the flush operation, a work item is created for a worker thread to execute all ready tasks.
// --- After recording all ready tasks into a command list, the command list is submitted, and the thread waits for it to complete.
// --- Then, all tasks that were part of that command list are marked complete. This enables new tasks to be marked ready.
// --- If there are any newly ready tasks, then another worker thread work item is created to execute those.
// --- Note that no barriers are required between tasks recorded into a single command list, because they have no dependencies on each other.
// --- This model is more conformant, and enables latency-hiding for tasks which require additional shader compilations,
//     but will result in less overall throughput.

class Task : public CLChildBase<Task, Context, cl_event>
{
    struct NotificationRequest
    {
        using Fn = void(CL_CALLBACK *)(cl_event, cl_int, void*);
        Fn m_pfn;
        void* m_userData;
    };

public:
    friend class Device;
    enum class State
    {
        // API-visible states (sorted in reverse order so CL_COMPLETE == CL_SUCCESS == 0)
        Complete = CL_COMPLETE,
        Running = CL_RUNNING,
        Submitted = CL_SUBMITTED,
        Queued = CL_QUEUED,

        // Internal states
        Ready,
    };

    void Record();
    State GetState() const { return m_State; }
    cl_ulong& GetTimestamp(cl_profiling_info timestampType);

    void AddDependencies(const cl_event* event_wait_list, cl_uint num_events_in_wait_list, TaskPoolLock const&);
    cl_int WaitForCompletion();
    void RegisterCallback(cl_int command_exec_callback_type, NotificationRequest::Fn pfn_notify, void* user_data);

    const cl_command_type m_CommandType;
    const ::ref_ptr_int<class CommandQueue> m_CommandQueue;

    Task(Context& Parent, cl_command_type command_type, cl_command_queue command_queue);
    virtual ~Task();

    static cl_ulong TimestampToNanoseconds(cl_ulong Ticks, cl_ulong Frequency);
    static cl_ulong TimestampFromQPC();

protected:
    void Submit();
    // TODO: Encode barriers into here at some point.
    void Ready(std::vector<ref_ptr_int>& OtherReadyTasks, TaskPoolLock const&);
    void Started(TaskPoolLock const&);
    void Complete(cl_int error, TaskPoolLock const&);

    virtual void RecordImpl() = 0;
    virtual void OnComplete() { }

    void FireNotification(NotificationRequest const& callback, cl_int state);
    void FireNotifications();

    // State changes can only be made while holding the task pool lock
    State m_State = State::Queued;
    cl_ulong m_ProfilingTimestamps[4] = {};

    std::vector<ref_ptr> m_TasksToWaitOn;
    std::vector<ref_ptr_int> m_TasksWaitingOnThis;
    std::vector<NotificationRequest> m_CompletionCallbacks;
    std::vector<NotificationRequest> m_RunningCallbacks;
    std::vector<NotificationRequest> m_SubmittedCallbacks;
    std::promise<void> m_CompletionPromise;

    std::shared_ptr<D3D12TranslationLayer::Query> m_StartTimestamp;
    std::shared_ptr<D3D12TranslationLayer::Query> m_StopTimestamp;
};

class UserEvent : public Task
{
public:
    UserEvent(Context& parent);
    using Task::Complete;

private:
    void RecordImpl() final { }
};

class Marker : public Task
{
public:
    Marker(Context& Parent, cl_command_queue command_queue);

private:
    void RecordImpl() final { }
};

class Barrier : public Task
{
public:
    Barrier(Context& Parent, cl_command_queue command_queue);

private:
    void RecordImpl() final { }
};
