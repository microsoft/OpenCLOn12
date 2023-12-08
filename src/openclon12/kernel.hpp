// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "program.hpp"
#include "resources.hpp"
#include <cstddef>

class Sampler;
class Kernel : public CLChildBase<Kernel, Program, cl_kernel>
{
private:
    CompiledDxil const& m_Dxil;
    std::string const m_Name;
    D3D12TranslationLayer::SShaderDecls m_ShaderDecls;

    std::vector<std::byte> m_KernelArgsCbData;
    std::vector<CompiledDxil::Configuration::Arg> m_ArgMetadataToCompiler;
    std::vector<bool> m_ArgsSet;

    // These are weak references for the API kernel object, however
    // these will be converted into strong references by an *execution*
    // of that kernel. Releasing an object *while a kernel is enqueued*
    // must be safe (according to the CTS), while the API kernel must not
    // hold any references.
    std::vector<Resource*> m_UAVs;
    std::vector<Resource*> m_SRVs;
    std::vector<Sampler*> m_Samplers;

    std::vector<::ref_ptr<Sampler>> m_ConstSamplers;
    std::vector<::ref_ptr<Resource>> m_InlineConsts;

    friend class ExecuteKernel;
    friend extern CL_API_ENTRY cl_int CL_API_CALL clGetKernelInfo(cl_kernel, cl_kernel_info, size_t, void*, size_t*);
    friend extern CL_API_ENTRY cl_int CL_API_CALL clGetKernelArgInfo(cl_kernel, cl_uint, cl_kernel_arg_info, size_t, void*, size_t*);
    friend extern CL_API_ENTRY cl_int CL_API_CALL clGetKernelWorkGroupInfo(cl_kernel, cl_device_id, cl_kernel_work_group_info, size_t, void*, size_t*);

public:
    Kernel(Program& Parent, std::string const& name, CompiledDxil const& Dxil, ProgramBinary::Kernel const& meta);
    Kernel(Kernel const&);
    ~Kernel();

    cl_int SetArg(cl_uint arg_index, size_t arg_size, const void* arg_value);
    bool AllArgsSet() const;

    uint16_t const* GetRequiredLocalDims() const;
    uint16_t const* GetLocalDimsHint() const;

    const ProgramBinary::Kernel m_Meta;
};
