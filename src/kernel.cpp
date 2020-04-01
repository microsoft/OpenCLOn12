#include "kernel.hpp"
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

Kernel::Kernel(Program& Parent, clc_dxil_object const* pDxil)
    : CLChildBase(Parent)
    , m_pDxil(pDxil)
    , m_Shader(&Parent.GetDevice().ImmCtx(), pDxil->binary.data, pDxil->binary.size, D3D12TranslationLayer::SShaderDecls{})
    , m_RootSig(&Parent.GetDevice().ImmCtx(), 
        D3D12TranslationLayer::RootSignatureDesc(&m_Shader, false))
    , m_PSO(&Parent.GetDevice().ImmCtx(), D3D12TranslationLayer::COMPUTE_PIPELINE_STATE_DESC{ &m_Shader })
{
}

cl_int Kernel::SetArg(cl_uint arg_index, size_t arg_size, const void* arg_value)
{
    auto ReportError = m_Parent->GetContext().GetErrorReporter();
    if (arg_index != 0)
    {
        return ReportError("Error", CL_INVALID_ARG_INDEX);
    }
    if (arg_size != sizeof(void*))
    {
        return ReportError("Error", CL_INVALID_ARG_SIZE);
    }
    m_ResourceArgument = reinterpret_cast<Resource*>(const_cast<void*>(arg_value));
    return CL_SUCCESS;
}