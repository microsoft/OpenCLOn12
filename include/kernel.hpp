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

    Resource* m_ResourceArgument = nullptr;

    friend class ExecuteKernel;

public:
    Kernel(Program& Parent, clc_dxil_object const* pDxil);

    cl_int SetArg(cl_uint arg_index, size_t arg_size, const void* arg_value);
};
