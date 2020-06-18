// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "kernel.hpp"
#include "task.hpp"
#include "queue.hpp"
#include "resources.hpp"
#include "sampler.hpp"
#include "clc_compiler.h"

extern void SignBlob(void* pBlob, size_t size);

auto Kernel::SpecializationKey::Allocate(clc_runtime_kernel_conf const& conf, ::clc_kernel_info const& info) -> std::unique_ptr<SpecializationKey>
{
    uint32_t NumAllocatedArgs = info.num_args ? (uint32_t)info.num_args - 1 : 0;
    std::unique_ptr<SpecializationKey> bits(reinterpret_cast<SpecializationKey*>(operator new(
        sizeof(SpecializationKey) + sizeof(PackedArgData) * NumAllocatedArgs)));
    new (bits.get()) SpecializationKey(conf, info);
    return bits;
}

Kernel::SpecializationKey::SpecializationKey(clc_runtime_kernel_conf const& conf, clc_kernel_info const& info)
{
    ConfigData.Bits.LocalSize[0] = conf.local_size[0];
    ConfigData.Bits.LocalSize[1] = conf.local_size[1];
    ConfigData.Bits.LocalSize[2] = conf.local_size[2];
    ConfigData.Bits.SupportGlobalOffsets = conf.support_global_work_id_offsets;
    ConfigData.Bits.SupportLocalOffsets = conf.support_global_work_id_offsets;
    ConfigData.Bits.LowerInt64 = 1;
    ConfigData.Bits.Padding = 0;

    NumArgs = (uint32_t)info.num_args;
    for (uint32_t i = 0; i < NumArgs; ++i)
    {
        if (info.args[i].address_qualifier == CLC_KERNEL_ARG_ADDRESS_LOCAL)
        {
            Args[i].LocalArgSize = conf.args[i].localptr.size;
        }
        else if (strcmp(info.args[i].type_name, "sampler_t") == 0)
        {
            Args[i].SamplerArgData.AddressingMode = conf.args[i].sampler.addressing_mode;
            Args[i].SamplerArgData.LinearFiltering = conf.args[i].sampler.linear_filtering;
            Args[i].SamplerArgData.NormalizedCoords = conf.args[i].sampler.normalized_coords;
            Args[i].SamplerArgData.Padding = 0;
        }
        else
        {
            Args[i].LocalArgSize = 0;
        }
    }
}

size_t Kernel::SpecializationKeyHash::operator()(std::unique_ptr<Kernel::SpecializationKey> const& ptr) const
{
    size_t val = std::hash<uint64_t>()(ptr->ConfigData.Value);
    for (uint32_t i = 0; i < ptr->NumArgs; ++i)
    {
        D3D12TranslationLayer::hash_combine(val, ptr->Args[i].LocalArgSize);
    }
    return val;
}

bool Kernel::SpecializationKeyEqual::operator()(std::unique_ptr<Kernel::SpecializationKey> const& a, std::unique_ptr<Kernel::SpecializationKey> const& b) const
{
    assert(a->NumArgs == b->NumArgs);
    uint32_t NumAllocatedArgs = a->NumArgs ? a->NumArgs - 1 : 0;
    size_t size = sizeof(Kernel::SpecializationKey) + sizeof(Kernel::SpecializationKey::PackedArgData) * NumAllocatedArgs;
    return memcmp(a.get(), b.get(), size) == 0;
}

class ExecuteKernel : public Task
{
public:
    Kernel::ref_ptr_int m_Kernel;
    const std::array<uint32_t, 3> m_DispatchDims;

    std::vector<D3D12TranslationLayer::UAV*> m_UAVs;
    std::vector<D3D12TranslationLayer::SRV*> m_SRVs;
    std::vector<D3D12TranslationLayer::Sampler*> m_Samplers;
    std::vector<D3D12TranslationLayer::Resource*> m_CBs;
    std::vector<cl_uint> m_CBOffsets;
    Resource::UnderlyingResourcePtr m_KernelArgsCb;

    std::vector<Resource::ref_ptr_int> m_KernelArgUAVs;
    std::vector<Resource::ref_ptr_int> m_KernelArgSRVs;
    std::vector<Sampler::ref_ptr_int> m_KernelArgSamplers;

    std::mutex m_SpecializeLock;
    std::condition_variable m_SpecializeEvent;
    
    Kernel::SpecializationValue *m_Specialized = nullptr;
    bool m_SpecializeError = false;

    void MigrateResources() final
    {
        for (auto& res : m_KernelArgUAVs)
        {
            if (res.Get())
                res->EnqueueMigrateResource(&m_CommandQueue->GetDevice(), this, 0);
        }
        for (auto& res : m_KernelArgSRVs)
        {
            if (res.Get())
                res->EnqueueMigrateResource(&m_CommandQueue->GetDevice(), this, 0);
        }
    }
    void RecordImpl() final;
    void OnComplete() final
    {
        m_Kernel.Release();
    }

