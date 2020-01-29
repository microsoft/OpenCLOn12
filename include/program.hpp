#pragma once

#include "context.hpp"
#undef GetBinaryType

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

    struct { const void* pBlob; size_t Size; } GetBinary() const { return { m_Binary.get(), m_BinarySize }; }

private:
    std::recursive_mutex m_Lock;
    std::unique_ptr<void, void(*)(void*)> m_Binary = { nullptr, operator delete };
    size_t m_BinarySize = 0;
    cl_program_binary_type m_BinaryType = CL_PROGRAM_BINARY_TYPE_NONE;

    cl_build_status m_BuildStatus = CL_BUILD_NONE;
    std::string m_BuildLog;

    // For reflection only
    std::string m_LastBuildOptions;

    std::vector<std::string> m_KernelNames;

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
