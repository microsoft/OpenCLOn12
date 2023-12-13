// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "kernel.hpp"
#include "sampler.hpp"
#include "compiler.hpp"

extern CL_API_ENTRY cl_kernel CL_API_CALL
clCreateKernel(cl_program      program_,
    const char* kernel_name,
    cl_int* errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!program_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_PROGRAM;
        return nullptr;
    }

    Program& program = *static_cast<Program*>(program_);
    auto ReportError = program.GetContext().GetErrorReporter(errcode_ret);
    const CompiledDxil* kernel = nullptr;
    const ProgramBinary::Kernel *meta = nullptr;

    {
        std::lock_guard Lock(program.m_Lock);
        cl_uint DeviceCountWithProgram = 0, DeviceCountWithKernel = 0;
        for (auto& [Device, _] : program.m_AssociatedDevices)
        {
            auto& BuildData = program.m_BuildData[Device.Get()];
            if (!BuildData ||
                BuildData->m_BuildStatus != CL_BUILD_SUCCESS ||
                BuildData->m_BinaryType != CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
            {
                continue;
            }

            ++DeviceCountWithProgram;
            auto iter = BuildData->m_Kernels.find(kernel_name);
            if (iter == BuildData->m_Kernels.end())
            {
                continue;
            }

            ++DeviceCountWithKernel;
            if (kernel)
            {
                auto& first_info = kernel->GetMetadata().program_kernel_info;
                auto& second_info = iter->second.m_GenericDxil->GetMetadata().program_kernel_info;
                if (first_info.args.size() != second_info.args.size())
                {
                    return ReportError("Kernel argument count differs between devices.", CL_INVALID_KERNEL_DEFINITION);
                }
                for (unsigned i = 0; i < first_info.args.size(); ++i)
                {
                    auto& a = first_info.args[i];
                    auto& b = second_info.args[i];
                    if (strcmp(a.type_name, b.type_name) != 0 ||
                        strcmp(a.name, b.name) != 0 ||
                        a.address_qualifier != b.address_qualifier ||
                        a.readable != b.readable ||
                        a.writable != b.writable ||
                        a.is_const != b.is_const ||
                        a.is_restrict != b.is_restrict ||
                        a.is_volatile != b.is_volatile)
                    {
                        return ReportError("Kernel argument differs between devices.", CL_INVALID_KERNEL_DEFINITION);
                    }
                }
            }
            kernel = iter->second.m_GenericDxil.get();
            meta = &iter->second.m_Meta;
            if (!kernel)
            {
                return ReportError("Kernel failed to compile.", CL_OUT_OF_RESOURCES);
            }
        }
        if (!DeviceCountWithProgram)
        {
            return ReportError("No executable available for program.", CL_INVALID_PROGRAM_EXECUTABLE);
        }
        if (!DeviceCountWithKernel)
        {
            return ReportError("No kernel with that name found.", CL_INVALID_KERNEL_NAME);
        }
    }

    try
    {
        if (errcode_ret) *errcode_ret = CL_SUCCESS;
        return new Kernel(program, kernel_name, *kernel, *meta);
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clCreateKernelsInProgram(cl_program     program_,
    cl_uint        num_kernels,
    cl_kernel* kernels,
    cl_uint* num_kernels_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!program_)
    {
        return CL_INVALID_PROGRAM;
    }

    Program& program = *static_cast<Program*>(program_);
    auto ReportError = program.GetContext().GetErrorReporter();

    try
    {
        std::map<std::string, Kernel::ref_ptr> temp;

        {
            std::lock_guard Lock(program.m_Lock);
            for (auto& [Device, _] : program.m_AssociatedDevices)
            {
                auto& BuildData = program.m_BuildData[Device.Get()];
                if (!BuildData ||
                    BuildData->m_BuildStatus != CL_BUILD_SUCCESS ||
                    BuildData->m_BinaryType != CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
                {
                    continue;
                }

                for (auto& pair : BuildData->m_Kernels)
                {
                    temp.emplace(pair.first, nullptr);
                }
            }
            if (temp.empty())
            {
                return ReportError("No executable available for program.", CL_INVALID_PROGRAM_EXECUTABLE);
            }
            if (num_kernels && num_kernels < temp.size())
            {
                return ReportError("num_kernels is too small.", CL_INVALID_VALUE);
            }
        }
        if (num_kernels_ret)
        {
            *num_kernels_ret = (cl_uint)temp.size();
        }

        if (num_kernels)
        {
            for (auto& pair : temp)
            {
                cl_int error = CL_SUCCESS;
                pair.second.Attach(static_cast<Kernel*>(clCreateKernel(program_, pair.first.c_str(), &error)));
                if (error != CL_SUCCESS)
                {
                    return error;
                }
            }
            for (auto& pair : temp)
            {
                *kernels = pair.second.Detach();
                ++kernels;
            }
        }
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clRetainKernel(cl_kernel    kernel) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel)
    {
        return CL_INVALID_KERNEL;
    }
    static_cast<Kernel*>(kernel)->Retain();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clReleaseKernel(cl_kernel   kernel) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel)
    {
        return CL_INVALID_KERNEL;
    }
    static_cast<Kernel*>(kernel)->Release();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clSetKernelArg(cl_kernel    kernel,
    cl_uint      arg_index,
    size_t       arg_size,
    const void* arg_value) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel)
    {
        return CL_INVALID_KERNEL;
    }
    return static_cast<Kernel*>(kernel)->SetArg(arg_index, arg_size, arg_value);
}

