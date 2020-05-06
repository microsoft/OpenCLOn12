// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "kernel.hpp"
#include "task.hpp"
#include "queue.hpp"
#include "resources.hpp"
#include "clc_compiler.h"

extern void SignBlob(void* pBlob, size_t size);

class ExecuteKernel : public Task
{
public:
    Kernel::ref_ptr_int m_Kernel;
    const std::array<uint32_t, 3> m_DispatchDims;

    const std::vector<D3D12TranslationLayer::UAV*> m_UAVs;
    const std::vector<D3D12TranslationLayer::SRV*> m_SRVs;
    const std::vector<D3D12TranslationLayer::Sampler*> m_Samplers;
    std::vector<clc_runtime_arg_info> m_ArgInfo;
    std::vector<D3D12TranslationLayer::Resource*> m_CBs;
    const std::vector<cl_uint> m_CBOffsets;
    Resource::UnderlyingResourcePtr m_KernelArgsCb;

    std::mutex m_SpecializeLock;
    std::condition_variable m_SpecializeEvent;
    // TODO: Cache these
    unique_dxil m_Specialized = {nullptr, nullptr};
    std::unique_ptr<D3D12TranslationLayer::Shader> m_Shader;
    std::unique_ptr<D3D12TranslationLayer::PipelineState> m_PSO;
    bool m_SpecializeError = false;

    void RecordImpl() final;
    void OnComplete() final
    {
        m_Kernel.Release();
    }

    ExecuteKernel(Kernel& kernel, cl_command_queue queue, std::array<uint32_t, 3> const& dims, std::array<uint32_t, 3> const& offset, std::array<uint16_t, 3> const& localSize)
        : Task(kernel.m_Parent->GetContext(), CL_COMMAND_NDRANGE_KERNEL, queue)
        , m_Kernel(&kernel)
        , m_DispatchDims(dims)
        , m_UAVs(kernel.m_UAVs)
        , m_SRVs(kernel.m_SRVs)
        , m_Samplers(kernel.m_Samplers)
        , m_ArgInfo(kernel.m_ArgMetadataToCompiler)
        , m_CBs(kernel.m_CBs)
        , m_CBOffsets(kernel.m_CBOffsets)
    {
        cl_uint KernelArgCBIndex = kernel.m_pDxil->metadata.kernel_inputs_cbv_id;
        cl_uint GlobalOffsetsCBIndex = kernel.m_pDxil->metadata.global_work_offset_cbv_id;
        assert(m_CBs[KernelArgCBIndex] == nullptr && m_CBs[GlobalOffsetsCBIndex] == nullptr);
        assert(m_CBOffsets[KernelArgCBIndex] == 0 && m_CBOffsets[GlobalOffsetsCBIndex] != 0);
        assert(m_CBs.size() == m_CBOffsets.size());

        memcpy(kernel.m_KernelArgsCbData.data() + m_CBOffsets[GlobalOffsetsCBIndex] * 16, offset.data(), sizeof(offset));

        D3D12TranslationLayer::ResourceCreationArgs Args = {};
        Args.m_appDesc.m_Subresources = 1;
        Args.m_appDesc.m_SubresourcesPerPlane = 1;
        Args.m_appDesc.m_NonOpaquePlaneCount = 1;
        Args.m_appDesc.m_MipLevels = 1;
        Args.m_appDesc.m_ArraySize = 1;
        Args.m_appDesc.m_Depth = 1;
        Args.m_appDesc.m_Width = (UINT)kernel.m_KernelArgsCbData.size();
        Args.m_appDesc.m_Height = 1;
        Args.m_appDesc.m_Format = DXGI_FORMAT_UNKNOWN;
        Args.m_appDesc.m_Samples = 1;
        Args.m_appDesc.m_Quality = 0;
        Args.m_appDesc.m_resourceDimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        Args.m_appDesc.m_usage = D3D12TranslationLayer::RESOURCE_USAGE_DYNAMIC;
        Args.m_appDesc.m_bindFlags = D3D12TranslationLayer::RESOURCE_BIND_CONSTANT_BUFFER;
        Args.m_desc12 = CD3DX12_RESOURCE_DESC::Buffer(Args.m_appDesc.m_Width);
        Args.m_heapDesc = CD3DX12_HEAP_DESC(Args.m_appDesc.m_Width,
            m_Parent->GetDevice().GetDevice()->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_UPLOAD));
        assert(Args.m_appDesc.m_Width % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);