    ExecuteKernel(Kernel& kernel, cl_command_queue queue, std::array<uint32_t, 3> const& dims, std::array<uint32_t, 3> const& offset, std::array<uint16_t, 3> const& localSize, cl_uint workDims)
        : Task(kernel.m_Parent->GetContext(), CL_COMMAND_NDRANGE_KERNEL, queue)
        , m_Kernel(&kernel)
        , m_DispatchDims(dims)
        , m_UAVs(kernel.m_UAVs.size(), nullptr)
        , m_SRVs(kernel.m_SRVs.size(), nullptr)
        , m_Samplers(kernel.m_Samplers.size(), nullptr)
        , m_KernelArgUAVs(kernel.m_UAVs.begin(), kernel.m_UAVs.end())
        , m_KernelArgSRVs(kernel.m_SRVs.begin(), kernel.m_SRVs.end())
        , m_KernelArgSamplers(kernel.m_Samplers.begin(), kernel.m_Samplers.end())
    {
        cl_uint KernelArgCBIndex = kernel.m_pDxil->metadata.kernel_inputs_cbv_id;
        cl_uint WorkPropertiesCBIndex = kernel.m_pDxil->metadata.work_properties_cbv_id;
        unsigned num_cbs = max(KernelArgCBIndex + 1,
                               WorkPropertiesCBIndex + 1);
        m_CBs.resize(num_cbs);
        m_CBOffsets.resize(num_cbs);

        clc_work_properties_data work_properties = {};
        work_properties.global_offset_x = offset[0];
        work_properties.global_offset_y = offset[1];
        work_properties.global_offset_z = offset[2];
        work_properties.work_dim = workDims;
        work_properties.group_count_total_x = dims[0];
        work_properties.group_count_total_y = dims[1];
        work_properties.group_count_total_z = dims[2];

        cl_uint numXIterations = ((dims[0] - 1) / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) + 1;
        cl_uint numYIterations = ((dims[1] - 1) / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) + 1;
        cl_uint numZIterations = ((dims[2] - 1) / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) + 1;
        cl_uint numIterations = numXIterations * numYIterations * numZIterations;

        size_t KernelInputsCbSize = kernel.m_pDxil->metadata.kernel_inputs_buf_size;
        size_t WorkPropertiesOffset = D3D12TranslationLayer::Align<size_t>(KernelInputsCbSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        m_CBOffsets[WorkPropertiesCBIndex] = (UINT)WorkPropertiesOffset / 16;
        static_assert(sizeof(work_properties) < D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        KernelInputsCbSize = WorkPropertiesOffset + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT * numIterations;

        kernel.m_KernelArgsCbData.resize(KernelInputsCbSize);
        byte* workPropertiesData = kernel.m_KernelArgsCbData.data() + WorkPropertiesOffset;
        for (cl_uint x = 0; x < numXIterations; ++x)
        {
            for (cl_uint y = 0; y < numYIterations; ++y)
            {
                for (cl_uint z = 0; z < numZIterations; ++z)
                {
                    work_properties.group_id_offset_x = x * D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                    work_properties.group_id_offset_y = y * D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                    work_properties.group_id_offset_z = z * D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                    memcpy(workPropertiesData, &work_properties, sizeof(work_properties));
                    workPropertiesData += D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
                }
            }
        }

        auto& Device = m_CommandQueue->GetDevice();

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
            Device.GetDevice()->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_UPLOAD));
        assert(Args.m_appDesc.m_Width % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);

        m_KernelArgsCb =
            D3D12TranslationLayer::Resource::CreateResource(
                &Device.ImmCtx(),
                Args,
                D3D12TranslationLayer::ResourceAllocationContext::FreeThread);

        D3D11_SUBRESOURCE_DATA Data = { kernel.m_KernelArgsCbData.data() };
        Device.ImmCtx().UpdateSubresources(
            m_KernelArgsCb.get(),
            m_KernelArgsCb->GetFullSubresourceSubset(),
            &Data,
            nullptr,
            D3D12TranslationLayer::ImmediateContext::UpdateSubresourcesScenario::InitialData);

        m_CBs[KernelArgCBIndex] = m_KernelArgsCb.get();
        m_CBs[WorkPropertiesCBIndex] = m_KernelArgsCb.get();

        clc_runtime_kernel_conf config = {};
        config.lower_int64 = true;
        config.support_global_work_id_offsets = std::any_of(std::begin(offset), std::end(offset), [](cl_uint v) { return v != 0; });
        config.support_work_group_id_offsets = numIterations != 1;
        std::copy(std::begin(localSize), std::end(localSize), config.local_size);
        config.args = kernel.m_ArgMetadataToCompiler.data();
        auto SpecKey = Kernel::SpecializationKey::Allocate(config, *kernel.m_pDxil->kernel);
        
        {
            std::lock_guard SpecLock(kernel.m_SpecializationCacheLock);
            auto iter = kernel.m_SpecializationCache.find(SpecKey);
            if (iter != kernel.m_SpecializationCache.end())
                m_Specialized = &iter->second;
        }

        if (!m_Specialized)
        {
            g_Platform->QueueProgramOp([this, localSize, offset, numIterations, &Device, config,
                                                 ArgInfo = kernel.m_ArgMetadataToCompiler,
                                                 SpecKey = std::move(SpecKey),
                                                 kernel = this->m_Kernel,
                                                 refThis = Task::ref_int(*this)]() mutable
            {
                try
                {
                    auto& Compiler = g_Platform->GetCompiler();
                    auto Context = g_Platform->GetCompilerContext();
                    auto get_kernel = Compiler.proc_address<decltype(&clc_to_dxil)>("clc_to_dxil");
                    auto free = Compiler.proc_address<decltype(&clc_free_dxil_object)>("clc_free_dxil_object");

                    config.args = ArgInfo.data();

                    auto spirv = kernel->m_Parent->GetSpirV(&m_CommandQueue->GetDevice());
                    auto name = kernel->m_pDxil->kernel->name;
                    unique_dxil specialized(get_kernel(Context, spirv, name, &config, nullptr), free);

                    SignBlob(specialized->binary.data, specialized->binary.size);

                    auto CS = std::make_unique<D3D12TranslationLayer::Shader>(&Device.ImmCtx(), specialized->binary.data, specialized->binary.size, kernel->m_ShaderDecls);
                    D3D12TranslationLayer::COMPUTE_PIPELINE_STATE_DESC Desc = { CS.get() };
                    auto PSO = Device.CreatePSO(Desc);

                    Kernel::SpecializationValue *cacheEntry = nullptr;

                    {
                        std::lock_guard lock(kernel->m_SpecializationCacheLock);
                        auto ret = kernel->m_SpecializationCache.try_emplace(std::move(SpecKey),
                                                                             std::move(specialized),
                                                                             std::move(CS),
                                                                             std::move(PSO));
                        cacheEntry = &ret.first->second;
                    }

                    {
                        std::lock_guard lock(m_SpecializeLock);
                        m_Specialized = cacheEntry;
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
    const std::array<uint16_t, 3> AutoDims[3] =
    {
        { 64, 1, 1 },
        { 8, 8, 1 },
        { 4, 4, 4 }
    };
    const std::array<uint16_t, 3> MaxDims =
    {
        D3D12_CS_THREAD_GROUP_MAX_X,
        D3D12_CS_THREAD_GROUP_MAX_Y,
        D3D12_CS_THREAD_GROUP_MAX_Z
    };
    for (cl_uint i = 0; i < work_dim; ++i)
    {
        uint16_t& LocalSize = LocalSizes[i];
        if (local_work_size && local_work_size[i] > std::numeric_limits<uint16_t>::max())
        {
            return ReportError("local_work_size is too large.", CL_INVALID_WORK_GROUP_SIZE);
        }

        LocalSize = local_work_size ? (uint16_t)local_work_size[i] :
            (DimsHint ? DimsHint[i] : AutoDims[work_dim][i]);
        if (RequiredDims)
        {
            if (RequiredDims[i] != LocalSize)
            {
                return ReportError("local_work_size does not match required size declared by kernel.", CL_INVALID_WORK_GROUP_SIZE);
            }
            if (global_work_size[i] % LocalSize != 0)
            {
                return ReportError("local_work_size must evenly divide the global_work_size.", CL_INVALID_WORK_GROUP_SIZE);
            }
            if (LocalSize > MaxDims[i])
            {
                return ReportError("local_work_size exceeds max in one dimension.", CL_INVALID_WORK_ITEM_SIZE);
            }
        }
        else
        {
            while (global_work_size[i] % LocalSize != 0 ||
                   LocalSize > MaxDims[i])
            {
                // TODO: Better backoff algorithm
                LocalSize /= 2;
            }
        }
    }
    if (RequiredDims)
    {
        if ((uint64_t)LocalSizes[0] * (uint64_t)LocalSizes[1] * (uint64_t)LocalSizes[2] > D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP)
        {
            return ReportError("local_work_size exceeds max work items per group.", CL_INVALID_WORK_GROUP_SIZE);
        }
    }
    else
    {
        cl_uint dimension = work_dim - 1;
        while ((uint64_t)LocalSizes[0] * (uint64_t)LocalSizes[1] * (uint64_t)LocalSizes[2] > D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP)
        {
            // Find a dimension to shorten
            // TODO: Better backoff algorithm
            if (LocalSizes[dimension] > 1)
            {
                LocalSizes[dimension] /= 2;
            }
            dimension = (dimension == 0) ? work_dim - 1 : dimension - 1;
        }
    }

    for (cl_uint i = 0; i < work_dim; ++i)
    {
        DispatchDimensions[i] = (uint32_t)(global_work_size[i] / LocalSizes[i]);
        if (!RequiredDims)
        {
            // Try to expand local size to avoid having to loop Dispatches
            while (DispatchDimensions[i] > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
            {
                auto OldLocalSize = LocalSizes[i];
                LocalSizes[i] *= 2;
                if ((uint64_t)LocalSizes[0] * (uint64_t)LocalSizes[1] * (uint64_t)LocalSizes[2] > D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP ||
                    LocalSizes[i] > MaxDims[i] ||
                    global_work_size[i] % LocalSizes[i] != 0)
                {
                    LocalSizes[i] = OldLocalSize;
                    break;
                }
                DispatchDimensions[i] /= 2;
            }
        }
    }

    try
    {
        std::unique_ptr<Task> task(new ExecuteKernel(kernel, command_queue, DispatchDimensions, GlobalWorkItemOffsets, LocalSizes, work_dim));

        auto Lock = g_Platform->GetTaskPoolLock();
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
    while (!m_Specialized && !m_SpecializeError)
    {
        m_SpecializeEvent.wait(lock);
    }

    if (m_SpecializeError)
    {
        auto Lock = g_Platform->GetTaskPoolLock();
        Complete(CL_BUILD_PROGRAM_FAILURE, Lock);
        throw std::exception("Failed to specialize");
    }

    auto& Device = m_CommandQueue->GetDevice();
    std::transform(m_KernelArgUAVs.begin(), m_KernelArgUAVs.end(), m_UAVs.begin(), [&Device](Resource::ref_ptr_int& resource) { return resource.Get() ? &resource->GetUAV(&Device) : nullptr; });
    std::transform(m_KernelArgSRVs.begin(), m_KernelArgSRVs.end(), m_SRVs.begin(), [&Device](Resource::ref_ptr_int& resource) { return resource.Get() ? &resource->GetSRV(&Device) : nullptr; });
    std::transform(m_KernelArgSamplers.begin(), m_KernelArgSamplers.end(), m_Samplers.begin(), [&Device](Sampler::ref_ptr_int& sampler) { return sampler.Get() ? &sampler->GetUnderlying(&Device) : nullptr; });

    auto& ImmCtx = Device.ImmCtx();
    ImmCtx.CsSetUnorderedAccessViews(0, (UINT)m_UAVs.size(), m_UAVs.data(), c_aUAVAppendOffsets);
    ImmCtx.SetShaderResources<D3D12TranslationLayer::e_CS>(0, (UINT)m_SRVs.size(), m_SRVs.data());
    ImmCtx.SetSamplers<D3D12TranslationLayer::e_CS>(0, (UINT)m_Samplers.size(), m_Samplers.data());
    ImmCtx.SetPipelineState(m_Specialized->m_PSO.get());

    cl_uint numXIterations = ((m_DispatchDims[0] - 1) / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) + 1;
    cl_uint numYIterations = ((m_DispatchDims[1] - 1) / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) + 1;
    cl_uint numZIterations = ((m_DispatchDims[2] - 1) / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) + 1;
    for (cl_uint x = 0; x < numXIterations; ++x)
    {
        for (cl_uint y = 0; y < numYIterations; ++y)
        {
            for (cl_uint z = 0; z < numZIterations; ++z)
            {
                UINT DimsX = (x == numXIterations - 1) ? (m_DispatchDims[0] - D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * (numXIterations - 1)) : D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                UINT DimsY = (y == numYIterations - 1) ? (m_DispatchDims[1] - D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * (numYIterations - 1)) : D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                UINT DimsZ = (z == numZIterations - 1) ? (m_DispatchDims[2] - D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * (numZIterations - 1)) : D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;

                ImmCtx.SetConstantBuffers<D3D12TranslationLayer::e_CS>(0, (UINT)m_CBs.size(), m_CBs.data(), m_CBOffsets.data(), c_NumConstants);
                ImmCtx.Dispatch(DimsX, DimsY, DimsZ);

                m_CBOffsets[m_Kernel->m_pDxil->metadata.work_properties_cbv_id] += D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT / 16;
            }
        }
    }

    ImmCtx.ClearState();
}
