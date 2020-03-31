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
    Program(Context& Parent, std::unique_ptr<byte[]> Binary, size_t BinarySize, cl_program_binary_type Type);
    Program(Context& Parent);
    using Callback = void(CL_CALLBACK*)(cl_program, void*);

    cl_int Build(const char* options, Callback pfn_notify, void* user_data);
    cl_int Compile(const char* options, cl_uint num_input_headers, const cl_program *input_headers, const char**header_include_names, Callback pfn_notify, void* user_data);
    cl_int Link(const char* options, cl_uint num_input_programs, const cl_program* input_programs, Callback pfn_notify, void* user_data);

    friend cl_int CL_API_CALL clGetProgramInfo(cl_program, cl_program_info, size_t, void*, size_t*);
    friend cl_int CL_API_CALL clGetProgramBuildInfo(cl_program, cl_device_id, cl_program_build_info, size_t, void*, size_t*);
    friend cl_kernel CL_API_CALL clCreateKernel(cl_program, const char*, cl_int*);
    friend cl_int CL_API_CALL clCreateKernelsInProgram(cl_program, cl_uint, cl_kernel*, cl_uint*);

private:
    std::recursive_mutex m_Lock;
    void* m_Binary = nullptr;
    size_t m_BinarySize = 0;
    cl_program_binary_type m_BinaryType = CL_PROGRAM_BINARY_TYPE_NONE;
    std::variant<std::monostate, unique_spirv, unique_dxil, unique_app_binary> m_OwnedBinary;

    cl_build_status m_BuildStatus = CL_BUILD_NONE;
    std::string m_BuildLog;

    // For reflection only
    std::string m_LastBuildOptions;

    std::vector<std::string> m_KernelNames = { "main_test" };

    struct CommonOptions
    {
        std::map<std::string, std::string> Defines;
        bool CreateLibrary;
        // TODO: Should I do the parsing of other args or should the compiler?
        //std::vector<std::string> IncludeDirectories;
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

};