        m_KernelArgsCb =
            D3D12TranslationLayer::Resource::CreateResource(
                &m_Parent->GetDevice().ImmCtx(),
                Args,
                D3D12TranslationLayer::ResourceAllocationContext::FreeThread);

        D3D11_SUBRESOURCE_DATA Data = { kernel.m_KernelArgsCbData.data() };
        m_Parent->GetDevice().ImmCtx().UpdateSubresources(
            m_KernelArgsCb.get(),
            m_KernelArgsCb->GetFullSubresourceSubset(),
            &Data,
            nullptr,
            D3D12TranslationLayer::ImmediateContext::UpdateSubresourcesScenario::InitialData);

        m_CBs[KernelArgCBIndex] = m_KernelArgsCb.get();
        m_CBs[GlobalOffsetsCBIndex] = m_KernelArgsCb.get();

        m_Parent->GetDevice().QueueProgramOp([this, localSize]()
        {
            try
            {
                auto& Compiler = g_Platform->GetCompiler();
                auto Context = g_Platform->GetCompilerContext();
                auto get_kernel = Compiler.proc_address<decltype(&clc_to_dxil)>("clc_to_dxil");
                auto free = Compiler.proc_address<decltype(&clc_free_dxil_object)>("clc_free_dxil_object");

                clc_runtime_kernel_conf config = {};
                std::copy(std::begin(localSize), std::end(localSize), config.local_size);
                config.args = m_ArgInfo.data();

                auto spirv = m_Kernel->m_Parent->GetSpirV();
                auto name = m_Kernel->m_pDxil->kernel->name;
                unique_dxil specialized(get_kernel(Context, spirv, name, &config, nullptr), free);

                SignBlob(specialized->binary.data, specialized->binary.size);

                auto CS = std::make_unique<D3D12TranslationLayer::Shader>(&m_Parent->GetDevice().ImmCtx(), specialized->binary.data, specialized->binary.size, m_Kernel->m_ShaderDecls);
                D3D12TranslationLayer::COMPUTE_PIPELINE_STATE_DESC Desc = { CS.get() };
                auto PSO = std::make_unique<D3D12TranslationLayer::PipelineState>(&m_Parent->GetDevice().ImmCtx(), Desc);

                {
                    std::lock_guard lock(m_SpecializeLock);
                    std::swap(m_Specialized, specialized);
                    std::swap(m_Shader, CS);
                    std::swap(m_PSO, PSO);
                }
                m_SpecializeEvent.notify_all();
            }
            catch (...)
            {
                {
                    std::lock_guard lock(m_SpecializeLock);
                    m_SpecializeError = true;
                }
                m_SpecializeEvent.notify_all();
            }
        });
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

    if (!global_work_size)
    {
        return ReportError("global_work_size must be specified.", CL_INVALID_GLOBAL_WORK_SIZE);
    }

    std::array<uint32_t, 3> GlobalWorkItemOffsets = {};
    if (global_work_offset != nullptr)
    {
        for (cl_uint i = 0; i < work_dim; ++i)
        {
            if (global_work_offset[i] + global_work_size[i] > std::numeric_limits<uint32_t>::max())
            {
                return ReportError("global_work_offset + global_work_size would exceed maximum value.", CL_INVALID_GLOBAL_OFFSET);
            }
            GlobalWorkItemOffsets[i] = (uint32_t)global_work_offset[i];
        }
    }

