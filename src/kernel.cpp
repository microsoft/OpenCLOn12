#include "kernel.hpp"
#include "sampler.hpp"
#include "clc_compiler.h"

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
    const clc_dxil_object* kernel;

    {
        std::lock_guard Lock(program.m_Lock);
        if (program.m_BuildStatus != CL_BUILD_SUCCESS ||
            program.m_BinaryType != CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
        {
            return ReportError("No executable available for program.", CL_INVALID_PROGRAM_EXECUTABLE);
        }

        kernel = program.GetKernel(kernel_name);
        if (kernel == nullptr)
        {
            return ReportError("No kernel with that name present in program.", CL_INVALID_KERNEL_NAME);
        }
    }

    try
    {
        return new Kernel(program, kernel);
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

    {
        std::lock_guard Lock(program.m_Lock);
        if (program.m_BuildStatus != CL_BUILD_SUCCESS ||
            program.m_BinaryType != CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
        {
            return ReportError("No executable available for program.", CL_INVALID_PROGRAM_EXECUTABLE);
        }
        if (num_kernels > 0 && num_kernels < program.m_Kernels.size())
        {
            return ReportError("num_kernels is too small.", CL_INVALID_VALUE);
        }
    }

    if (num_kernels > 0)
    {
        try
        {
            std::vector<Kernel::ref_ptr> temp;
            temp.resize(program.m_Kernels.size());
            std::transform(program.m_Kernels.begin(), program.m_Kernels.end(), temp.begin(),
                [&program](std::pair<const std::string, unique_dxil> const& pair)
                {
                    return Kernel::ref_ptr(new Kernel(program, pair.second.get()), adopt_ref{});
                });

            for (cl_uint i = 0; i < program.m_Kernels.size(); ++i)
            {
                kernels[i] = temp[i].Detach();
            }
        }
        catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
        catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
        catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    }
    if (num_kernels_ret)
    {
        *num_kernels_ret = (cl_uint)program.m_Kernels.size();
    }
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

static D3D12TranslationLayer::RESOURCE_DIMENSION ResourceDimensionFromMemObjectType(cl_mem_object_type type)
{
    switch (type)
    {
    case CL_MEM_OBJECT_IMAGE1D: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE1D;
    case CL_MEM_OBJECT_IMAGE1D_ARRAY: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE1DARRAY;
    case CL_MEM_OBJECT_IMAGE1D_BUFFER: return D3D12TranslationLayer::RESOURCE_DIMENSION::BUFFER;
    case CL_MEM_OBJECT_IMAGE2D: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE2D;
    case CL_MEM_OBJECT_IMAGE2D_ARRAY: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE2DARRAY;
    case CL_MEM_OBJECT_IMAGE3D: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE3D;
    }
    return D3D12TranslationLayer::RESOURCE_DIMENSION::UNKNOWN;
}

static D3D12TranslationLayer::SShaderDecls DeclsFromMetadata(clc_dxil_object const* pDxil)
{
    auto& metadata = pDxil->metadata;
    D3D12TranslationLayer::SShaderDecls decls = {};
    decls.m_NumCBs = (UINT)metadata.num_consts + 2;
    decls.m_NumSamplers = (UINT)metadata.num_samplers;
    decls.m_ResourceDecls.resize(metadata.num_srvs);
    decls.m_UAVDecls.resize(metadata.num_uavs);

    for (cl_uint i = 0; i < pDxil->kernel->num_args; ++i)
    {
        auto& arg = pDxil->kernel->args[i];
        if (arg.address_qualifier == CLC_KERNEL_ARG_ADDRESS_GLOBAL)
        {
            cl_mem_object_type imageType = MemObjectTypeFromName(arg.type_name);
            if (imageType != 0)
            {
                auto dim = ResourceDimensionFromMemObjectType(imageType);
                bool uav = (arg.access_qualifier & CLC_KERNEL_ARG_ACCESS_WRITE) != 0;
                auto& declVector = uav ? decls.m_UAVDecls : decls.m_ResourceDecls;
                for (cl_uint j = 0; j < metadata.args[i].image.num_buf_ids; ++j)
                    declVector[metadata.args[i].image.buf_ids[j]] = dim;
            }
            else if (strcmp(arg.type_name, "sampler_t") != 0)
            {
                decls.m_UAVDecls[metadata.args[i].globalptr.buf_id] =
                    D3D12TranslationLayer::RESOURCE_DIMENSION::RAW_BUFFER;
            }
        }
    }
    return decls;
}

Kernel::Kernel(Program& Parent, clc_dxil_object const* pDxil)
    : CLChildBase(Parent)
    , m_pDxil(pDxil)
    , m_ShaderDecls(DeclsFromMetadata(pDxil))
    , m_RootSig(&Parent.GetDevice().ImmCtx(), 
        D3D12TranslationLayer::RootSignatureDesc(&m_ShaderDecls, false))
{
    m_UAVs.resize(m_pDxil->metadata.num_uavs);
    m_SRVs.resize(m_pDxil->metadata.num_srvs);
    m_Samplers.resize(m_pDxil->metadata.num_samplers);
    m_CBs.resize(m_pDxil->metadata.num_consts + 2);
    m_CBOffsets.resize(m_pDxil->metadata.num_consts + 2);
    m_ArgMetadataToCompiler.resize(m_pDxil->kernel->num_args);
    size_t KernelInputsCbSize = m_pDxil->metadata.kernel_inputs_buf_size;
    KernelInputsCbSize = D3D12TranslationLayer::Align<size_t>(KernelInputsCbSize + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
                                                              D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    m_KernelArgsCbData.resize(KernelInputsCbSize);
    m_CBOffsets[m_pDxil->metadata.global_work_offset_cbv_id] = (UINT)(KernelInputsCbSize - D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    // Fill out the fixed data in the kernel args buffer
    for (cl_uint i = 0; i < m_pDxil->kernel->num_args; ++i)
    {
        if (m_pDxil->kernel->args[i].address_qualifier == CLC_KERNEL_ARG_ADDRESS_GLOBAL &&
            MemObjectTypeFromName(m_pDxil->kernel->args[i].type_name) == 0 &&
            strcmp(m_pDxil->kernel->args[i].type_name, "sampler_t") != 0)
        {
            auto& arg = m_pDxil->metadata.args[i];
            cl_uint value = arg.globalptr.buf_id << 28;
            byte* KernelArgBase = m_KernelArgsCbData.data() + arg.offset;
            *reinterpret_cast<cl_uint*>(KernelArgBase) = value;
        }
    }

    // TODO: Do we need constant buffers for global constant data?
}

Kernel::~Kernel() = default;

cl_int Kernel::SetArg(cl_uint arg_index, size_t arg_size, const void* arg_value)
{
    auto ReportError = m_Parent->GetContext().GetErrorReporter();
    if (arg_index > m_pDxil->kernel->num_args)
    {
        return ReportError("Argument index out of bounds", CL_INVALID_ARG_INDEX);
    }

    auto& arg = m_pDxil->kernel->args[arg_index];
    switch (arg.address_qualifier)
    {
    case CLC_KERNEL_ARG_ADDRESS_GLOBAL:
    {
        static_assert(sizeof(cl_mem) == sizeof(cl_sampler));
        if (arg_size != sizeof(cl_mem))
        {
            return ReportError("Invalid argument size, must be sizeof(cl_mem) for global arguments", CL_INVALID_ARG_SIZE);
        }

        cl_mem_object_type imageType = MemObjectTypeFromName(arg.type_name);
        if (imageType != 0)
        {
            cl_mem mem = arg_value ? *reinterpret_cast<cl_mem const*>(arg_value) : nullptr;
            Resource* resource = static_cast<Resource*>(mem);
            bool validImageType = true;
            if (resource)
            {
                validImageType = resource->m_Desc.image_type == imageType;
            }

            if (!validImageType)
            {
                return ReportError("Invalid image type.", CL_INVALID_ARG_VALUE);
            }

            if (arg.access_qualifier & CLC_KERNEL_ARG_ACCESS_WRITE)
            {
                if (resource && (resource->m_Flags & CL_MEM_READ_ONLY))
                {
                    return ReportError("Invalid mem object flags, binding read-only image to writable image argument.", CL_INVALID_ARG_VALUE);
                }
                if ((arg.access_qualifier & CLC_KERNEL_ARG_ACCESS_READ) == 0 &&
                    resource && (resource->m_Flags & CL_MEM_WRITE_ONLY))
                {
                    return ReportError("Invalid mem object flags, binding write-only image to read-write image argument.", CL_INVALID_ARG_VALUE);
                }
                D3D12TranslationLayer::UAV* uav = resource ? &resource->GetUAV() : nullptr;
                for (cl_uint i = 0; i < m_pDxil->metadata.args[arg_index].image.num_buf_ids; ++i)
                {
                    m_UAVs[m_pDxil->metadata.args[arg_index].image.buf_ids[i]] = uav;
                }
            }
            else
            {
                if (resource && (resource->m_Flags & CL_MEM_READ_ONLY))
                {
                    return ReportError("Invalid mem object flags, binding write-only image to read-only image argument.", CL_INVALID_ARG_VALUE);
                }
                D3D12TranslationLayer::SRV* srv = resource ? &resource->GetSRV() : nullptr;
                for (cl_uint i = 0; i < m_pDxil->metadata.args[arg_index].image.num_buf_ids; ++i)
                {
                    m_SRVs[m_pDxil->metadata.args[arg_index].image.buf_ids[i]] = srv;
                }
            }

            // Store image format in the kernel args
            cl_image_format* ImageFormatInKernelArgs = reinterpret_cast<cl_image_format*>(
                m_KernelArgsCbData.data() + m_pDxil->metadata.args[arg_index].offset);
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
        else if (strcmp(arg.type_name, "sampler_t") == 0)
        {
            cl_sampler samp = arg_value ? *reinterpret_cast<cl_sampler const*>(arg_value) : nullptr;
            Sampler* sampler = static_cast<Sampler*>(samp);
            D3D12TranslationLayer::Sampler* underlying = sampler ? &sampler->GetUnderlying() : nullptr;
            m_Samplers[m_pDxil->metadata.args[arg_index].sampler.sampler_id] = underlying;
            // Store normalized coords in the kernel args
            *reinterpret_cast<cl_uint*>(m_KernelArgsCbData.data() + m_pDxil->metadata.args[arg_index].offset) =
                sampler ? sampler->m_Desc.NormalizedCoords : 1u;
        }
        else
        {
            cl_mem mem = arg_value ? *reinterpret_cast<cl_mem const*>(arg_value) : nullptr;
            Resource* resource = static_cast<Resource*>(mem);
            if (resource && resource->m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
            {
                return ReportError("Invalid mem object type, must be buffer.", CL_INVALID_ARG_VALUE);
            }
            D3D12TranslationLayer::UAV* uav = resource ? &resource->GetUAV() : nullptr;
            m_UAVs[m_pDxil->metadata.args[arg_index].globalptr.buf_id] = uav;
        }

        break;
    }

    case CLC_KERNEL_ARG_ADDRESS_CONSTANT:
        if (arg_size != sizeof(cl_mem))
        {
            return ReportError("Invalid argument size, must be sizeof(cl_mem) for constant arguments", CL_INVALID_ARG_SIZE);
        }
        // TODO: Which CB index?
        return ReportError("Unsupported argument type", CL_OUT_OF_HOST_MEMORY);
        break;

    case CLC_KERNEL_ARG_ADDRESS_PRIVATE:
        if (arg_size != m_pDxil->metadata.args[arg_index].size)
        {
            return ReportError("Invalid argument size", CL_INVALID_ARG_SIZE);
        }
        memcpy(m_KernelArgsCbData.data() + m_pDxil->metadata.args[arg_index].offset, arg_value, arg_size);
        break;

    case CLC_KERNEL_ARG_ADDRESS_LOCAL:
        if (arg_size == 0)
        {
            return ReportError("Argument size must be nonzero for local arguments", CL_INVALID_ARG_SIZE);
        }
        if (arg_value != nullptr)
        {
            return ReportError("Argument value must be null for local arguments", CL_INVALID_ARG_VALUE);
        }
        m_ArgMetadataToCompiler[arg_index].localptr.size = (cl_uint)arg_size;
        break;
    }

    return CL_SUCCESS;
}

uint16_t const* Kernel::GetRequiredLocalDims() const
{
    if (m_pDxil->metadata.local_size[0] != 0)
        return m_pDxil->metadata.local_size;
    return nullptr;
}

uint16_t const* Kernel::GetLocalDimsHint() const
{
    if (m_pDxil->metadata.local_size_hint[0] != 0)
        return m_pDxil->metadata.local_size_hint;
    return nullptr;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetKernelInfo(cl_kernel       kernel_,
    cl_kernel_info  param_name,
    size_t          param_value_size,
    void* param_value,
    size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (kernel_)
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
    case CL_KERNEL_FUNCTION_NAME: return RetValue(kernel.m_pDxil->kernel->name);
    case CL_KERNEL_NUM_ARGS: return RetValue((cl_uint)kernel.m_pDxil->kernel->num_args);
    case CL_KERNEL_REFERENCE_COUNT: return RetValue(kernel.GetRefCount());
    case CL_KERNEL_CONTEXT: return RetValue((cl_context)&kernel.m_Parent->m_Parent.get());
    case CL_KERNEL_PROGRAM: return RetValue((cl_program)&kernel.m_Parent.get());
    case CL_KERNEL_ATTRIBUTES: return RetValue("");
    }

    return CL_INVALID_VALUE;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetKernelArgInfo(cl_kernel       kernel_,
    cl_uint         arg_indx,
    cl_kernel_arg_info  param_name,
    size_t          param_value_size,
    void* param_value,
    size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_2
{
    if (kernel_)
    {
        return CL_INVALID_KERNEL;
    }

    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    auto& kernel = *static_cast<Kernel*>(kernel_);
    
    if (arg_indx > kernel.m_pDxil->kernel->num_args)
    {
        return CL_INVALID_ARG_INDEX;
    }

    auto& arg = kernel.m_pDxil->kernel->args[arg_indx];
    switch (param_name)
    {
    case CL_KERNEL_ARG_ADDRESS_QUALIFIER:
        switch (arg.address_qualifier)
        {
        default:
        case CLC_KERNEL_ARG_ADDRESS_PRIVATE: return RetValue(CL_KERNEL_ARG_ADDRESS_PRIVATE);
        case CLC_KERNEL_ARG_ADDRESS_CONSTANT: return RetValue(CL_KERNEL_ARG_ADDRESS_CONSTANT);
        case CLC_KERNEL_ARG_ADDRESS_LOCAL: return RetValue(CL_KERNEL_ARG_ADDRESS_LOCAL);
        case CLC_KERNEL_ARG_ADDRESS_GLOBAL: return RetValue(CL_KERNEL_ARG_ADDRESS_GLOBAL);
        }
        break;
    case CL_KERNEL_ARG_ACCESS_QUALIFIER:
        // Only valid for images, and there's no metadata for that yet
        return RetValue(CL_KERNEL_ARG_ACCESS_NONE);
    case CL_KERNEL_ARG_TYPE_NAME: return RetValue(arg.type_name);
    case CL_KERNEL_ARG_TYPE_QUALIFIER:
    {
        cl_kernel_arg_type_qualifier qualifier = CL_KERNEL_ARG_TYPE_NONE;
        if (arg.type_qualifier & CLC_KERNEL_ARG_TYPE_CONST) qualifier |= CL_KERNEL_ARG_TYPE_CONST;
        if (arg.type_qualifier & CLC_KERNEL_ARG_TYPE_RESTRICT) qualifier |= CL_KERNEL_ARG_TYPE_RESTRICT;
        if (arg.type_qualifier & CLC_KERNEL_ARG_TYPE_VOLATILE) qualifier |= CL_KERNEL_ARG_TYPE_VOLATILE;
        return RetValue(qualifier);
    }
    case CL_KERNEL_ARG_NAME:
        if (arg.name) return RetValue(arg.name);
        return CL_KERNEL_ARG_INFO_NOT_AVAILABLE;
    }

    return CL_INVALID_VALUE;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetKernelWorkGroupInfo(cl_kernel                  kernel,
    cl_device_id               device,
    cl_kernel_work_group_info  param_name,
    size_t                     param_value_size,
    void *                     param_value,
    size_t *                   param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    UNREFERENCED_PARAMETER(kernel);
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(param_name);
    UNREFERENCED_PARAMETER(param_value_size);
    UNREFERENCED_PARAMETER(param_value);
    UNREFERENCED_PARAMETER(param_value_size_ret);
    return CL_INVALID_PLATFORM;
}