static cl_mem_object_type MemObjectTypeFromName(const char* name)
{
    if (strcmp(name, "image1d_buffer_t") == 0) return CL_MEM_OBJECT_IMAGE1D_BUFFER;
    if (strcmp(name, "image1d_t") == 0) return CL_MEM_OBJECT_IMAGE1D;
    if (strcmp(name, "image1d_array_t") == 0) return CL_MEM_OBJECT_IMAGE1D_ARRAY;
    if (strcmp(name, "image2d_t") == 0) return CL_MEM_OBJECT_IMAGE2D;
    if (strcmp(name, "image2d_array_t") == 0) return CL_MEM_OBJECT_IMAGE2D_ARRAY;
    if (strcmp(name, "image3d_t") == 0) return CL_MEM_OBJECT_IMAGE3D;
    return 0;
}

static ComPtr<ID3DBlob> SerializeRootSignature(CompiledDxil::Metadata const& metadata)
{
    static constexpr cl_uint MaxRootCost = D3D12_MAX_ROOT_COST;
    static constexpr cl_uint BaseRootCost = 2; /* Two descriptor tables take one DWORD each */
    static constexpr cl_uint RootDescriptorAvailableRootCost = MaxRootCost - BaseRootCost;
    static constexpr cl_uint RootDescriptorMaxCount = RootDescriptorAvailableRootCost / 2; /* Two DWORDs per descriptor */

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RSDesc;
    CD3DX12_ROOT_PARAMETER1 Params[2 + RootDescriptorMaxCount];
    CD3DX12_DESCRIPTOR_RANGE1 ViewRanges[4], SamplerRange;
    cl_uint NumRanges = 0;
    ViewRanges[NumRanges++].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, 0);
    ViewRanges[NumRanges++].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, (UINT)metadata.num_uavs, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 2);
    if (metadata.num_srvs)
    {
        ViewRanges[NumRanges++].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)metadata.num_srvs, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
    }
    cl_uint NumParameters = 0;
    Params[NumParameters++].InitAsDescriptorTable(NumRanges, ViewRanges);
    if (metadata.num_samplers)
    {
        // TODO: Static samplers
        SamplerRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, (UINT)metadata.num_samplers, 0);
        Params[NumParameters++].InitAsDescriptorTable(1, &SamplerRange);
    }

    for (size_t i = 0; i < metadata.args.size() && NumParameters < ARRAYSIZE(Params); ++i)
    {
        auto &properties = metadata.args[i].properties;
        auto pMemProps = std::get_if<CompiledDxil::Metadata::Arg::Memory>(&properties);
        if (pMemProps)
        {
            // Buffers should be the first UAVs; if they're not, this code needs to also build a mapping so that the binding
            // logic can set the appropriate root UAVs
            assert(pMemProps->buffer_id == NumParameters - (metadata.num_samplers ? 2 : 1));
            Params[NumParameters++].InitAsUnorderedAccessView(pMemProps->buffer_id, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);
        }
    }
    for (size_t i = 0; i < metadata.consts.size() && NumParameters < ARRAYSIZE(Params); ++i)
    {
        assert(metadata.consts[i].uav_id == NumParameters - (metadata.num_samplers ? 2 : 1));
        Params[NumParameters++].InitAsUnorderedAccessView(metadata.consts[i].uav_id, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);
    }
    if (metadata.printf_uav_id >= 0 && NumParameters < ARRAYSIZE(Params))
    {
        assert(metadata.printf_uav_id == NumParameters - (metadata.num_samplers ? 2 : 1));
        Params[NumParameters++].InitAsUnorderedAccessView(metadata.printf_uav_id, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);
    }
    if (NumParameters == ARRAYSIZE(Params))
    {
        // Ran out of space for root descriptors, just reference these buffers via descriptor table
        ViewRanges[NumRanges++].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, (UINT)metadata.num_uavs, 0,
                                     1, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 2);
        Params[0].DescriptorTable.NumDescriptorRanges++;
        NumParameters = 2;
    }

    RSDesc.Init_1_1(NumParameters, Params);

    ComPtr<ID3DBlob> ret;
    D3D12TranslationLayer::ThrowFailure(D3D12SerializeVersionedRootSignature(&RSDesc, &ret, nullptr));
    return ret;
}

