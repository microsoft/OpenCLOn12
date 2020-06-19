// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "program.hpp"
#include "resources.hpp"

class Sampler;
class Kernel : public CLChildBase<Kernel, Program, cl_kernel>
{
private:
    clc_dxil_object const* m_pDxil;
    D3D12TranslationLayer::SShaderDecls m_ShaderDecls;

    std::vector<byte> m_KernelArgsCbData;
    std::vector<struct clc_runtime_arg_info> m_ArgMetadataToCompiler;

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

    // TODO: Consider moving this to the program so it can be shared
    // across multiple kernel instances for the same kernel name,
    // or clCloneKernel instances when we get there.
    struct SpecializationKey
    {
        union
        {
            struct
            {
                uint16_t LocalSize[3];
                uint16_t LowerInt64 : 1;
                uint16_t LowerInt16 : 1;
                uint16_t SupportGlobalOffsets : 1;
                uint16_t SupportLocalOffsets : 1;
                uint16_t Padding : 12;
            } Bits;
            uint64_t Value;
        } ConfigData;
        uint32_t NumArgs;
        union PackedArgData
        {
            uint32_t LocalArgSize;
            struct
            {
                unsigned NormalizedCoords : 1;
                unsigned AddressingMode : 3;
                unsigned LinearFiltering : 1;
                unsigned Padding : 27;
            } SamplerArgData;
        } Args[1];
        static std::unique_ptr<SpecializationKey> Allocate(struct clc_runtime_kernel_conf const& conf, struct clc_kernel_info const& info);
    private:
        SpecializationKey(struct clc_runtime_kernel_conf const& conf, struct clc_kernel_info const& info);
    };
    struct SpecializationKeyHash
    {
        size_t operator()(std::unique_ptr<SpecializationKey> const&) const;
    };
    struct SpecializationKeyEqual
    {
        bool operator()(std::unique_ptr<SpecializationKey> const& a, std::unique_ptr<SpecializationKey> const& b) const;
    };
    struct SpecializationValue
    {
        unique_dxil m_Dxil;
        std::unique_ptr<D3D12TranslationLayer::Shader> m_Shader;
        std::unique_ptr<D3D12TranslationLayer::PipelineState> m_PSO;
        SpecializationValue(decltype(m_Dxil) d, decltype(m_Shader) s, decltype(m_PSO) p)
            : m_Dxil(std::move(d)), m_Shader(std::move(s)), m_PSO(std::move(p)) { }
    };
    std::mutex m_SpecializationCacheLock;
    std::unordered_map<std::unique_ptr<SpecializationKey>, SpecializationValue,
                       SpecializationKeyHash, SpecializationKeyEqual> m_SpecializationCache;

    friend class ExecuteKernel;
    friend extern CL_API_ENTRY cl_int CL_API_CALL clGetKernelInfo(cl_kernel, cl_kernel_info, size_t, void*, size_t*);
    friend extern CL_API_ENTRY cl_int CL_API_CALL clGetKernelArgInfo(cl_kernel, cl_uint, cl_kernel_arg_info, size_t, void*, size_t*);
    friend extern CL_API_ENTRY cl_int CL_API_CALL clGetKernelWorkGroupInfo(cl_kernel, cl_device_id, cl_kernel_work_group_info, size_t, void*, size_t*);

public:
    Kernel(Program& Parent, clc_dxil_object const* pDxil);
    ~Kernel();

    cl_int SetArg(cl_uint arg_index, size_t arg_size, const void* arg_value);

    uint16_t const* GetRequiredLocalDims() const;
    uint16_t const* GetLocalDimsHint() const;
};
