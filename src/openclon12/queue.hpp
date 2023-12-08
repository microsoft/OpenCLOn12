// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "device.hpp"
#include "context.hpp"
#include "task.hpp"

class CommandQueue : public CLChildBase<CommandQueue, Device, cl_command_queue>
{
public:
    CommandQueue(D3DDevice& device, Context& context, const cl_queue_properties* properties, bool synthesizedProperties);

    friend cl_int CL_API_CALL clGetCommandQueueInfo(cl_command_queue, cl_command_queue_info, size_t, void*, size_t*);

    Context& GetContext() const { return m_Context.get(); }
    Device& GetDevice() const { return m_Parent.get(); }
    D3DDevice &GetD3DDevice() const { return m_D3DDevice; }

    void Flush(TaskPoolLock const&, bool flushDevice);
    void QueueTask(Task*, TaskPoolLock const&);
    void NotifyTaskCompletion(Task*, TaskPoolLock const&);
    void AddAllTasksAsDependencies(Task*, TaskPoolLock const&);

    const bool m_bOutOfOrder;
    const bool m_bProfile;
    const bool m_bPropertiesSynthesized;
    std::vector<cl_queue_properties> const m_Properties;

protected:
    Context::ref_int m_Context;
    D3DDevice &m_D3DDevice;

    std::deque<Task::ref_ptr> m_QueuedTasks;
    std::vector<Task::ref_ptr_int> m_OutstandingTasks;
    Task* m_LastQueuedTask = nullptr;
    Task* m_LastQueuedBarrier = nullptr;
};
