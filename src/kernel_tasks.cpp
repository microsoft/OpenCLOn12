#include "kernel.hpp"
#include "task.hpp"
#include "queue.hpp"
#include "resources.hpp"

class ExecuteKernel : public Task
{
public:
    Kernel::ref_ptr_int m_Kernel;
    const std::array<uint32_t, 3> m_DispatchDims;
    D3D12TranslationLayer::UAV* const m_UAV;
    void RecordImpl() final;
    void OnComplete() final
    {
        m_Kernel.Release();
    }

    ExecuteKernel(Kernel& kernel, cl_command_queue queue, std::array<uint32_t, 3> const& dims)
        : Task(kernel.m_Parent->GetContext(), CL_COMMAND_NDRANGE_KERNEL, queue)
        , m_Kernel(&kernel)
        , m_DispatchDims(dims)
        , m_UAV(kernel.m_ResourceArgument ? &kernel.m_ResourceArgument->GetUAV() : nullptr)
    {
    }
};

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueNDRangeKernel(cl_command_queue command_queue,
    cl_kernel        kernel_,
    cl_uint          work_dim,
    const size_t* global_work_offset,
    const size_t* global_work_size,
    const size_t* local_work_size,
    cl_uint          num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!kernel_)
    {
        return CL_INVALID_KERNEL;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    Kernel& kernel = *static_cast<Kernel*>(kernel_);

    if (&kernel.m_Parent->GetContext() != &context)
    {
        return ReportError("Kernel was not created on the same context as the command queue.", CL_INVALID_CONTEXT);
    }

    if ((event_wait_list == nullptr) != (num_events_in_wait_list == 0))
    {
        return ReportError("If event_wait_list is null, then num_events_in_wait_list mut be zero, and vice versa.", CL_INVALID_EVENT_WAIT_LIST);
    }

    if (work_dim == 0 || work_dim > 3)
    {
        return ReportError("work_dim must be between 1 and 3.", CL_INVALID_WORK_DIMENSION);
    }
    if (global_work_offset != nullptr &&
        std::any_of(global_work_offset, global_work_offset + work_dim, [](size_t o) { return o != 0; }))
    {
        return ReportError("global_work_offset must all be 0 on this platform.", CL_INVALID_GLOBAL_OFFSET);
    }
    if (local_work_size != nullptr &&
        std::any_of(local_work_size, local_work_size + work_dim, [](size_t s) { return s != 1; }))
    {
        return ReportError("local_work_size must all be 1 on this platform.", CL_INVALID_WORK_GROUP_SIZE);
    }

    std::array<uint32_t, 3> DispatchDimensions = { 1, 1, 1 };
    std::transform(global_work_size, global_work_size + work_dim,
        DispatchDimensions.begin(), [](size_t s) { return (uint32_t)s; });

    try
    {
        std::unique_ptr<Task> task(new ExecuteKernel(kernel, command_queue, DispatchDimensions));

        auto Lock = context.GetDevice().GetTaskPoolLock();
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
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }

    return CL_SUCCESS;
}

extern CL_API_ENTRY CL_EXT_PREFIX__VERSION_1_2_DEPRECATED cl_int CL_API_CALL
clEnqueueTask(cl_command_queue  command_queue,
    cl_kernel         kernel,
    cl_uint           num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_EXT_SUFFIX__VERSION_1_2_DEPRECATED
{
    size_t global_work_size = 1, local_work_size = 1;
    return clEnqueueNDRangeKernel(
        command_queue,
        kernel,
        1,
        nullptr,
        &global_work_size,
        &local_work_size,
        num_events_in_wait_list,
        event_wait_list,
        event);
}

void ExecuteKernel::RecordImpl()
{
    auto& ImmCtx = m_Parent->GetDevice().ImmCtx();
    const UINT InitialCount = UINT_MAX;
    ImmCtx.CsSetUnorderedAccessViews(0, 1, &m_UAV, &InitialCount);
    ImmCtx.SetPipelineState(&m_Kernel->m_PSO);
    ImmCtx.Dispatch(m_DispatchDims[0], m_DispatchDims[1], m_DispatchDims[2]);
}
