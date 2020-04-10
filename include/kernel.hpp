#pragma once

#include "program.hpp"
#include "resources.hpp"

class Kernel : public CLChildBase<Kernel, Program, cl_kernel>
{
private:
    clc_dxil_object const* m_pDxil;
    D3D12TranslationLayer::Shader m_Shader;
    D3D12TranslationLayer::RootSignature m_RootSig;
    D3D12TranslationLayer::PipelineState m_PSO;

    std::vector<D3D12TranslationLayer::UAV*> m_UAVs;
    std::vector<D3D12TranslationLayer::Resource*> m_CBs;
    std::vector<cl_uint> m_CBOffsets;
    std::vector<byte> m_KernelArgsCbData;

    friend class ExecuteKernel;
    friend extern CL_API_ENTRY cl_int CL_API_CALL clGetKernelInfo(cl_kernel, cl_kernel_info, size_t, void*, size_t*);
    friend extern CL_API_ENTRY cl_int CL_API_CALL clGetKernelArgInfo(cl_kernel, cl_uint, cl_kernel_arg_info, size_t, void*, size_t*);

public:
    Kernel(Program& Parent, clc_dxil_object const* pDxil);

    cl_int SetArg(cl_uint arg_index, size_t arg_size, const void* arg_value);
};
