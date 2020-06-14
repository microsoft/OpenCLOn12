// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "context.hpp"
#include <variant>
#undef GetBinaryType

struct clc_object;
struct clc_dxil_object;

using unique_spirv = std::unique_ptr<clc_object, void(*)(clc_object*)>;
using unique_dxil = std::unique_ptr<clc_dxil_object, void(*)(clc_dxil_object*)>;
using unique_app_binary = std::unique_ptr<byte[]>;

class Kernel;
class Program : public CLChildBase<Program, Context, cl_program>
{
public:
    const std::string m_Source;

    Context& GetContext() const { return m_Parent.get(); }
    Device& GetDevice() const { return GetContext().GetDevice(); }

    Program(Context& Parent, std::string Source);
    Program(Context& Parent, unique_spirv Binary, cl_program_binary_type Type);
    Program(Context& Parent);
    using Callback = void(CL_CALLBACK*)(cl_program, void*);

    cl_int Build(const char* options, Callback pfn_notify, void* user_data);
    cl_int Compile(const char* options, cl_uint num_input_headers, const cl_program *input_headers, const char**header_include_names, Callback pfn_notify, void* user_data);
    cl_int Link(const char* options, cl_uint num_input_programs, const cl_program* input_programs, Callback pfn_notify, void* user_data);

    const clc_dxil_object* GetKernel(const char* name) const;
    const clc_object* GetSpirV() const { return m_OwnedBinary.get(); }

    friend cl_int CL_API_CALL clGetProgramInfo(cl_program, cl_program_info, size_t, void*, size_t*);
    friend cl_int CL_API_CALL clGetProgramBuildInfo(cl_program, cl_device_id, cl_program_build_info, size_t, void*, size_t*);
    friend cl_kernel CL_API_CALL clCreateKernel(cl_program, const char*, cl_int*);
    friend cl_int CL_API_CALL clCreateKernelsInProgram(cl_program, cl_uint, cl_kernel*, cl_uint*);

    void KernelCreated();
    void KernelFreed();

private:
    mutable std::recursive_mutex m_Lock;
    unique_spirv m_OwnedBinary{ nullptr, nullptr };
    cl_program_binary_type m_BinaryType = CL_PROGRAM_BINARY_TYPE_NONE;
    uint32_t m_NumLiveKernels = 0;

    cl_build_status m_BuildStatus = CL_BUILD_NONE;
    std::string m_BuildLog;

    // For reflection only
    std::string m_LastBuildOptions;

    std::map<std::string, unique_dxil> m_Kernels;

    struct CommonOptions
    {
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
    };

    cl_int ParseOptions(const char* optionsStr, CommonOptions& optionsStruct, bool SupportCompilerOptions, bool SupportLinkerOptions);
    void BuildImpl(BuildArgs const& Args);
    void CompileImpl(CompileArgs const& Args);
    void LinkImpl(LinkArgs const& Args);

    void CreateKernels();

};
