// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "kernel.hpp"
#include "task.hpp"
#include "queue.hpp"
#include "resources.hpp"
#include "sampler.hpp"
#include "program.hpp"
#include "compiler.hpp"

#include <wil/resource.h>
#include <sstream>
#include <numeric>

#include "ImmediateContext.inl"

#include "spookyv2.h"

extern void SignBlob(void* pBlob, size_t size);
constexpr uint32_t PrintfBufferSize = 1024 * 1024;
constexpr uint32_t PrintfBufferInitialData[PrintfBufferSize / sizeof(uint32_t)] = { sizeof(uint32_t) * 2, PrintfBufferSize };

size_t Program::SpecializationKey::AllocatedByteSize(uint32_t NumArgs)
{
    uint32_t NumAllocatedArgs = NumArgs ? NumArgs - 1 : 0;
    return sizeof(SpecializationKey) + sizeof(PackedArgData) * NumAllocatedArgs;
}
size_t Program::SpecializationKey::HashByteSize(uint32_t NumArgs)
{
    return offsetof(SpecializationKey, Args) + sizeof(PackedArgData) * NumArgs;
}

auto Program::SpecializationKey::Allocate(D3DDevice const* Device, CompiledDxil::Configuration const& conf) -> std::unique_ptr<const SpecializationKey>
{
    std::unique_ptr<SpecializationKey> bits(reinterpret_cast<SpecializationKey*>(operator new(AllocatedByteSize((uint32_t)conf.args.size()))));
    new (bits.get()) SpecializationKey(Device, conf);
    return bits;
}

Program::SpecializationKey::SpecializationKey(D3DDevice const* Device, CompiledDxil::Configuration const& conf)
{
    this->Device = Device;
    ConfigData.Bits.LocalSize[0] = conf.local_size[0];
    ConfigData.Bits.LocalSize[1] = conf.local_size[1];
    ConfigData.Bits.LocalSize[2] = conf.local_size[2];
    ConfigData.Bits.SupportGlobalOffsets = conf.support_global_work_id_offsets;
    ConfigData.Bits.SupportLocalOffsets = conf.support_work_group_id_offsets;
    ConfigData.Bits.LowerInt64 = conf.lower_int64;
    ConfigData.Bits.LowerInt16 = conf.lower_int64;
    ConfigData.Bits.Padding = 0;

    NumArgs = (uint32_t)conf.args.size();
    for (uint32_t i = 0; i < NumArgs; ++i)
    {
        memset(&Args[i], 0, sizeof(Args[i]));
        if (auto localConfig = std::get_if<CompiledDxil::Configuration::Arg::Local>(&conf.args[i].config); localConfig)
        {
            Args[i].LocalArgSize = localConfig->size;
        }
        else if (auto samplerConfig = std::get_if<CompiledDxil::Configuration::Arg::Sampler>(&conf.args[i].config); samplerConfig)
        {
            Args[i].SamplerArgData.AddressingMode = samplerConfig->addressingMode;
            Args[i].SamplerArgData.LinearFiltering = samplerConfig->linearFiltering;
            Args[i].SamplerArgData.NormalizedCoords = samplerConfig->normalizedCoords;
            Args[i].SamplerArgData.Padding = 0;
        }
        else
        {
            Args[i].LocalArgSize = 0;
        }
    }
}

size_t Program::SpecializationKeyHash::operator()(std::unique_ptr<const SpecializationKey> const& ptr) const
{
    size_t val = std::hash<uint64_t>()(ptr->ConfigData.Value);
    D3D12TranslationLayer::hash_combine(val, std::hash<const void *>()(ptr->Device));
    for (uint32_t i = 0; i < ptr->NumArgs; ++i)
    {
        D3D12TranslationLayer::hash_combine(val, ptr->Args[i].LocalArgSize);
    }
    return val;
}

bool Program::SpecializationKeyEqual::operator()(std::unique_ptr<const SpecializationKey> const& a,
                                                 std::unique_ptr<const SpecializationKey> const& b) const
{
    assert(a->NumArgs == b->NumArgs);
    size_t size = SpecializationKey::HashByteSize(a->NumArgs);
    return memcmp(a.get(), b.get(), size) == 0;
}

auto Program::GetSpecializationData(
    Device* device, std::string const& kernelName, std::unique_ptr<const SpecializationKey> key) ->
    SpecializationData
{
    std::lock_guard programLock(m_Lock);
    auto buildDataIter = m_BuildData.find(device);
    assert(buildDataIter != m_BuildData.end());
    auto& buildData = buildDataIter->second;
    auto kernelsIter = buildData->m_Kernels.find(kernelName);
    assert(kernelsIter != buildData->m_Kernels.end());
    auto& kernel = kernelsIter->second;

    std::lock_guard specializationCacheLock(buildData->m_SpecializationCacheLock);
    auto [iter, success] = kernel.m_SpecializationCache.try_emplace(std::move(key));
    return { iter->first.get(), &iter->second, success, { buildData->m_Hash[0], buildData->m_Hash[1] }};
}

