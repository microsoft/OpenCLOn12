// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "queue.hpp"

/* Command Queue APIs */

template <typename TReporter>
bool ValidateQueueProperties(cl_queue_properties const* properties, TReporter&& ReportError)
{
    constexpr cl_queue_properties KnownProperties[] =
    {
        CL_QUEUE_PROPERTIES
    };
    bool SeenProperties[std::extent_v<decltype(KnownProperties)>] = {};
    for (auto CurProp = properties; properties && *CurProp; CurProp += 2)
    {
        auto KnownPropIter = std::find(KnownProperties, std::end(KnownProperties), *CurProp);
        if (KnownPropIter == std::end(KnownProperties))
        {
            return !ReportError("Unknown property.", CL_INVALID_PROPERTY);
        }

        auto PropIndex = std::distance(KnownProperties, KnownPropIter);
        if (SeenProperties[PropIndex])
        {
            return !ReportError("Property specified twice.", CL_INVALID_PROPERTY);
        }

        SeenProperties[PropIndex] = true;
    }

    return true;
}

static cl_command_queue
clCreateCommandQueueWithPropertiesImpl(cl_context               context_,
    cl_device_id             device_,
    const cl_queue_properties *    properties,
    cl_int *                 errcode_ret,
    bool synthesized_properties)
{
    if (!context_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return nullptr;
    }
    Context& context = *static_cast<Context*>(context_);
    auto ReportError = context.GetErrorReporter(errcode_ret);

    if (!device_)
    {
        return ReportError("Invalid device.", CL_INVALID_DEVICE);
    }
    Device& device = *static_cast<Device*>(device_);
    D3DDevice *d3dDevice = context.D3DDeviceForContext(device);
    if (!d3dDevice)
    {
        return ReportError("Provided device not associated with provided context.", CL_INVALID_DEVICE);
    }

    if (!ValidateQueueProperties(properties, ReportError))
    {
        return nullptr;
    }

    cl_command_queue_properties PropertyBits = 0;
    if (auto FoundPropertyBits = FindProperty<cl_queue_properties>(properties, CL_QUEUE_PROPERTIES);
        FoundPropertyBits != nullptr)
    {
        PropertyBits = static_cast<cl_command_queue_properties>(*FoundPropertyBits);
    }
    constexpr cl_command_queue_properties ValidPropertyBits = CL_QUEUE_PROFILING_ENABLE | CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE;
    if (PropertyBits & ~ValidPropertyBits)
    {
        return ReportError("Invalid properties specified.", CL_INVALID_QUEUE_PROPERTIES);
    }

    try
    {
        if (errcode_ret) *errcode_ret = CL_SUCCESS;
        return new CommandQueue(*d3dDevice, context, properties, synthesized_properties);
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error &) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_command_queue CL_API_CALL
clCreateCommandQueueWithProperties(cl_context               context,
    cl_device_id             device,
    const cl_queue_properties *    properties,
    cl_int *                 errcode_ret) CL_API_SUFFIX__VERSION_2_0
{
    return clCreateCommandQueueWithPropertiesImpl(context, device, properties, errcode_ret, false);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clRetainCommandQueue(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    static_cast<CommandQueue*>(command_queue)->Retain();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clReleaseCommandQueue(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    cl_int flushRet = clFlush(command_queue);
    static_cast<CommandQueue *>(command_queue)->Release();
    return flushRet;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetCommandQueueInfo(cl_command_queue      command_queue,
    cl_command_queue_info param_name,
    size_t                param_value_size,
    void *                param_value,
    size_t *              param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    auto ReportError = queue.GetContext().GetErrorReporter();

    switch (param_name)
    {
    case CL_QUEUE_CONTEXT: return RetValue(static_cast<cl_context>(&queue.GetContext()));
    case CL_QUEUE_DEVICE: return RetValue(static_cast<cl_device_id>(&queue.GetDevice()));
    case CL_QUEUE_REFERENCE_COUNT: return RetValue((cl_uint)queue.GetRefCount());
    case CL_QUEUE_PROPERTIES: 
        return RetValue(*FindProperty<cl_queue_properties>(queue.m_Properties.data(), CL_QUEUE_PROPERTIES));
    case CL_QUEUE_PROPERTIES_ARRAY:
        if (queue.m_bPropertiesSynthesized)
            return RetValue(nullptr);
        return CopyOutParameterImpl(queue.m_Properties.data(),
            queue.m_Properties.size() * sizeof(queue.m_Properties[0]),
            param_value_size, param_value, param_value_size_ret);
    case CL_QUEUE_SIZE: return ReportError("Queue is not a device queue", CL_INVALID_COMMAND_QUEUE);
    case CL_QUEUE_DEVICE_DEFAULT: return RetValue((cl_command_queue)nullptr);
    }

    return ReportError("Unknown param_name", CL_INVALID_VALUE);
}

/*
 *  WARNING:
 *     This API introduces mutable state into the OpenCL implementation. It has been REMOVED
 *  to better facilitate thread safety.  The 1.0 API is not thread safe. It is not tested by the
 *  OpenCL 1.1 conformance test, and consequently may not work or may not work dependably.
 *  It is likely to be non-performant. Use of this API is not advised. Use at your own risk.
 *
 *  Software developers previously relying on this API are instructed to set the command queue
 *  properties when creating the queue, instead.
 */
extern CL_API_ENTRY cl_int CL_API_CALL
clSetCommandQueueProperty(cl_command_queue  command_queue,
    cl_command_queue_properties,
    cl_bool,
    cl_command_queue_properties*) CL_API_SUFFIX__VERSION_1_0_DEPRECATED
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    return ReportError("clSetCommandQueueProperty is deprecated", CL_INVALID_OPERATION);
}

extern CL_API_ENTRY CL_API_PREFIX__VERSION_1_2_DEPRECATED cl_command_queue CL_API_CALL
clCreateCommandQueue(cl_context                     context,
    cl_device_id                   device,
    cl_command_queue_properties    properties,
    cl_int *                       errcode_ret) CL_API_SUFFIX__VERSION_1_2_DEPRECATED
{
    cl_queue_properties PropArray[3] = { CL_QUEUE_PROPERTIES, properties, 0 };
    return clCreateCommandQueueWithPropertiesImpl(context, device, PropArray, errcode_ret, true);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clFlush(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto ReportError = queue.GetContext().GetErrorReporter();
    try
    {
        queue.Flush(g_Platform->GetTaskPoolLock(), /* flushDevice */ true);
        return CL_SUCCESS;
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clFinish(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_0
{
    cl_event e = nullptr;
    cl_int status = clEnqueueMarker(command_queue, &e);
    if (status != CL_SUCCESS)
    {
        return status;
    }
    status = clWaitForEvents(1, &e);
    clReleaseEvent(e);
    return status;
}

static bool IsOutOfOrder(const cl_queue_properties* properties)
{
    auto prop = FindProperty<cl_queue_properties>(properties, CL_QUEUE_PROPERTIES);
    return prop != nullptr && ((*prop) & CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE) != 0;
}
static bool IsProfile(const cl_queue_properties* properties)
{
    auto prop = FindProperty<cl_queue_properties>(properties, CL_QUEUE_PROPERTIES);
    return prop != nullptr && ((*prop) & CL_QUEUE_PROFILING_ENABLE) != 0;
}
CommandQueue::CommandQueue(D3DDevice& device, Context& context, const cl_queue_properties* properties, bool synthesizedProperties)
    : CLChildBase(device.GetParent())
    , m_Context(context)
    , m_D3DDevice(device)
    , m_Properties(PropertiesToVector(properties))
    , m_bOutOfOrder(IsOutOfOrder(properties))
    , m_bProfile(IsProfile(properties))
    , m_bPropertiesSynthesized(synthesizedProperties)
{
}

void CommandQueue::Flush(TaskPoolLock const& lock, bool flushDevice)
{
    while (!m_QueuedTasks.empty())
    {
        m_OutstandingTasks.emplace_back(m_QueuedTasks.front().Get());
        m_QueuedTasks.pop_front();
        m_D3DDevice.SubmitTask(m_OutstandingTasks.back().Get(), lock);
    }
    if (flushDevice)
    {
        m_D3DDevice.Flush(lock);
    }
}

void CommandQueue::QueueTask(Task* p, TaskPoolLock const& lock)
{
    if (m_LastQueuedTask)
    {
        cl_event TaskAsEvent = m_LastQueuedTask;
        p->AddDependencies(&TaskAsEvent, 1, lock);
    }
    if (m_LastQueuedBarrier)
    {
        cl_event TaskAsEvent = m_LastQueuedBarrier;
        p->AddDependencies(&TaskAsEvent, 1, lock);
    }

    m_QueuedTasks.emplace_back(p);

    if (!m_bOutOfOrder)
    {
        m_LastQueuedTask = p;
    }
    if (p->m_CommandType == CL_COMMAND_BARRIER)
    {
        m_LastQueuedBarrier = p;
    }
}

void CommandQueue::NotifyTaskCompletion(Task* p, TaskPoolLock const&)
{
    {
        auto newEnd = std::remove_if(m_OutstandingTasks.begin(), m_OutstandingTasks.end(),
            [p](Task::ref_ptr_int const& e) { return e.Get() == p; });
        m_OutstandingTasks.erase(newEnd, m_OutstandingTasks.end());
    }
    {
        auto newEnd = std::remove_if(m_QueuedTasks.begin(), m_QueuedTasks.end(),
            [p](Task::ref_ptr const& e) { return e.Get() == p; });
        m_QueuedTasks.erase(newEnd, m_QueuedTasks.end());
    }
    if (m_LastQueuedTask == p)
    {
        m_LastQueuedTask = nullptr;
    }
    if (m_LastQueuedBarrier == p)
    {
        m_LastQueuedBarrier = nullptr;
    }
}

void CommandQueue::AddAllTasksAsDependencies(Task* p, TaskPoolLock const& lock)
{
    for (auto& task : m_OutstandingTasks)
    {
        if (task.Get() == m_LastQueuedTask || task.Get() == m_LastQueuedBarrier)
            continue;
        cl_event TaskAsEvent = task.Get();
        p->AddDependencies(&TaskAsEvent, 1, lock);
    }
    for (auto& task : m_QueuedTasks)
    {
        if (task.Get() == m_LastQueuedTask || task.Get() == m_LastQueuedBarrier)
            continue;
        cl_event TaskAsEvent = task.Get();
        p->AddDependencies(&TaskAsEvent, 1, lock);
    }
}
