// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "context.hpp"
#include "compiler.hpp"
#include <variant>
#undef GetBinaryType

using unique_spirv = std::unique_ptr<ProgramBinary>;
using unique_dxil = std::unique_ptr<CompiledDxil>;

class Kernel;
class Program : public CLChildBase<Program, Context, cl_program>
{
public:
    const std::string m_Source;
    const std::vector<std::byte> m_IL;

    Context& GetContext() const { return m_Parent.get(); }

    Program(Context& Parent, std::string Source);
    Program(Context& Parent, std::vector<std::byte> IL);
    Program(Context& Parent, std::vector<Device::ref_ptr_int> Devices);
    using Callback = void(CL_CALLBACK*)(cl_program, void*);

    cl_int Build(std::vector<Device::ref_ptr_int> Devices, const char* options, Callback pfn_notify, void* user_data);
    cl_int Compile(std::vector<Device::ref_ptr_int> Devices, const char* options, cl_uint num_input_headers, const cl_program *input_headers, const char**header_include_names, Callback pfn_notify, void* user_data);
    cl_int Link(const char* options, cl_uint num_input_programs, const cl_program* input_programs, Callback pfn_notify, void* user_data);

    void StoreBinary(Device* Device, unique_spirv OwnedBinary, cl_program_binary_type Type);

    const ProgramBinary* GetSpirV(Device* device) const;

    friend cl_int CL_API_CALL clGetProgramInfo(cl_program, cl_program_info, size_t, void*, size_t*);
    friend cl_int CL_API_CALL clGetProgramBuildInfo(cl_program, cl_device_id, cl_program_build_info, size_t, void*, size_t*);
    friend cl_kernel CL_API_CALL clCreateKernel(cl_program, const char*, cl_int*);
    friend cl_int CL_API_CALL clCreateKernelsInProgram(cl_program, cl_uint, cl_kernel*, cl_uint*);

    void KernelCreated();
    void KernelFreed();

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
        static std::unique_ptr<SpecializationKey> Allocate(CompiledDxil::Configuration const& conf);
    private:
        SpecializationKey(CompiledDxil::Configuration const& conf);
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

    SpecializationValue *FindExistingSpecialization(Device* device, std::string const& kernelName, std::unique_ptr<SpecializationKey> const& key) const;
    
    template <typename... TArgs>
    SpecializationValue *StoreSpecialization(Device* device, std::string const& kernelName, std::unique_ptr<SpecializationKey>& key, TArgs&&... args)
    {
        std::lock_guard programLock(m_Lock);
        auto& buildData = m_BuildData[device];
        std::lock_guard specializationCacheLock(buildData->m_SpecializationCacheLock);
        auto kernelsIter = buildData->m_Kernels.find(kernelName);
        assert(kernelsIter != buildData->m_Kernels.end());
        auto ret = kernelsIter->second.m_SpecializationCache.try_emplace(std::move(key), std::forward<TArgs>(args)...);
        return &ret.first->second;
    }

private:
    mutable std::recursive_mutex m_Lock;
    uint32_t m_NumLiveKernels = 0;

    struct KernelData
    {
        KernelData(unique_dxil d) : m_GenericDxil(std::move(d)) {}

        unique_dxil m_GenericDxil;
        std::unordered_map<std::unique_ptr<SpecializationKey>, SpecializationValue,
            SpecializationKeyHash, SpecializationKeyEqual> m_SpecializationCache;
    };

    struct PerDeviceData
    {
        Device* m_Device;
        cl_build_status m_BuildStatus = CL_BUILD_IN_PROGRESS;
        std::string m_BuildLog;
        unique_spirv m_OwnedBinary;
        cl_program_binary_type m_BinaryType = CL_PROGRAM_BINARY_TYPE_NONE;
        std::string m_LastBuildOptions;
        std::map<std::string, KernelData> m_Kernels;

        uint32_t m_NumPendingLinks = 0;

        void CreateKernels(Program& program);

        std::mutex m_SpecializationCacheLock;
    };
    std::unordered_map<Device*, std::shared_ptr<PerDeviceData>> m_BuildData;

    friend struct Loggers;

    std::vector<Device::ref_ptr_int> m_AssociatedDevices;

    struct CommonOptions
    {
        std::shared_ptr<PerDeviceData> BuildData;

        Compiler::CompileArgs::Features Features;
        std::vector<std::string> Args;
        bool CreateLibrary;
        bool EnableLinkOptions; // Does nothing, validation only
        Callback pfn_notify;
        void* CallbackUserData;
    };
    struct CompileArgs
    {
        std::map<std::string, Program::ref_ptr_int> Headers;
        CommonOptions Common;
    };
    struct LinkArgs
    {
        CommonOptions Common;
        std::vector<Program::ref_ptr_int> LinkPrograms;
    };
    struct BuildArgs
    {
        CommonOptions Common;
        std::vector<Device::ref_ptr_int> BinaryBuildDevices;
    };

    void AddBuiltinOptions(std::vector<Device::ref_ptr_int> const& devices, CommonOptions& optionsStruct);
    cl_int ParseOptions(const char* optionsStr, CommonOptions& optionsStruct, bool SupportCompilerOptions, bool SupportLinkerOptions);
    cl_int BuildImpl(BuildArgs const& Args);
    cl_int CompileImpl(CompileArgs const& Args);
    cl_int LinkImpl(LinkArgs const& Args);
};