class ExecuteKernel : public Task
{
public:
    Kernel::ref_ptr_int m_Kernel;
    const std::array<uint32_t, 3> m_DispatchDims;

    Resource::UnderlyingResourcePtr m_KernelArgsCb;
    std::vector<std::byte> m_KernelArgsCbData;
    size_t m_WorkPropertiesOffset;
    Resource::ref_ptr m_PrintfUAV;

    std::vector<Resource::ref_ptr_int> m_KernelArgUAVs;
    std::vector<Resource::ref_ptr_int> m_KernelArgSRVs;
    std::vector<Sampler::ref_ptr_int> m_KernelArgSamplers;
    
    Program::SpecializationValue *m_Specialized = nullptr;

    void MigrateResources() final
    {
        for (auto& res : m_KernelArgUAVs)
        {
            if (res.Get())
                res->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
        }
        for (auto& res : m_KernelArgSRVs)
        {
            if (res.Get())
                res->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
        }
    }
    void RecordImpl() final;
    void OnComplete() final;

    ExecuteKernel(Kernel& kernel, cl_command_queue queue, std::array<uint32_t, 3> const& dims, std::array<uint32_t, 3> const& offset, std::array<uint16_t, 3> const& localSize, cl_uint workDims)
        : Task(kernel.m_Parent->GetContext(), CL_COMMAND_NDRANGE_KERNEL, queue)
        , m_Kernel(&kernel)
        , m_DispatchDims(dims)
        , m_KernelArgUAVs(kernel.m_UAVs.begin(), kernel.m_UAVs.end())
        , m_KernelArgSRVs(kernel.m_SRVs.begin(), kernel.m_SRVs.end())
        , m_KernelArgSamplers(kernel.m_Samplers.begin(), kernel.m_Samplers.end())
    {
        cl_uint KernelArgCBIndex = kernel.m_Dxil.GetMetadata().kernel_inputs_cbv_id;
        cl_uint WorkPropertiesCBIndex = kernel.m_Dxil.GetMetadata().work_properties_cbv_id;

        WorkProperties work_properties = {};
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

        size_t KernelInputsCbSize = kernel.m_Dxil.GetMetadata().kernel_inputs_buf_size;
        m_WorkPropertiesOffset = D3D12TranslationLayer::Align<size_t>(KernelInputsCbSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        auto pCompiler = g_Platform->GetCompiler();
        size_t WorkPropertiesSize = pCompiler->GetWorkPropertiesChunkSize() * numIterations;
        KernelInputsCbSize = m_WorkPropertiesOffset + WorkPropertiesSize;

        m_KernelArgsCbData.resize(KernelInputsCbSize);
        if (!kernel.m_KernelArgsCbData.empty())
        {
            memcpy(m_KernelArgsCbData.data(), kernel.m_KernelArgsCbData.data(), kernel.m_KernelArgsCbData.size());
        }
        std::byte* workPropertiesData = m_KernelArgsCbData.data() + m_WorkPropertiesOffset;
        for (cl_uint x = 0; x < numXIterations; ++x)
        {
            for (cl_uint y = 0; y < numYIterations; ++y)
            {
                for (cl_uint z = 0; z < numZIterations; ++z)
                {
                    work_properties.group_id_offset_x = x * D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                    work_properties.group_id_offset_y = y * D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                    work_properties.group_id_offset_z = z * D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                    workPropertiesData = pCompiler->CopyWorkProperties(workPropertiesData, work_properties);
                }
            }
        }

        auto& Device = m_CommandQueue->GetD3DDevice();

        D3D12TranslationLayer::ResourceCreationArgs Args = {};
        Args.m_appDesc.m_Subresources = 1;
        Args.m_appDesc.m_SubresourcesPerPlane = 1;
        Args.m_appDesc.m_NonOpaquePlaneCount = 1;
        Args.m_appDesc.m_MipLevels = 1;
        Args.m_appDesc.m_ArraySize = 1;
        Args.m_appDesc.m_Depth = 1;
        Args.m_appDesc.m_Width = (UINT)m_KernelArgsCbData.size();
        Args.m_appDesc.m_Height = 1;
        Args.m_appDesc.m_Format = DXGI_FORMAT_UNKNOWN;
        Args.m_appDesc.m_Samples = 1;
        Args.m_appDesc.m_Quality = 0;
        Args.m_appDesc.m_resourceDimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        Args.m_appDesc.m_usage = D3D12TranslationLayer::RESOURCE_USAGE_DYNAMIC;
        Args.m_appDesc.m_bindFlags = D3D12TranslationLayer::RESOURCE_BIND_CONSTANT_BUFFER;
        Args.m_appDesc.m_cpuAcess = D3D12TranslationLayer::RESOURCE_CPU_ACCESS_WRITE;
        Args.m_desc12 = CD3DX12_RESOURCE_DESC::Buffer(Args.m_appDesc.m_Width);
        Args.m_heapDesc = CD3DX12_HEAP_DESC(Args.m_appDesc.m_Width, D3D12_HEAP_TYPE_UPLOAD);
        Args.m_heapType = D3D12TranslationLayer::AllocatorHeapType::Upload;
        assert(Args.m_appDesc.m_Width % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);

        m_KernelArgsCb =
            D3D12TranslationLayer::Resource::CreateResource(
                &Device.ImmCtx(),
                Args,
                D3D12TranslationLayer::ResourceAllocationContext::FreeThread);

        if (kernel.m_Dxil.GetMetadata().printf_uav_id >= 0)
        {
            m_PrintfUAV.Attach(static_cast<Resource*>(clCreateBuffer(&m_Parent.get(), CL_MEM_ALLOC_HOST_PTR | CL_MEM_COPY_HOST_PTR, PrintfBufferSize, (void*)PrintfBufferInitialData, nullptr)));
            m_PrintfUAV->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
            m_KernelArgUAVs[kernel.m_Dxil.GetMetadata().printf_uav_id] = m_PrintfUAV.Get();
        }

        CompiledDxil::Configuration config = {};
        config.lower_int64 = true;
        config.lower_int16 = !m_Device->SupportsInt16();
        config.shader_model = Device.GetParent().GetShaderModel();
        config.support_global_work_id_offsets = std::any_of(std::begin(offset), std::end(offset), [](cl_uint v) { return v != 0; });
        config.support_work_group_id_offsets = numIterations != 1;
        std::copy(std::begin(localSize), std::end(localSize), config.local_size);
        config.args = kernel.m_ArgMetadataToCompiler;
        auto SpecKey = Program::SpecializationKey::Allocate(m_D3DDevice, config);
        
        auto SpecializationData = kernel.m_Parent->GetSpecializationData(m_Device.Get(), kernel.m_Name, std::move(SpecKey));
        m_Specialized = SpecializationData.Value;

        if (SpecializationData.NeedToCreate)
        {
            g_Platform->QueueProgramOp([this, &Device,
                                              config = std::move(config),
                                              kernel = this->m_Kernel,
                                              refThis = Task::ref_int(*this),
                                              SpecializationData]() mutable
            {
                try
                {
                    auto pCompiler = g_Platform->GetCompiler();
                    unique_dxil specialized;
                    auto spirv = kernel->m_Parent->GetSpirV(&m_CommandQueue->GetDevice());

                    auto &Cache = m_D3DDevice->GetShaderCache();
                    
                    SpookyHash hasher;
                    hasher.Init(SpecializationData.ProgramHash[0], SpecializationData.ProgramHash[1]);
                    hasher.Update(kernel->m_Name.c_str(), kernel->m_Name.size());
                    hasher.Update(&SpecializationData.KeyInMap->ConfigData,
                                  SpecializationData.KeyInMap->HashByteSize(SpecializationData.KeyInMap->NumArgs) -
                                    offsetof(Program::SpecializationKey, ConfigData));
                    uint64_t finalHash[2];
                    hasher.Final(&finalHash[0], &finalHash[1]);

                    auto found = Cache.Find(finalHash, sizeof(finalHash));
                    if (found.first)
                    {
                        // Adjust the metadata to match this specialization. Everything matches except the offsets
                        // to use for local args. The CL compiler treats unspecialized args as consuming 4 bytes.
                        // We don't have the metadata for how much local memory is embedded in the kernel definition,
                        // so the first local arg's offset tells us that.
                        auto metadata = kernel->m_Dxil.GetMetadata(); // copy
                        uint32_t offset = 0;
                        uint32_t last_size = 0;
                        for (uint32_t i = 0; i < metadata.args.size(); ++i)
                        {
                            if (auto local = std::get_if<CompiledDxil::Metadata::Arg::Local>(&metadata.args[i].properties))
                            {
                                if (last_size)
                                    local->sharedmem_offset = offset + last_size;
                                offset = local->sharedmem_offset;
                                last_size = std::get<CompiledDxil::Configuration::Arg::Local>(config.args[i].config).size;
                                // Match the logic in the compiler, which aligns these sizes based on the types it could contain,
                                // up to long16 which has a 128 byte alignment.
                                auto findFirstSet = [](uint32_t i)
                                    {
                                        unsigned long index;
                                        if (_BitScanForward(&index, i))
                                            return index + 1;
                                        else
                                            return 0ul;
                                    };
                                uint32_t align = last_size < 128 ? (1 << (findFirstSet(last_size) - 1)) : 128;
                                last_size = D3D12TranslationLayer::Align(last_size, align);
                            }
                        }
                        specialized = pCompiler->LoadKernel(*spirv, found.first.get(), found.second, metadata);
                    }
                    else
                    {
                        auto name = kernel->m_Dxil.GetMetadata().program_kernel_info.name;
                        specialized = pCompiler->GetKernel(name, *spirv, &config, nullptr);
                        specialized->Sign();

                        Cache.Store(finalHash, sizeof(finalHash), specialized->GetBinary(), specialized->GetBinarySize());
                    }

                    auto RS = kernel->GetRootSignature(Device.ImmCtx());

                    auto &DriverCache = m_D3DDevice->GetDriverShaderCache();
                    auto CachedBlob = DriverCache.Find(finalHash, sizeof(finalHash));
                    D3D12_CACHED_PIPELINE_STATE CachedDesc = {};
                    if (CachedBlob.first)
                    {
                        CachedDesc = { CachedBlob.first.get(), CachedBlob.second };
                    }

                    auto PSO = std::make_unique<D3D12TranslationLayer::PipelineState>(
                        &Device.ImmCtx(), D3D12_SHADER_BYTECODE{ specialized->GetBinary(), specialized->GetBinarySize() }, RS.get(), CachedDesc);

                    if (!CachedBlob.first)
                    {
                        D3D12TranslationLayer::unique_comptr<ID3DBlob> blob;
                        if (SUCCEEDED(PSO->GetForImmediateUse()->GetCachedBlob(&blob)))
                        {
                            DriverCache.Store(finalHash, sizeof(finalHash), blob->GetBufferPointer(), blob->GetBufferSize());
                        }
                    }

                    {
                        auto lock = kernel->m_Parent->GetSpecializationUpdateLock();
                        *m_Specialized = Program::SpecializationValue(std::move(specialized), std::move(RS), std::move(PSO));
                    }
                    kernel->m_Parent->SpecializationComplete();
                }
                catch (...)
                {
                    {
                        auto lock = kernel->m_Parent->GetSpecializationUpdateLock();
                        m_Specialized->m_Error = true;
                    }
                    kernel->m_Parent->SpecializationComplete();
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

    if (!kernel.AllArgsSet())
    {
        return ReportError("Cannot enqueue a kernel before all args are set.", CL_INVALID_KERNEL_ARGS);
    }

    std::array<uint32_t, 3> DispatchDimensions = { 1, 1, 1 };
    std::array<uint16_t, 3> LocalSizes = { 1, 1, 1 };
    auto RequiredDims = kernel.GetRequiredLocalDims();
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
            (RequiredDims ? RequiredDims[i] : 1);
        if (RequiredDims && local_work_size && RequiredDims[i] != local_work_size[i])
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

    for (cl_uint i = 0; i < work_dim; ++i)
    {
        DispatchDimensions[i] = (uint32_t)(global_work_size[i] / LocalSizes[i]);
    }

    if (RequiredDims || local_work_size)
    {
        if ((uint64_t)LocalSizes[0] * (uint64_t)LocalSizes[1] * (uint64_t)LocalSizes[2] > D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP)
        {
            return ReportError("local_work_size exceeds max work items per group.", CL_INVALID_WORK_GROUP_SIZE);
        }
    }
    else
    {
        // Try to partition this thread count into groups that fall between the min and max wave size.
        // Don't overshoot the max wave size, since threads in a group need to be scheduled together,
        // which can limit how many groups can run in parallel.
        std::pair<cl_uint, cl_uint> WaveSizes = queue.GetDevice().GetWaveSizes();
        cl_uint ThreadsInGroup = 1;
        // No device has a wave size > 128
        static constexpr uint16_t Primes[] =
        { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43,
          47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101,
          107, 109, 113, 127 };
        const uint16_t *FactorizationProgress[3] = { Primes, Primes, Primes };

        bool Progress;
        do
        {
            Progress = false;
            for (cl_uint dimension = 0; dimension < work_dim; ++dimension)
            {
                // Find the next factor that divides the dispatch size, for this dimension
                while (FactorizationProgress[dimension] != std::end(Primes))
                {
                    uint16_t Factor = *FactorizationProgress[dimension];
                    if (DispatchDimensions[dimension] < Factor ||
                        // Allow thread group size to increase past the max only if we're already at the minimum 
                        // and it will help to decrease how many dispatches we need to loop
                        (ThreadsInGroup * Factor > WaveSizes.second &&
                         ThreadsInGroup < WaveSizes.first &&
                         DispatchDimensions[dimension] <= D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) ||
                        // Unless it would cause us to exceed the max thread group size
                        ThreadsInGroup * Factor > D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP ||
                        LocalSizes[dimension] * Factor > MaxDims[dimension])
                    {
                        // No more factors in the list will ever match, this dimension is done
                        FactorizationProgress[dimension] = std::end(Primes);
                        break;
                    }
                    if (DispatchDimensions[dimension] % Factor == 0)
                    {
                        // Match
                        break;
                    }
                    ++FactorizationProgress[dimension];
                }
                // This dimension is done
                if (FactorizationProgress[dimension] == std::end(Primes))
                {
                    continue;
                }

                // Expand the local size
                uint16_t Factor = *FactorizationProgress[dimension];
                LocalSizes[dimension] *= Factor;
                ThreadsInGroup *= Factor;
                DispatchDimensions[dimension] /= Factor;
                Progress = true;

                // Stop if we hit the minimum wave size exactly, or once we exceed the min/max size
                if ((ThreadsInGroup == WaveSizes.first || ThreadsInGroup > WaveSizes.second) &&
                    DispatchDimensions[dimension] <= D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
                {
                    Progress = false;
                    break;
                }
            }
        } while (Progress);

        // If we're not going to launch even a single full wave, and the dispatch size for a dimension
        // can be used as a group size, then do so.
        // This means remaining dispatch dimensions are a prime number > 128 in all dimensions.
        for (cl_uint dimension = 0; dimension < work_dim && ThreadsInGroup < WaveSizes.first; ++dimension)
        {
            if (DispatchDimensions[dimension] > 1 &&
                DispatchDimensions[dimension] <= D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP &&
                (cl_uint)DispatchDimensions[dimension] * ThreadsInGroup <= D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP)
            {
                LocalSizes[dimension] *= (uint16_t)DispatchDimensions[dimension];
                ThreadsInGroup *= DispatchDimensions[dimension];
                DispatchDimensions[dimension] = 1;
            }
        }
    }

    bool IsEmptyDispatch = DispatchDimensions[0] == 0 || DispatchDimensions[1] == 0 || DispatchDimensions[2] == 0;

    try
    {
        std::unique_ptr<Task> task(IsEmptyDispatch ?
            (Task*)(new DummyTask(context, CL_COMMAND_NDRANGE_KERNEL, command_queue)) :
            (Task*)(new ExecuteKernel(kernel, command_queue, DispatchDimensions, GlobalWorkItemOffsets, LocalSizes, work_dim)));

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
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }

    return CL_SUCCESS;
}

extern CL_API_ENTRY CL_API_PREFIX__VERSION_1_2_DEPRECATED cl_int CL_API_CALL
clEnqueueTask(cl_command_queue  command_queue,
    cl_kernel         kernel,
    cl_uint           num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_2_DEPRECATED
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
    {
        auto lock = m_Kernel->m_Parent->GetSpecializationUpdateLock();
        while (!m_Specialized->m_PSO && !m_Specialized->m_Error)
        {
            m_Kernel->m_Parent->WaitForSpecialization(lock);
        }
    }

    if (m_Specialized->m_Error)
    {
        auto Lock = g_Platform->GetTaskPoolLock();
        Complete(CL_BUILD_PROGRAM_FAILURE, Lock);
        throw std::exception("Failed to specialize");
    }

    // Fill out offsets that'll be read by the kernel for local arg pointers, based on the offsets
    // returned by the compiler for this specialization
    for (UINT i = 0; i < m_Specialized->m_Dxil->GetMetadata().args.size(); ++i)
    {
        if (m_Specialized->m_Dxil->GetMetadata().program_kernel_info.args[i].address_qualifier != ProgramBinary::Kernel::Arg::AddressSpace::Local)
            continue;

        UINT *offsetLocation = reinterpret_cast<UINT*>(&m_KernelArgsCbData[m_Specialized->m_Dxil->GetMetadata().args[i].offset]);
        *offsetLocation = std::get<CompiledDxil::Metadata::Arg::Local>(m_Specialized->m_Dxil->GetMetadata().args[i].properties).sharedmem_offset;
    }

    auto &Device = m_CommandQueue->GetD3DDevice();
    auto &ImmCtx = Device.ImmCtx();

    D3D11_SUBRESOURCE_DATA Data = { m_KernelArgsCbData.data() };
    ImmCtx.UpdateSubresources(
        m_KernelArgsCb.get(),
        m_KernelArgsCb->GetFullSubresourceSubset(),
        &Data,
        nullptr,
        D3D12TranslationLayer::ImmediateContext::UpdateSubresourcesFlags::ScenarioInitialData);

    ImmCtx.GetResourceStateManager().TransitionResource(m_KernelArgsCb.get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> SrcDescriptors;
    UINT NumViewDescriptors = 2 + (UINT)m_KernelArgUAVs.size() + (UINT)m_KernelArgSRVs.size();
    UINT NumSamplerDescriptors = (UINT)m_KernelArgSamplers.size();
    SrcDescriptors.reserve(std::max(NumViewDescriptors - 2, NumSamplerDescriptors));

    ID3D12GraphicsCommandList *pCmdList = ImmCtx.GetGraphicsCommandList();
    pCmdList->SetComputeRootSignature(m_Specialized->m_RS->GetForUse());
    pCmdList->SetPipelineState(m_Specialized->m_PSO->GetForUse());

    if (NumSamplerDescriptors)
    {
        UINT SamplerSlot = ImmCtx.ReserveSlots(ImmCtx.m_SamplerHeap, NumSamplerDescriptors);
        for (auto &samp : m_KernelArgSamplers)
        {
            SrcDescriptors.push_back(samp->GetUnderlying(&Device).m_Descriptor);
        }
        ImmCtx.m_pDevice12->CopyDescriptors(1, &ImmCtx.m_SamplerHeap.CPUHandle(SamplerSlot), &NumSamplerDescriptors,
                                            NumSamplerDescriptors, SrcDescriptors.data(), nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        SrcDescriptors.clear();
        pCmdList->SetComputeRootDescriptorTable(1, ImmCtx.m_SamplerHeap.GPUHandle(SamplerSlot));
    }

    UINT RootUAVParamIdx = NumSamplerDescriptors ? 2 : 1; // First 2 params are view descriptor table and sample table (if present)
    for (auto &UavRes : m_KernelArgUAVs)
    {
        if (UavRes.Get())
        {
            auto &UAV = UavRes->GetUAV(&Device);
            Device.ImmCtx().GetResourceStateManager().TransitionSubresources(UavRes->GetUnderlyingResource(&Device),
                                                                             UAV.m_subresources,
                                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            SrcDescriptors.push_back(UAV.GetRefreshedDescriptorHandle());
            if (UavRes->m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
            {
                pCmdList->SetComputeRootUnorderedAccessView(RootUAVParamIdx,
                                                            D3D12TranslationLayer::GetBufferGPUVA(UavRes->GetUnderlyingResource(&Device), UavRes->m_Offset));
            }
        }
        else
        {
            SrcDescriptors.push_back(ImmCtx.m_NullUAV);
        }
        RootUAVParamIdx++;
    }
    for (auto &SrvRes : m_KernelArgSRVs)
    {
        auto &SRV = SrvRes->GetSRV(&Device);
        Device.ImmCtx().GetResourceStateManager().TransitionSubresources(SrvRes->GetUnderlyingResource(&Device),
                                                                         SRV.m_subresources,
                                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        SrcDescriptors.push_back(SRV.GetRefreshedDescriptorHandle());
    }

    auto pCompiler = g_Platform->GetCompiler();
    cl_uint WorkPropertiesChunkSize = (cl_uint)pCompiler->GetWorkPropertiesChunkSize();

    auto CopyAndSetViewDescriptors = [&](size_t WorkPropertiesOffset)
    {
        // The root signature indicates CBVs, then UAVs, then SRVs
        UINT ViewSlot = ImmCtx.ReserveSlots(ImmCtx.m_ViewHeap, NumViewDescriptors);
        UINT KernelArgsSlot = ViewSlot + m_Specialized->m_Dxil->GetMetadata().kernel_inputs_cbv_id;
        UINT WorkPropertiesSlot = ViewSlot + m_Specialized->m_Dxil->GetMetadata().work_properties_cbv_id;

        D3D12_CONSTANT_BUFFER_VIEW_DESC CBVDesc;
        D3D12TranslationLayer::GetBufferViewDesc(m_KernelArgsCb.get(), CBVDesc, 0, m_WorkPropertiesOffset);
        ImmCtx.m_pDevice12->CreateConstantBufferView(&CBVDesc, ImmCtx.m_ViewHeap.CPUHandle(KernelArgsSlot));
        CBVDesc.BufferLocation += WorkPropertiesOffset;
        CBVDesc.SizeInBytes = WorkPropertiesChunkSize;
        ImmCtx.m_pDevice12->CreateConstantBufferView(&CBVDesc, ImmCtx.m_ViewHeap.CPUHandle(WorkPropertiesSlot));

        UINT CopyStartSlot = ViewSlot + 2;
        UINT CopySize = NumViewDescriptors - 2;
        ImmCtx.m_pDevice12->CopyDescriptors(1, &ImmCtx.m_ViewHeap.CPUHandle(CopyStartSlot), &CopySize,
                                            CopySize, SrcDescriptors.data(), nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        pCmdList->SetComputeRootDescriptorTable(0, ImmCtx.m_ViewHeap.GPUHandle(ViewSlot));
    };

    ImmCtx.GetResourceStateManager().ApplyAllResourceTransitions();

    // TODO: Optimize this out
    ImmCtx.UAVBarrier();

    cl_uint numXIterations = ((m_DispatchDims[0] - 1) / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) + 1;
    cl_uint numYIterations = ((m_DispatchDims[1] - 1) / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) + 1;
    cl_uint numZIterations = ((m_DispatchDims[2] - 1) / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) + 1;
    size_t WorkPropertiesOffset = m_WorkPropertiesOffset;
    for (cl_uint x = 0; x < numXIterations; ++x)
    {
        for (cl_uint y = 0; y < numYIterations; ++y)
        {
            for (cl_uint z = 0; z < numZIterations; ++z)
            {
                UINT DimsX = (x == numXIterations - 1) ? (m_DispatchDims[0] - D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * (numXIterations - 1)) : D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                UINT DimsY = (y == numYIterations - 1) ? (m_DispatchDims[1] - D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * (numYIterations - 1)) : D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
                UINT DimsZ = (z == numZIterations - 1) ? (m_DispatchDims[2] - D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * (numZIterations - 1)) : D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;

                CopyAndSetViewDescriptors(WorkPropertiesOffset);
                ImmCtx.Dispatch(DimsX, DimsY, DimsZ);

                WorkPropertiesOffset += WorkPropertiesChunkSize;
            }
        }
    }
}

void ExecuteKernel::OnComplete()
{
    auto Cleanup = wil::scope_exit([this]()
    {
        m_Kernel.Release();
    });

    if (m_PrintfUAV.Get())
    {
        auto& Device = m_CommandQueue->GetD3DDevice();
        auto& ImmCtx = Device.ImmCtx();
        auto TranslationResource = m_PrintfUAV->GetUnderlyingResource(&Device);
        D3D12TranslationLayer::MappedSubresource MapRet = {};
        ImmCtx.Map(TranslationResource, 0, D3D12TranslationLayer::MAP_TYPE_READ, false, nullptr, &MapRet);

        auto Unmap = wil::scope_exit([&]()
        {
            ImmCtx.Unmap(TranslationResource, 0, D3D12TranslationLayer::MAP_TYPE_READ, nullptr);
        });

        // The buffer has a two-uint header.
        constexpr uint32_t InitialBufferOffset = sizeof(uint32_t) * 2;
        // The first uint is the offset where the next chunk of data would be written. Alternatively,
        // it's the size of the buffer that's *been* written, including the size of the header.
        uint32_t NumBytesWritten = *reinterpret_cast<uint32_t*>(MapRet.pData);
        uint32_t CurOffset = InitialBufferOffset;

        std::byte* ByteStream = reinterpret_cast<std::byte*>(MapRet.pData);
        while (CurOffset < NumBytesWritten && CurOffset < PrintfBufferSize)
        {
            uint32_t FormatStringId = *reinterpret_cast<uint32_t*>(ByteStream + CurOffset);
            assert(FormatStringId <= m_Kernel->m_Dxil.GetMetadata().printfs.size());
            if (FormatStringId == 0)
                break;

            auto& PrintfData = m_Kernel->m_Dxil.GetMetadata().printfs[FormatStringId - 1];
            CurOffset += sizeof(FormatStringId);
            auto StructBeginOffset = CurOffset;
            uint32_t OffsetInStruct = 0;

            uint32_t ArgIdx = 0;
            uint32_t TotalArgSize = std::accumulate(PrintfData.arg_sizes,
                                                    PrintfData.arg_sizes + PrintfData.num_args,
                                                    0u);
            TotalArgSize = D3D12TranslationLayer::Align<uint32_t>(TotalArgSize, 4);

            if (CurOffset + TotalArgSize > PrintfBufferSize)
                break;

            std::ostringstream stream;
            const char* SectionStart = PrintfData.str;
            while (const char* SectionEnd = strchr(SectionStart, '%'))
            {
                if (SectionEnd[1] == '%')
                {
                    stream << std::string_view(SectionStart, SectionEnd - SectionStart + 2);
                    SectionStart = SectionEnd + 2;
                    continue;
                }
                stream << std::string_view(SectionStart, SectionEnd - SectionStart);

                // Parse the printf declaration to find what type we should load
                char FinalFormatString[16] = "%", *OutputFormatString = FinalFormatString + 1;
                const char* FormatStr = SectionEnd + 1;
                for (; *FormatStr; ++FormatStr)
                {
                    switch (*FormatStr)
                    {
                    case '+':
                    case '-':
                    case ' ':
                    case '#':
                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    case '8':
                    case '9':
                    case '.':
                        // Flag, field width, or precision
                        *(OutputFormatString++) = *FormatStr;
                        continue;
                    }
                    break;
                }

                uint32_t VectorSize = 1;
                if (*FormatStr == 'v')
                {
                    ++FormatStr;
                    switch (*FormatStr)
                    {
                    case '2': VectorSize = 2; break;
                    case '3': VectorSize = 3; break;
                    case '4': VectorSize = 4; break;
                    case '8': VectorSize = 8; break;
                    case '1':
                        ++FormatStr;
                        if (*FormatStr == '6')
                        {
                            VectorSize = 16;
                            break;
                        }
                        // fallthrough
                    default:
                        printf("Invalid format string, unexpected vector size.\n");
                        return;
                    }
                    ++FormatStr;
                }

                uint32_t DataSize = 4;
                bool ExplicitDataSize = false;
                switch (*FormatStr)
                {
                case 'h':
                    ExplicitDataSize = true;
                    ++FormatStr;
                    if (*FormatStr == 'h')
                    {
                        DataSize = 1;
                        *(OutputFormatString++) = 'h';
                        *(OutputFormatString++) = 'h';
                        ++FormatStr;
                    }
                    else if (*FormatStr == 'l')
                    {
                        if (VectorSize == 1)
                        {
                            printf("Invalid format string, hl precision only valid with vectors.\n");
                            return;
                        }
                        DataSize = 4;
                        ++FormatStr;
                    }
                    else
                    {
                        *(OutputFormatString++) = 'h';
                        DataSize = 2;
                    }
                    break;
                case 'l':
                    ExplicitDataSize = true;
                    *(OutputFormatString++) = 'l';
                    ++FormatStr;
                    DataSize = 8;
                    break;
                }

                if (!ExplicitDataSize && VectorSize > 1)
                {
                    printf("Invalid format string, vectors require explicit data size.\n");
                    return;
                }

                *(OutputFormatString++) = *FormatStr;
                if (!ExplicitDataSize)
                {
                    switch (*FormatStr)
                    {
                    case 's':
                    case 'p':
                        // Pointers are 64bit
                        DataSize = 8;
                        break;
                    }
                }

                // Get the base pointer to the arg, now that we know how big it is
                uint32_t ArgSize = DataSize * (VectorSize == 3 ? 4 : VectorSize);
                assert(ArgSize == PrintfData.arg_sizes[ArgIdx]);
                uint32_t ArgOffset = D3D12TranslationLayer::Align<uint32_t>(OffsetInStruct, 4) + StructBeginOffset;
                std::byte* ArgPtr = ByteStream + ArgOffset;
                OffsetInStruct += ArgSize;

                std::string StringBuffer;
                StringBuffer.resize(32);
                for (uint32_t i = 0; i < VectorSize; ++i)
                {
                    switch (*FormatStr)
                    {
                    default:
                        printf("Invalid format string, unknown conversion specifier.\n");
                        return;
                    case 's':
                    {
                        if (DataSize != 8 || VectorSize != 1)
                        {
                            printf("Invalid format string, precision or vector applied to string.\n");
                            return;
                        }
                        uint64_t StringId = *reinterpret_cast<uint64_t*>(ArgPtr);
                        const char *Str = &PrintfData.str[StringId];
                        // Use sprintf to deal with precision potentially shortening how much is printed
                        StringBuffer.resize(snprintf(nullptr, 0, FinalFormatString, Str) + 1);
                        sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString, Str);
                        break;
                    }
                    case 'f':
                    case 'F':
                    case 'e':
                    case 'E':
                    case 'g':
                    case 'G':
                    case 'a':
                    case 'A':
                    {
                        if (ExplicitDataSize && DataSize != 4)
                        {
                            printf("Invalid format string, floats other than 4 bytes are not supported.\n");
                            return;
                        }
                        float val = *reinterpret_cast<float*>(ArgPtr);
                        sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString, val);
                        break;
                    }
                    break;
                    case 'c':
                        DataSize = 1;
                        // fallthrough
                    case 'd':
                    case 'i':
                        switch (DataSize)
                        {
                        case 1:
                            sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString,
                                      *reinterpret_cast<int8_t*>(ArgPtr));
                            break;
                        case 2:
                            sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString,
                                      *reinterpret_cast<int16_t*>(ArgPtr));
                            break;
                        case 4:
                            sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString,
                                      *reinterpret_cast<int32_t*>(ArgPtr));
                            break;
                        case 8:
                            sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString,
                                      *reinterpret_cast<int64_t*>(ArgPtr));
                            break;
                        }
                        break;
                    case 'o':
                    case 'u':
                    case 'x':
                    case 'X':
                    case 'p':
                        switch (DataSize)
                        {
                        case 1:
                            sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString,
                                      *reinterpret_cast<uint8_t*>(ArgPtr));
                            break;
                        case 2:
                            sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString,
                                      *reinterpret_cast<uint16_t*>(ArgPtr));
                            break;
                        case 4:
                            sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString,
                                      *reinterpret_cast<uint32_t*>(ArgPtr));
                            break;
                        case 8:
                            sprintf_s(StringBuffer.data(), StringBuffer.size(), FinalFormatString,
                                      *reinterpret_cast<uint64_t*>(ArgPtr));
                            break;
                        }
                        break;
                    }

                    ArgPtr += DataSize;
                    stream << StringBuffer.c_str();
                    if (i < VectorSize - 1)
                        stream << ",";
                }

                SectionStart = FormatStr + 1;
                ArgIdx++;
            }

            stream << SectionStart;
            printf("%s", stream.str().c_str());
            fflush(stdout);

            CurOffset += TotalArgSize;
        }
    }
}
