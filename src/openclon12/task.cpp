// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "task.hpp"
#include "queue.hpp"

/* Event Object APIs */
extern CL_API_ENTRY cl_int CL_API_CALL
clWaitForEvents(cl_uint             num_events,
    const cl_event *    event_list) CL_API_SUFFIX__VERSION_1_0
{
    if (num_events == 0 || event_list == nullptr)
    {
        return CL_INVALID_VALUE;
    }

    // Validation pass
    Context& context = static_cast<Task*>(event_list[0])->m_Parent.get();
    auto ReportError = context.GetErrorReporter();
    for (cl_uint i = 0; i < num_events; ++i)
    {
        Task* t = static_cast<Task*>(event_list[i]);
        if (!t)
        {
            return ReportError("Null event in list.", CL_INVALID_EVENT);
        }
        if (&t->m_Parent.get() != &context)
        {
            return ReportError("Events must all belong to the same context.", CL_INVALID_CONTEXT);
        }
    }

    try
    {
        // Flush pass
        {
            auto Lock = g_Platform->GetTaskPoolLock();
            for (cl_uint i = 0; i < num_events; ++i)
            {
                Task* t = static_cast<Task*>(event_list[i]);
                if (t->GetState() == Task::State::Queued)
                {
                    t->m_CommandQueue->Flush(Lock, /* flushDevice */ true);
                }
            }
        }

        // Wait pass
        for (cl_uint i = 0; i < num_events; ++i)
        {
            Task* t = static_cast<Task*>(event_list[i]);
            cl_int error = t->WaitForCompletion();
            if (error < 0)
            {
                return ReportError("Event status is an error.", CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST);
            }
        }
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error &) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }

    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetEventInfo(cl_event         event,
    cl_event_info    param_name,
    size_t           param_value_size,
    void *           param_value,
    size_t *         param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!event)
    {
        return CL_INVALID_EVENT;
    }

    Task& task = *static_cast<Task*>(event);
    Context& context = task.m_Parent.get();
    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    switch (param_name)
    {
    case CL_EVENT_COMMAND_QUEUE: return RetValue((cl_command_queue)task.m_CommandQueue.Get());
    case CL_EVENT_CONTEXT: return RetValue((cl_context)&context);
    case CL_EVENT_COMMAND_TYPE: return RetValue(task.m_CommandType);
    case CL_EVENT_COMMAND_EXECUTION_STATUS:
    {
        auto Lock = g_Platform->GetTaskPoolLock();
        auto state = task.GetState();
        if (state == Task::State::Ready)
            state = Task::State::Submitted;
        return RetValue((cl_int)state);
    }
    case CL_EVENT_REFERENCE_COUNT: return RetValue(task.GetRefCount());
    }
    return task.m_Parent->GetErrorReporter()("Unknown param_name", CL_INVALID_VALUE);
}