    std::array<uint32_t, 3> DispatchDimensions = { 1, 1, 1 };
    std::array<uint16_t, 3> LocalSizes = { 1, 1, 1 };
    auto RequiredDims = kernel.GetRequiredLocalDims();
    auto DimsHint = kernel.GetLocalDimsHint();
    for (cl_uint i = 0; i < work_dim; ++i)
    {
        uint16_t& LocalSize = LocalSizes[i];
        if (local_work_size && local_work_size[i] > std::numeric_limits<uint16_t>::max())
        {
            return ReportError("local_work_size is too large.", CL_INVALID_WORK_GROUP_SIZE);
        }

        LocalSize = local_work_size ? (uint16_t)local_work_size[i] :
            (DimsHint ? DimsHint[i] : 1u);
        if (global_work_size[i] % LocalSize != 0)
        {
            return ReportError("local_work_size must evenly divide the global_work_size.", CL_INVALID_WORK_GROUP_SIZE);
        }
        DispatchDimensions[i] = (uint32_t)(global_work_size[i] / LocalSize);
        if (DispatchDimensions[i] > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
        {
            return ReportError("global_work_size / local_work_size is too large.", CL_INVALID_GLOBAL_WORK_SIZE);
        }

        if (RequiredDims && RequiredDims[i] != LocalSize)
        {
            return ReportError("local_work_size does not match required size declared by kernel.", CL_INVALID_WORK_GROUP_SIZE);
        }
    }
    if ((uint64_t)LocalSizes[0] * (uint64_t)LocalSizes[1] * (uint64_t)LocalSizes[2] > D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP)
    {
        return ReportError("local_work_size exceeds max work items per group.", CL_INVALID_WORK_GROUP_SIZE);
    }
    if (LocalSizes[0] > D3D12_CS_THREAD_GROUP_MAX_X ||
        LocalSizes[1] > D3D12_CS_THREAD_GROUP_MAX_Y ||
        LocalSizes[2] > D3D12_CS_THREAD_GROUP_MAX_Z)
    {
        return ReportError("local_work_size exceeds max in one dimension.", CL_INVALID_WORK_ITEM_SIZE);
    }

    try
    {
        std::unique_ptr<Task> task(new ExecuteKernel(kernel, command_queue, DispatchDimensions, GlobalWorkItemOffsets, LocalSizes));

        auto Lock = context.GetDevice().GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
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

constexpr UINT c_aUAVAppendOffsets[D3D11_1_UAV_SLOT_COUNT] =
{
    (UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,
    (UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,
    (UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,
    (UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,
    (UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,
    (UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,
    (UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,
    (UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,(UINT)-1,
};
constexpr UINT c_NumConstants[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] =
{
    D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT,
    D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT,
    D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT,
    D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT,
    D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT
};

void ExecuteKernel::RecordImpl()
{
    std::unique_lock lock(m_SpecializeLock);
    while (!m_PSO.get() && !m_SpecializeError)
    {
        m_SpecializeEvent.wait(lock);
    }

    if (m_SpecializeError)
    {
        auto Lock = m_Parent->GetDevice().GetTaskPoolLock();
        Complete(CL_BUILD_PROGRAM_FAILURE, Lock);
        throw std::exception("Failed to specialize");
    }

    auto& ImmCtx = m_Parent->GetDevice().ImmCtx();
    ImmCtx.CsSetUnorderedAccessViews(0, (UINT)m_UAVs.size(), m_UAVs.data(), c_aUAVAppendOffsets);
    ImmCtx.SetConstantBuffers<D3D12TranslationLayer::e_CS>(0, (UINT)m_CBs.size(), m_CBs.data(), m_CBOffsets.data(), c_NumConstants);
    ImmCtx.SetShaderResources<D3D12TranslationLayer::e_CS>(0, (UINT)m_SRVs.size(), m_SRVs.data());
    ImmCtx.SetSamplers<D3D12TranslationLayer::e_CS>(0, (UINT)m_Samplers.size(), m_Samplers.data());
    ImmCtx.SetPipelineState(m_PSO.get());
    ImmCtx.Dispatch(m_DispatchDims[0], m_DispatchDims[1], m_DispatchDims[2]);

    ImmCtx.ClearState();
}