static cl_addressing_mode CLAddressingModeFromSpirv(unsigned addressing_mode)
{
    return addressing_mode + CL_ADDRESS_NONE;
}

static unsigned SpirvAddressingModeFromCL(cl_addressing_mode mode)
{
    return mode - CL_ADDRESS_NONE;
}

static cl_filter_mode CLFilterModeFromSpirv(unsigned filter_mode)
{
    return filter_mode + CL_FILTER_NEAREST;
}

Kernel::Kernel(Program& Parent, std::string const& name, CompiledDxil const& Dxil, ProgramBinary::Kernel const& spirv_meta)
    : CLChildBase(Parent)
    , m_Dxil(Dxil)
    , m_Name(name)
    , m_Meta(spirv_meta)
    , m_SerializedRootSignature(SerializeRootSignature(Dxil.GetMetadata()))
{
    m_UAVs.resize(Dxil.GetMetadata().num_uavs);
    m_SRVs.resize(Dxil.GetMetadata().num_srvs);
    m_Samplers.resize(Dxil.GetMetadata().num_samplers);
    m_ArgMetadataToCompiler.resize(Dxil.GetMetadata().args.size());
    m_ArgsSet.resize(Dxil.GetMetadata().args.size());
    for (cl_uint i = 0; i < Dxil.GetMetadata().args.size(); ++i)
    {
        auto& meta = Dxil.GetMetadata().args[i];
        auto& config = m_ArgMetadataToCompiler[i].config;
        if (std::holds_alternative<CompiledDxil::Metadata::Arg::Local>(meta.properties))
            config = CompiledDxil::Configuration::Arg::Local{ 0 };
        else if (std::holds_alternative<CompiledDxil::Metadata::Arg::Sampler>(meta.properties))
            config = CompiledDxil::Configuration::Arg::Sampler{};
    }
    size_t KernelInputsCbSize = Dxil.GetMetadata().kernel_inputs_buf_size;
    m_KernelArgsCbData.resize(KernelInputsCbSize);

    m_ConstSamplers.reserve(Dxil.GetMetadata().constSamplers.size());
    for (auto& samplerMeta : Dxil.GetMetadata().constSamplers)
    {
        Sampler::Desc desc =
        {
            samplerMeta.normalized_coords,
            CLAddressingModeFromSpirv(samplerMeta.addressing_mode),
            CLFilterModeFromSpirv(samplerMeta.filter_mode)
        };
        m_ConstSamplers.emplace_back(new Sampler(m_Parent->GetContext(), desc, nullptr));
        m_Samplers[samplerMeta.sampler_id] = m_ConstSamplers.back().Get();
    }

    for (auto& constMeta : Dxil.GetMetadata().consts)
    {
        auto resource = static_cast<Resource*>(clCreateBuffer(&Parent.GetContext(),
                                                              CL_MEM_COPY_HOST_PTR | CL_MEM_READ_ONLY | CL_MEM_HOST_NO_ACCESS,
                                                              constMeta.size, constMeta.data,
                                                              nullptr));
        m_InlineConsts.emplace_back(resource, adopt_ref{});
        m_UAVs[constMeta.uav_id] = resource;
    }

    m_Parent->KernelCreated();
}