extern CL_API_ENTRY cl_event CL_API_CALL
clCreateUserEvent(cl_context    context_,
    cl_int *      errcode_ret) CL_API_SUFFIX__VERSION_1_1
{
    if (!context_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return nullptr;
    }
    Context& context = *static_cast<Context*>(context_);
    auto ReportError = context.GetErrorReporter(errcode_ret);
    try
    {
        if (errcode_ret) *errcode_ret = CL_SUCCESS;
        return new UserEvent(context);
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error &) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clRetainEvent(cl_event event) CL_API_SUFFIX__VERSION_1_0
{
    if (!event)
    {
        return CL_INVALID_EVENT;
    }
    static_cast<Task*>(event)->Retain();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clReleaseEvent(cl_event event) CL_API_SUFFIX__VERSION_1_0
{
    if (!event)
    {
        return CL_INVALID_EVENT;
    }
    auto task = static_cast<Task*>(event);
    if (task->m_CommandType == CL_COMMAND_USER &&
        (task->m_RefCount & UINT_MAX) == 1 &&
        task->GetState() > (Task::State)0)
    {
        clSetUserEventStatus(event, -1);
    }
    task->Release();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clSetUserEventStatus(cl_event   event,
    cl_int     execution_status) CL_API_SUFFIX__VERSION_1_1
{
    if (!event)
    {
        return CL_INVALID_EVENT;
    }
    Task& task = *static_cast<Task*>(event);
    Context& context = task.m_Parent.get();
    auto ReportError = context.GetErrorReporter();
    if (task.m_CommandType != CL_COMMAND_USER)
    {
        return ReportError("Can only use clSetUserEventStatus on user events.", CL_INVALID_EVENT);
    }
    if (execution_status > 0)
    {
        return ReportError("Can only set event status to CL_SUCCESS or a negative error code.", CL_INVALID_VALUE);
    }

    if (task.GetState() != Task::State::Submitted)
    {
        return ReportError("Task event has already been modified.", CL_INVALID_OPERATION);
    }

    try
    {
        UserEvent& e = static_cast<UserEvent&>(task);
        auto Lock = g_Platform->GetTaskPoolLock();
        e.Complete(execution_status, Lock);
        for (cl_uint i = 0; i < context.GetDeviceCount(); ++i)
        {
            context.GetD3DDevice(i).Flush(Lock);
        }
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clSetEventCallback(cl_event    event,
    cl_int      command_exec_callback_type,
    void (CL_CALLBACK * pfn_notify)(cl_event event,
        cl_int   event_command_status,
        void *   user_data),
    void *      user_data) CL_API_SUFFIX__VERSION_1_1
{
    if (!event)
    {
        return CL_INVALID_EVENT;
    }
    Task& task = *static_cast<Task*>(event);
    Context& context = task.m_Parent.get();
    auto ReportError = context.GetErrorReporter();
    if (!pfn_notify)
    {
        return ReportError("Must provide a notification function.", CL_INVALID_VALUE);
    }
    switch (command_exec_callback_type)
    {
    case CL_COMPLETE:
    case CL_RUNNING:
    case CL_SUBMITTED:
        break;
    default:
        return ReportError("Invalid command_exec_callback_type", CL_INVALID_VALUE);
    }

    try
    {
        task.RegisterCallback(command_exec_callback_type, pfn_notify, user_data);
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    return CL_SUCCESS;
}

/* Profiling APIs */
extern CL_API_ENTRY cl_int CL_API_CALL
clGetEventProfilingInfo(cl_event            event,
    cl_profiling_info   param_name,
    size_t              param_value_size,
    void *              param_value,
    size_t *            param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!event)
    {
        return CL_INVALID_EVENT;
    }

    Task& task = *static_cast<Task*>(event);
    Context& context = task.m_Parent.get();
    auto ReportError = context.GetErrorReporter();
    if (!task.GetTimestamp(CL_PROFILING_COMMAND_QUEUED))
    {
        return ReportError("Timestamps not available.", CL_PROFILING_INFO_NOT_AVAILABLE);
    }
    if (task.GetState() != Task::State::Complete)
    {
        return ReportError("Event not complete.", CL_PROFILING_INFO_NOT_AVAILABLE);
    }

    cl_ulong Time;
    switch (param_name)
    {
    case CL_PROFILING_COMMAND_QUEUED:
    case CL_PROFILING_COMMAND_SUBMIT:
    case CL_PROFILING_COMMAND_START:
    case CL_PROFILING_COMMAND_END:
    case CL_PROFILING_COMMAND_COMPLETE:
        Time = task.GetTimestamp(param_name);
        break;
    default:
        return ReportError("Invalid param_name", CL_INVALID_VALUE);
    }

    return CopyOutParameter(Time, param_value_size, param_value, param_value_size_ret);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueMarkerWithWaitList(cl_command_queue  command_queue,
    cl_uint           num_events_in_wait_list,
    const cl_event *  event_wait_list,
    cl_event *        event) CL_API_SUFFIX__VERSION_1_2
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();

    if ((event_wait_list == nullptr) != (num_events_in_wait_list == 0))
    {
        return ReportError("If event_wait_list is null, then num_events_in_wait_list mut be zero, and vice versa.", CL_INVALID_EVENT_WAIT_LIST);
    }

    try
    {
        std::unique_ptr<Task> task(new DummyTask(context, CL_COMMAND_MARKER, command_queue));

        auto Lock = g_Platform->GetTaskPoolLock();
        if (num_events_in_wait_list)
        {
            task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        }
        else
        {
            queue.AddAllTasksAsDependencies(task.get(), Lock);
        }

        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }

    return CL_SUCCESS;
}

extern CL_API_ENTRY CL_API_PREFIX__VERSION_1_1_DEPRECATED cl_int CL_API_CALL
clEnqueueMarker(cl_command_queue    command_queue,
    cl_event* event) CL_API_SUFFIX__VERSION_1_1_DEPRECATED
{
    return clEnqueueMarkerWithWaitList(command_queue, 0, nullptr, event);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueBarrierWithWaitList(cl_command_queue  command_queue,
    cl_uint           num_events_in_wait_list,
    const cl_event *  event_wait_list,
    cl_event *        event) CL_API_SUFFIX__VERSION_1_2
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();

    if ((event_wait_list == nullptr) != (num_events_in_wait_list == 0))
    {
        return ReportError("If event_wait_list is null, then num_events_in_wait_list mut be zero, and vice versa.", CL_INVALID_EVENT_WAIT_LIST);
    }

    try
    {
        std::unique_ptr<Task> task(new DummyTask(context, CL_COMMAND_BARRIER, command_queue));

        auto Lock = g_Platform->GetTaskPoolLock();
        if (num_events_in_wait_list)
        {
            task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        }
        else
        {
            queue.AddAllTasksAsDependencies(task.get(), Lock);
        }

        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }

    return CL_SUCCESS;
}

extern CL_API_ENTRY CL_API_PREFIX__VERSION_1_1_DEPRECATED cl_int CL_API_CALL
clEnqueueWaitForEvents(cl_command_queue  command_queue,
    cl_uint          num_events,
    const cl_event* event_list) CL_API_SUFFIX__VERSION_1_1_DEPRECATED
{
    return clEnqueueBarrierWithWaitList(command_queue, num_events, event_list, nullptr);
}

extern CL_API_ENTRY CL_API_PREFIX__VERSION_1_1_DEPRECATED cl_int CL_API_CALL
clEnqueueBarrier(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_1_DEPRECATED
{
    return clEnqueueBarrierWithWaitList(command_queue, 0, nullptr, nullptr);
}

void Task::Record()
{
    ImmCtx *pImmCtx = nullptr;
    if (m_CommandQueue.Get())
    {
        pImmCtx = &m_CommandQueue->GetD3DDevice().ImmCtx();
    }
    if (GetTimestamp(CL_PROFILING_COMMAND_QUEUED))
    {
        try
        {
            // TODO: Maybe share a start timestamp with the end of the previous command?
            m_StartTimestamp.reset(new D3D12TranslationLayer::TimestampQuery(pImmCtx));
            m_StopTimestamp.reset(new D3D12TranslationLayer::TimestampQuery(pImmCtx));
        }
        catch(...) { /* Do nothing, just don't capture timestamps */ }
    }

    if (m_StartTimestamp)
    {
        m_StartTimestamp->End();
    }
    RecordImpl();
    if (m_StopTimestamp)
    {
        m_StopTimestamp->End();
    }
}

cl_ulong& Task::GetTimestamp(cl_profiling_info timestampType)
{
    switch (timestampType)
    {
    case CL_PROFILING_COMMAND_QUEUED:
    case CL_PROFILING_COMMAND_SUBMIT:
    case CL_PROFILING_COMMAND_START:
    case CL_PROFILING_COMMAND_END:
        return m_ProfilingTimestamps[timestampType - CL_PROFILING_COMMAND_QUEUED];
    case CL_PROFILING_COMMAND_COMPLETE:
        return GetTimestamp(CL_PROFILING_COMMAND_END);
    default:
        assert(false);
        return m_ProfilingTimestamps[0];
    }
}

Task::Task(Context& Parent, cl_command_type command_type, cl_command_queue command_queue)
    : CLChildBase(Parent)
    , m_CommandQueue(static_cast<CommandQueue*>(command_queue))
    , m_Device(command_queue ? &m_CommandQueue->GetDevice() : nullptr)
    , m_D3DDevice(command_queue ? &m_CommandQueue->GetD3DDevice() : nullptr)
    , m_CommandType(command_type)
{
    if (m_CommandQueue.Get() && m_CommandQueue->m_bProfile)
    {
        LARGE_INTEGER li = {};
        QueryPerformanceCounter(&li);
        GetTimestamp(CL_PROFILING_COMMAND_QUEUED) = TimestampFromQPC();
    }
}

Task::Task(Context& Parent, D3DDevice& device)
    : CLChildBase(Parent)
    , m_Device(&device.GetParent())
    , m_D3DDevice(&device)
    , m_CommandType(0)
{
}

Task::~Task()
{
    // This should only ever be invoked with non-empty waiting lists in the case of failed construction
    for (auto& task : m_TasksToWaitOn)
    {
        task->m_TasksWaitingOnThis.erase(
            std::find_if(task->m_TasksWaitingOnThis.begin(), task->m_TasksWaitingOnThis.end(),
                [this](ref_ptr_int const& p) { return p.Get() == this; }));
    }
}

cl_ulong Task::TimestampToNanoseconds(cl_ulong Ticks, cl_ulong Frequency)
{
    return (cl_ulong)((double)Ticks * (1000000.0 / Frequency));
}

cl_ulong Task::TimestampFromQPC()
{
    LARGE_INTEGER ticks, frequency;
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return TimestampToNanoseconds(ticks.QuadPart, frequency.QuadPart);
}

void Task::AddDependencies(const cl_event* event_wait_list, cl_uint num_events_in_wait_list, TaskPoolLock const&)
{
    if (num_events_in_wait_list)
    {
        m_TasksToWaitOn.reserve(m_TasksToWaitOn.size() + num_events_in_wait_list);
        try
        {
            for (UINT i = 0; i < num_events_in_wait_list; ++i)
            {
                Task* task = static_cast<Task*>(event_wait_list[i]);
                if (&task->m_Parent.get() != &m_Parent.get())
                {
                    throw DependencyException {};
                }
                if (task->m_D3DDevice != m_D3DDevice ||
                    task->GetState() == Task::State::Queued ||
                    task->GetState() == Task::State::Submitted)
                {
                    auto insertRet = task->m_TasksWaitingOnThis.insert(this);
                    if (insertRet.second)
                    {
                        m_TasksToWaitOn.emplace_back(task);
                    }
                }
            }
        }
        catch (...)
        {
            // Clean up references to this
            for (auto& task : m_TasksToWaitOn)
            {
                task->m_TasksWaitingOnThis.erase(
                    std::find_if(task->m_TasksWaitingOnThis.begin(), task->m_TasksWaitingOnThis.end(),
                        [this](ref_ptr_int const& p) { return p.Get() == this; }));
            }
            throw;
        }
    }
}

cl_int Task::WaitForCompletion()
{
    m_CompletionFuture.wait();
    return (cl_int)m_State;
}

void Task::RegisterCallback(cl_int command_exec_callback_type, NotificationRequest::Fn pfn_notify, void* user_data)
{
    // If the state is already satisfied, fire the callback immediately once we release the lock
    bool bCallNotification = false;
    cl_int StateToSend = 0;
    {
        auto Lock = g_Platform->GetTaskPoolLock();
        if ((cl_int)GetState() <= command_exec_callback_type)
        {
            bCallNotification = true;
            StateToSend = command_exec_callback_type == CL_COMPLETE ? (cl_int)GetState() : command_exec_callback_type;
        }

        if (!bCallNotification)
        {
            auto& NotificationList = [this, command_exec_callback_type]() -> std::vector<NotificationRequest>&
            {
                switch (command_exec_callback_type)
                {
                case CL_SUBMITTED: return m_SubmittedCallbacks;
                case CL_RUNNING: return m_RunningCallbacks;
                default:
                case CL_COMPLETE: return m_CompletionCallbacks;
                }
            }();
            NotificationList.push_back(NotificationRequest{ pfn_notify, user_data });
        }
    }
    if (bCallNotification)
    {
        pfn_notify(this, StateToSend, user_data);
    }
}

void Task::Submit()
{
    m_State = State::Submitted;
    if (GetTimestamp(CL_PROFILING_COMMAND_QUEUED) != 0)
    {
        GetTimestamp(CL_PROFILING_COMMAND_SUBMIT) = TimestampFromQPC();
    }
    FireNotifications();
}

void Task::Ready(TaskPoolLock const& lock)
{
    m_State = State::Ready;
    for (auto &task : m_TasksWaitingOnThis)
    {
        assert(task->m_CommandQueue.Get() || task->m_D3DDevice);
        if (task->m_D3DDevice != m_D3DDevice)
        {
            continue;
        }

        auto newEnd = std::remove_if(task->m_TasksToWaitOn.begin(), task->m_TasksToWaitOn.end(),
                                     [this](ref_ptr_int const &p) { return p.Get() == this; });
        assert(newEnd != task->m_TasksToWaitOn.end());
        task->m_TasksToWaitOn.erase(newEnd, task->m_TasksToWaitOn.end());

        if (task->m_TasksToWaitOn.empty() &&
            task->m_State == State::Submitted)
        {
            task->m_D3DDevice->ReadyTask(task.Get(), lock);
        }
    }
}

void Task::Started(TaskPoolLock const &)
{
    m_State = State::Running;
    FireNotifications();
}

void Task::Complete(cl_int error, TaskPoolLock const& lock)
{
    assert(error <= 0);
    if (m_State <= State::Complete)
    {
        return;
    }
    m_State = (State)error;

    if (m_CommandQueue.Get())
    {
        m_CommandQueue->NotifyTaskCompletion(this, lock);
    }

    if (m_StartTimestamp || m_StopTimestamp)
    {
        assert(m_CommandQueue.Get() && m_D3DDevice);
        UINT64 Frequency = m_D3DDevice->GetTimestampFrequency();
        UINT64 GPUTimestamp;
        if (m_StartTimestamp)
        {
            GPUTimestamp = m_StartTimestamp->GetData();
            GetTimestamp(CL_PROFILING_COMMAND_START) =
                TimestampToNanoseconds(GPUTimestamp, Frequency) + m_D3DDevice->GPUToQPCTimestampOffset();
        }
        if (m_StopTimestamp)
        {
            GPUTimestamp = m_StopTimestamp->GetData();
            GetTimestamp(CL_PROFILING_COMMAND_END) =
                TimestampToNanoseconds(GPUTimestamp, Frequency) + m_D3DDevice->GPUToQPCTimestampOffset();
        }
    }

    if (error >= 0)
    {
        // Perform any on-complete type work, such as CPU copies of memory
        OnComplete();
    }

    FireNotifications();

    if (error < 0)
    {
        for (auto& task : m_TasksWaitingOnThis)
        {
            if (task->m_State >= State::Running)
            {
                task->Complete(error, lock);
            }
        }
    }
    else
    {
        for (auto& task : m_TasksWaitingOnThis)
        {
            assert(task->m_CommandQueue.Get() || task->m_D3DDevice);

            auto newEnd = std::remove_if(task->m_TasksToWaitOn.begin(), task->m_TasksToWaitOn.end(),
                [this](ref_ptr_int const& p) { return p.Get() == this; });
            if (newEnd == task->m_TasksToWaitOn.end())
            {
                continue;
            }

            task->m_TasksToWaitOn.erase(newEnd, task->m_TasksToWaitOn.end());

            if (task->m_TasksToWaitOn.empty() &&
                task->m_State == State::Submitted)
            {
                task->m_D3DDevice->ReadyTask(task.Get(), lock);
            }
        }
    }

    m_TasksToWaitOn.clear();
    m_TasksWaitingOnThis.clear();
    m_CompletionPromise.set_value();
}

void Task::FireNotification(NotificationRequest const& callback, cl_int state)
{
    g_Platform->QueueCallback([=]()
    {
        callback.m_pfn(this, state, callback.m_userData);
    });
}

void Task::FireNotifications()
{
    switch (m_State)
    {
    default: // Error states
    case State::Complete:
        for (auto& c : m_CompletionCallbacks) FireNotification(c, (cl_int)m_State);
        m_CompletionCallbacks.clear();
        // Fallthrough
    case State::Running:
        for (auto& c : m_RunningCallbacks) FireNotification(c, CL_RUNNING);
        m_RunningCallbacks.clear();
        // Fallthrough
    case State::Ready:
    case State::Submitted:
        for (auto& c : m_SubmittedCallbacks) FireNotification(c, CL_SUBMITTED);
        m_SubmittedCallbacks.clear();
        break;
    case State::Queued:
        break;
    }
}

UserEvent::UserEvent(Context& Parent)
    : Task(Parent, CL_COMMAND_USER, nullptr)
{
    Submit();
}

DummyTask::DummyTask(Context & Parent, cl_command_type type, cl_command_queue command_queue)
    : Task(Parent, type, command_queue)
{
}
