// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "platform.hpp"
#include "context.hpp"
#include <mutex>
#include <future>

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
// --- The list of forwards dependencies is cleared upon task completion/readiness.
// - If a task is submitted to an in-order command queue, then the task immediately preceeding
//   it in the queue is added as a dependency.
// - When a command queue is flushed, all tasks in the queue are submitted to the device and become 'submitted'.
// --- Note that the command queue needs to keep track of what tasks it has submitted but are not yet
//     complete, for the purposes of implementing 'finish', markers, and barriers, as well as specifically
//     the last task that was submitted for in-order queues.
// - Tasks that are submitted with no dependencies are considered 'ready'. Ready tasks are added to a list.
// --- When a task becomes ready, any other tasks that depend on it that can execute on the same device will also
//     be marked ready. Technically, this is a violation of the CL spec for events, because it means that given task A
//     and task B, where B depends on A, both A and B can be considered 'running' at the same time. The CL spec explicitly
//     says that an event should only be marked running when previous events are 'complete', but this seems like a more
//     desireable design than the one imposed by the spec.
// --- At the end of the flush operation, a work item is created for a worker thread to execute all ready tasks.
// --- After recording all ready tasks into a command list, the command list is submitted, and the thread waits for it to complete.
//     All tasks that were part of the command list are considered to be running at this point.
// --- Then, all tasks that were part of that command list are marked complete. This enables new tasks to be marked ready.
// --- If there are any newly ready tasks, then another worker thread work item is created to execute those.

class Task : public CLChildBase<Task, Context, cl_event>
{
    struct NotificationRequest
    {
        using Fn = void(CL_CALLBACK *)(cl_event, cl_int, void*);
        Fn m_pfn;
        void* m_userData;
    };

public:
    struct DependencyException {};
    friend class D3DDevice;
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
    const ::ref_ptr_int<class Device> m_Device;
    class D3DDevice *const m_D3DDevice;

    Task(Context& Parent, cl_command_type command_type, cl_command_queue command_queue);
    Task(Context& Parent, D3DDevice& device);
    virtual ~Task();

    static cl_ulong TimestampToNanoseconds(cl_ulong Ticks, cl_ulong Frequency);
    static cl_ulong TimestampFromQPC();

protected:
    void Submit();
    void Ready(TaskPoolLock const&);
    void Started(TaskPoolLock const&);
    void Complete(cl_int error, TaskPoolLock const&);

    virtual void MigrateResources() = 0;
    virtual void RecordImpl() = 0;
    virtual void OnComplete() { }

    void FireNotification(NotificationRequest const& callback, cl_int state);
    void FireNotifications();

    // State changes can only be made while holding the task pool lock
    State m_State = State::Queued;
    cl_ulong m_ProfilingTimestamps[4] = {};

    std::vector<ref_ptr_int> m_TasksToWaitOn;
    std::set<ref_ptr_int> m_TasksWaitingOnThis;
    std::vector<NotificationRequest> m_CompletionCallbacks;
    std::vector<NotificationRequest> m_RunningCallbacks;
    std::vector<NotificationRequest> m_SubmittedCallbacks;
    std::promise<void> m_CompletionPromise;
    std::future<void> m_CompletionFuture{ m_CompletionPromise.get_future() };

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
    void MigrateResources() final { }
};

class DummyTask : public Task
{
public:
    DummyTask(Context& Parent, cl_command_type type, cl_command_queue command_queue);

private:
    void RecordImpl() final { }
    void MigrateResources() final { }
};

class Resource;
// Map tasks will be enqueued on a queue, and also tracked on the resource which has been mapped.
// That'll allow Unmap to do the right thing, by looking up the type of map operation that was
// done for a given pointer.
class MapTask : public Task
{
public:
    struct Args
    {
        cl_uint SrcX;
        cl_uint SrcY;
        cl_uint SrcZ;
        cl_uint Width;
        cl_uint Height;
        cl_uint Depth;
        cl_ushort FirstArraySlice;
        cl_ushort NumArraySlices;
        cl_uchar FirstMipLevel;
    };

    MapTask(Context& Parent, cl_command_queue command_queue, Resource& resource, cl_map_flags flags, cl_command_type command, Args const& args);
    ~MapTask();
    virtual void Unmap(bool IsResourceBeingDestroyed) = 0;
    void* GetPointer() const { return m_Pointer; }
    size_t GetRowPitch() const { return m_RowPitch; }
    size_t GetSlicePitch() const { return m_SlicePitch; }
    Resource& GetResource() const { return m_Resource; }

protected:
    void* m_Pointer = nullptr;
    size_t m_RowPitch = 0, m_SlicePitch = 0;
    Resource& m_Resource;
    const cl_map_flags m_MapFlags;
    const Args m_Args;

    void OnComplete() override;
    void MigrateResources() override;
};