Kernel::Kernel(Kernel const& other)
    : CLChildBase(other.m_Parent.get())
    , m_Dxil(other.m_Dxil)
    , m_Name(other.m_Name)
    , m_SerializedRootSignature(other.m_SerializedRootSignature)
    , m_UAVs(other.m_UAVs)
    , m_SRVs(other.m_SRVs)
    , m_Samplers(other.m_Samplers)
    , m_ArgMetadataToCompiler(other.m_ArgMetadataToCompiler)
    , m_KernelArgsCbData(other.m_KernelArgsCbData)
    , m_ConstSamplers(other.m_ConstSamplers)
    , m_InlineConsts(other.m_InlineConsts)
    , m_Meta(other.m_Meta)
    , m_ArgsSet(other.m_ArgsSet)
{
    m_Parent->KernelCreated();
}

Kernel::~Kernel()
{
    m_Parent->KernelFreed();
}

cl_int Kernel::SetArg(cl_uint arg_index, size_t arg_size, const void* arg_value)
{
    auto ReportError = m_Parent->GetContext().GetErrorReporter();
    if (arg_index > m_Dxil.GetMetadata().args.size())
    {
        return ReportError("Argument index out of bounds", CL_INVALID_ARG_INDEX);
    }

    auto& arg_meta = m_Dxil.GetMetadata().args[arg_index];
    auto& arg_info = m_Dxil.GetMetadata().program_kernel_info.args[arg_index];
    switch (arg_info.address_qualifier)
    {
    case ProgramBinary::Kernel::Arg::AddressSpace::Global:
    case ProgramBinary::Kernel::Arg::AddressSpace::Constant:
    {
        if (arg_size != sizeof(cl_mem))
        {
            return ReportError("Invalid argument size, must be sizeof(cl_mem) for global and constant arguments", CL_INVALID_ARG_SIZE);
        }

        cl_mem_object_type imageType = MemObjectTypeFromName(arg_info.type_name);
        cl_mem mem = arg_value ? *reinterpret_cast<cl_mem const*>(arg_value) : nullptr;
        Resource* resource = static_cast<Resource*>(mem);
        if (imageType != 0)
        {
            auto& imageMeta = std::get<CompiledDxil::Metadata::Arg::Image>(arg_meta.properties);
            bool validImageType = true;
            if (resource)
            {
                validImageType = resource->m_Desc.image_type == imageType;
            }

            if (!validImageType)
            {
                return ReportError("Invalid image type.", CL_INVALID_ARG_VALUE);
            }

            if (arg_info.writable)
            {
                if (resource && (resource->m_Flags & CL_MEM_READ_ONLY))
                {
                    return ReportError("Invalid mem object flags, binding read-only image to writable image argument.", CL_INVALID_ARG_VALUE);
                }
                if (arg_info.readable &&
                    resource && (resource->m_Flags & CL_MEM_WRITE_ONLY))
                {
                    return ReportError("Invalid mem object flags, binding write-only image to read-write image argument.", CL_INVALID_ARG_VALUE);
                }
                for (cl_uint i = 0; i < imageMeta.num_buffer_ids; ++i)
                {
                    m_UAVs[imageMeta.buffer_ids[i]] = resource;
                }
            }
            else
            {
                if (resource && (resource->m_Flags & CL_MEM_WRITE_ONLY))
                {
                    return ReportError("Invalid mem object flags, binding write-only image to read-only image argument.", CL_INVALID_ARG_VALUE);
                }
                for (cl_uint i = 0; i < imageMeta.num_buffer_ids; ++i)
                {
                    m_SRVs[imageMeta.buffer_ids[i]] = resource;
                }
            }

            // Store image format in the kernel args
            cl_image_format* ImageFormatInKernelArgs = reinterpret_cast<cl_image_format*>(
                m_KernelArgsCbData.data() + arg_meta.offset);
            *ImageFormatInKernelArgs = {};
            if (resource)
            {
                *ImageFormatInKernelArgs = resource->m_Format;
                // The SPIR-V expects the values coming from the intrinsics to be 0-indexed, and implicitly
                // adds the necessary values to put it back into the CL constant range
                ImageFormatInKernelArgs->image_channel_data_type -= CL_SNORM_INT8;
                ImageFormatInKernelArgs->image_channel_order -= CL_R;
            }
        }
        else
        {
            if (resource && resource->m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
            {
                return ReportError("Invalid mem object type, must be buffer.", CL_INVALID_ARG_VALUE);
            }
            auto& memMeta = std::get<CompiledDxil::Metadata::Arg::Memory>(arg_meta.properties);
            uint64_t *buffer_val = reinterpret_cast<uint64_t*>(m_KernelArgsCbData.data() + arg_meta.offset);
            m_UAVs[memMeta.buffer_id] = resource;
            if (resource)
            {
                *buffer_val = (uint64_t)memMeta.buffer_id << 32ull;
            }
            else
            {
                *buffer_val = ~0ull;
            }
        }

        break;
    }

    case ProgramBinary::Kernel::Arg::AddressSpace::Private:
        if (strcmp(arg_info.type_name, "sampler_t") == 0)
        {
            if (arg_size != sizeof(cl_sampler))
            {
                return ReportError("Invalid argument size, must be sizeof(cl_mem) for global arguments", CL_INVALID_ARG_SIZE);
            }
            cl_sampler samp = arg_value ? *reinterpret_cast<cl_sampler const*>(arg_value) : nullptr;
            Sampler* sampler = static_cast<Sampler*>(samp);
            auto& samplerMeta = std::get<CompiledDxil::Metadata::Arg::Sampler>(arg_meta.properties);
            auto& samplerConfig = std::get<CompiledDxil::Configuration::Arg::Sampler>(m_ArgMetadataToCompiler[arg_index].config);
            m_Samplers[samplerMeta.sampler_id] = sampler;
            samplerConfig.normalizedCoords = sampler ? sampler->m_Desc.NormalizedCoords : 1u;
            samplerConfig.addressingMode = sampler ? SpirvAddressingModeFromCL(sampler->m_Desc.AddressingMode) : 0u;
            samplerConfig.linearFiltering = sampler ? (sampler->m_Desc.FilterMode == CL_FILTER_LINEAR) : 0u;
        }
        else
        {
            if (arg_size != arg_meta.size)
            {
                return ReportError("Invalid argument size", CL_INVALID_ARG_SIZE);
            }
            memcpy(m_KernelArgsCbData.data() + arg_meta.offset, arg_value, arg_size);
        }
        break;

    case ProgramBinary::Kernel::Arg::AddressSpace::Local:
        if (arg_size == 0)
        {
            return ReportError("Argument size must be nonzero for local arguments", CL_INVALID_ARG_SIZE);
        }
        if (arg_value != nullptr)
        {
            return ReportError("Argument value must be null for local arguments", CL_INVALID_ARG_VALUE);
        }
        auto& localConfig = std::get<CompiledDxil::Configuration::Arg::Local>(m_ArgMetadataToCompiler[arg_index].config);
        localConfig.size = (cl_uint)arg_size;
        break;
    }

    m_ArgsSet[arg_index] = true;
    return CL_SUCCESS;
}

bool Kernel::AllArgsSet() const
{
    return std::all_of(m_ArgsSet.begin(), m_ArgsSet.end(), [](bool b) { return b; });
}

uint16_t const* Kernel::GetRequiredLocalDims() const
{
    if (m_Dxil.GetMetadata().local_size[0] != 0)
        return m_Dxil.GetMetadata().local_size;
    return nullptr;
}

uint16_t const* Kernel::GetLocalDimsHint() const
{
    if (m_Dxil.GetMetadata().local_size_hint[0] != 0)
        return m_Dxil.GetMetadata().local_size_hint;
    return nullptr;
}

std::unique_ptr<D3D12TranslationLayer::RootSignature> Kernel::GetRootSignature(ImmCtx &ImmCtx) const
{
    auto pRS = std::make_unique<D3D12TranslationLayer::RootSignature>(&ImmCtx);
    pRS->Create(m_SerializedRootSignature->GetBufferPointer(), m_SerializedRootSignature->GetBufferSize());
    return pRS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetKernelInfo(cl_kernel       kernel_,
    cl_kernel_info  param_name,
    size_t          param_value_size,
    void* param_value,
    size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel_)
    {
        return CL_INVALID_KERNEL;
    }

    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    auto& kernel = *static_cast<Kernel*>(kernel_);
    switch (param_name)
    {
    case CL_KERNEL_FUNCTION_NAME: return RetValue(kernel.m_Dxil.GetMetadata().program_kernel_info.name);
    case CL_KERNEL_NUM_ARGS: return RetValue((cl_uint)kernel.m_Dxil.GetMetadata().args.size());
    case CL_KERNEL_REFERENCE_COUNT: return RetValue(kernel.GetRefCount());
    case CL_KERNEL_CONTEXT: return RetValue((cl_context)&kernel.m_Parent->m_Parent.get());
    case CL_KERNEL_PROGRAM: return RetValue((cl_program)&kernel.m_Parent.get());
    case CL_KERNEL_ATTRIBUTES:
    {
        if (kernel.m_Parent->m_Source.empty())
        {
            // For kernels not created from OpenCL C source and the clCreateProgramWithSource API call the string returned from this query will be empty.
            return RetValue("");
        }
        std::string result;
        char tempBuf[64];
        if (kernel.m_Meta.vec_hint_size)
        {
            sprintf_s(tempBuf, "vec_type_hint(%s%d) ",
                      [](ProgramBinary::Kernel::VecHintType type) {
                          using T = decltype(type);
                          switch (type)
                          {
                          case T::Char: return "uchar";
                          case T::Short: return "ushort";
                          case T::Int: return "uint";
                          case T::Long: return "ulong";
                          case T::Half: return "half";
                          case T::Float: return "float";
                          case T::Double: return "double";
                          default: return "";
                          }
                      }(kernel.m_Meta.vec_hint_type),
                      kernel.m_Meta.vec_hint_size);
            result += tempBuf;
        }
        auto ReqLocalSize = kernel.GetRequiredLocalDims();
        if (ReqLocalSize)
        {
            sprintf_s(tempBuf, "reqd_work_group_size(%d,%d,%d) ", ReqLocalSize[0], ReqLocalSize[1], ReqLocalSize[2]);
            result += tempBuf;
        }
        auto LocalSizeHint = kernel.GetLocalDimsHint();
        if (LocalSizeHint)
        {
            sprintf_s(tempBuf, "work_group_size_hint(%d,%d,%d) ", LocalSizeHint[0], LocalSizeHint[1], LocalSizeHint[2]);
            result += tempBuf;
        }
        return RetValue(result.c_str());
    }
    }

    return kernel.m_Parent->GetContext().GetErrorReporter()("Unknown param_name", CL_INVALID_VALUE);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetKernelArgInfo(cl_kernel       kernel_,
    cl_uint         arg_indx,
    cl_kernel_arg_info  param_name,
    size_t          param_value_size,
    void* param_value,
    size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_2
{
    if (!kernel_)
    {
        return CL_INVALID_KERNEL;
    }

    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    auto& kernel = *static_cast<Kernel*>(kernel_);
    
    if (arg_indx > kernel.m_Dxil.GetMetadata().args.size())
    {
        return CL_INVALID_ARG_INDEX;
    }

    auto& arg_info = kernel.m_Dxil.GetMetadata().program_kernel_info.args[arg_indx];
    switch (param_name)
    {
    case CL_KERNEL_ARG_ADDRESS_QUALIFIER:
        switch (arg_info.address_qualifier)
        {
        default:
        case ProgramBinary::Kernel::Arg::AddressSpace::Private: return RetValue(CL_KERNEL_ARG_ADDRESS_PRIVATE);
        case ProgramBinary::Kernel::Arg::AddressSpace::Constant: return RetValue(CL_KERNEL_ARG_ADDRESS_CONSTANT);
        case ProgramBinary::Kernel::Arg::AddressSpace::Local: return RetValue(CL_KERNEL_ARG_ADDRESS_LOCAL);
        case ProgramBinary::Kernel::Arg::AddressSpace::Global: return RetValue(CL_KERNEL_ARG_ADDRESS_GLOBAL);
        }
        break;
    case CL_KERNEL_ARG_ACCESS_QUALIFIER:
        if (arg_info.writable && arg_info.readable)
            return RetValue(CL_KERNEL_ARG_ACCESS_READ_WRITE);
        else if (arg_info.writable)
            return RetValue(CL_KERNEL_ARG_ACCESS_WRITE_ONLY);
        else if (arg_info.readable)
            return RetValue(CL_KERNEL_ARG_ACCESS_READ_ONLY);
        else
            return RetValue(CL_KERNEL_ARG_ACCESS_NONE);
    case CL_KERNEL_ARG_TYPE_NAME: return RetValue(arg_info.type_name);
    case CL_KERNEL_ARG_TYPE_QUALIFIER:
    {
        cl_kernel_arg_type_qualifier qualifier = CL_KERNEL_ARG_TYPE_NONE;
        if (arg_info.is_const ||
            arg_info.address_qualifier == ProgramBinary::Kernel::Arg::AddressSpace::Constant) qualifier |= CL_KERNEL_ARG_TYPE_CONST;
        if (arg_info.is_restrict) qualifier |= CL_KERNEL_ARG_TYPE_RESTRICT;
        if (arg_info.is_volatile) qualifier |= CL_KERNEL_ARG_TYPE_VOLATILE;
        return RetValue(qualifier);
    }
    case CL_KERNEL_ARG_NAME:
        if (arg_info.name) return RetValue(arg_info.name);
        return CL_KERNEL_ARG_INFO_NOT_AVAILABLE;
    }

    return kernel.m_Parent->GetContext().GetErrorReporter()("Unknown param_name", CL_INVALID_VALUE);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetKernelWorkGroupInfo(cl_kernel                  kernel_,
    cl_device_id               device,
    cl_kernel_work_group_info  param_name,
    size_t                     param_value_size,
    void *                     param_value,
    size_t *                   param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel_)
    {
        return CL_INVALID_KERNEL;
    }
    UNREFERENCED_PARAMETER(device);

    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    auto& kernel = *static_cast<Kernel*>(kernel_);

    switch (param_name)
    {
    case CL_KERNEL_WORK_GROUP_SIZE: return RetValue((size_t)D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP);
    case CL_KERNEL_COMPILE_WORK_GROUP_SIZE:
    {
        size_t size[3] = {};
        auto ReqDims = kernel.GetRequiredLocalDims();
        if (ReqDims)
            std::copy(ReqDims, ReqDims + 3, size);
        return RetValue(size);
    }
    case CL_KERNEL_LOCAL_MEM_SIZE:
    {
        size_t size = kernel.m_Dxil.GetMetadata().local_mem_size;
        for (cl_uint i = 0; i < kernel.m_Dxil.GetMetadata().args.size(); ++i)
        {
            if (kernel.m_Dxil.GetMetadata().program_kernel_info.args[i].address_qualifier == ProgramBinary::Kernel::Arg::AddressSpace::Local)
            {
                size -= 4;
                size += std::get<CompiledDxil::Configuration::Arg::Local>(kernel.m_ArgMetadataToCompiler[i].config).size;
            }
        }
        return RetValue(size);
    }
    case CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE: return RetValue((size_t)64);
    case CL_KERNEL_PRIVATE_MEM_SIZE: return RetValue(kernel.m_Dxil.GetMetadata().priv_mem_size);
    }

    return CL_INVALID_VALUE;
}

extern CL_API_ENTRY cl_kernel CL_API_CALL
clCloneKernel(cl_kernel     source_kernel,
    cl_int*       errcode_ret) CL_API_SUFFIX__VERSION_2_1
{
    if (!source_kernel)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_KERNEL;
        return nullptr;
    }
    Kernel &kernel = *static_cast<Kernel*>(source_kernel);
    auto ReportError = kernel.m_Parent->m_Parent->GetErrorReporter(errcode_ret);
    try
    {
        if (errcode_ret) *errcode_ret = CL_SUCCESS;
        return new Kernel(kernel);
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}
