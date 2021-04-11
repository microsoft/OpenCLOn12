// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "program.hpp"
#include "clc_compiler.h"
#include "kernel.hpp"
#include <dxc/dxcapi.h>

#include <algorithm>

struct ProgramBinaryHeader
{
    static constexpr GUID c_ValidHeaderGuid = { /* 8d46c01e-2977-4234-a5b0-292405fc1d34 */
        0x8d46c01e, 0x2977, 0x4234, {0xa5, 0xb0, 0x29, 0x24, 0x05, 0xfc, 0x1d, 0x34} };
    const GUID HeaderGuid = c_ValidHeaderGuid;
    cl_program_binary_type BinaryType = CL_PROGRAM_BINARY_TYPE_NONE;
    uint32_t BinarySize = 0;

    ProgramBinaryHeader() = default;
    ProgramBinaryHeader(const clc_object* obj, cl_program_binary_type type)
        : BinaryType(type)
        , BinarySize((uint32_t)obj->spvbin.size)
    {
    }
    struct CopyBinaryContentsTag {};
    ProgramBinaryHeader(const clc_object* obj, cl_program_binary_type type, CopyBinaryContentsTag)
        : ProgramBinaryHeader(obj, type)
    {
        memcpy(GetBinary(), obj->spvbin.data, BinarySize);
    }

    size_t ComputeFullBlobSize() const
    {
        return sizeof(*this) + BinarySize;
    }
    void* GetBinary() { return this + 1; }
    const void* GetBinary() const { return this + 1; }
};

void SignBlob(void* pBlob, size_t size)
{
    auto& DXIL = g_Platform->GetDXIL();
    auto pfnCreateInstance = DXIL.proc_address<decltype(&DxcCreateInstance)>("DxcCreateInstance");
    ComPtr<IDxcValidator> spValidator;
    if (SUCCEEDED(pfnCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(&spValidator))))
    {
        struct Blob : IDxcBlob
        {
            void* pBlob;
            UINT Size;
            Blob(void* p, UINT s) : pBlob(p), Size(s) { }
            STDMETHOD(QueryInterface)(REFIID, void** ppv) { *ppv = this; return S_OK; }
            STDMETHOD_(ULONG, AddRef)() { return 1; }
            STDMETHOD_(ULONG, Release)() { return 0; }
            STDMETHOD_(void*, GetBufferPointer)() override { return pBlob; }
            STDMETHOD_(SIZE_T, GetBufferSize)() override { return Size; }
        } Blob = { pBlob, (UINT)size };
        ComPtr<IDxcOperationResult> spResult;
        (void)spValidator->Validate(&Blob, DxcValidatorFlags_InPlaceEdit, &spResult);
        HRESULT hr = S_OK;
        if (spResult)
        {
            (void)spResult->GetStatus(&hr);
        }
        if (FAILED(hr))
        {
            ComPtr<IDxcBlobEncoding> spError;
            spResult->GetErrorBuffer(&spError);
            BOOL known = FALSE;
            UINT32 cp = 0;
            spError->GetEncoding(&known, &cp);
            if (cp == CP_UTF8 || cp == CP_ACP)
                printf("%s", (char*)spError->GetBufferPointer());
            else
                printf("%S", (wchar_t*)spError->GetBufferPointer());
            DebugBreak();
        }
    }
}

extern CL_API_ENTRY cl_program CL_API_CALL
clCreateProgramWithSource(cl_context        context_,
    cl_uint           count,
    const char** strings,
    const size_t* lengths,
    cl_int* errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!context_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return nullptr;
    }
    Context& context = *static_cast<Context*>(context_);
    auto ReportError = context.GetErrorReporter(errcode_ret);

    if (count == 0)
    {
        return ReportError("count is zero.", CL_INVALID_VALUE);
    }

    try
    {
        std::string CompleteProgram;
        for (cl_uint i = 0; i < count; ++i)
        {
            if (strings[i] == nullptr)
            {
                return ReportError("strings contains a NULL entry.", CL_INVALID_VALUE);
            }
            size_t length = (lengths && lengths[i]) ? lengths[i] : strlen(strings[i]);
            CompleteProgram.append(strings[i], length);
        }

        if (errcode_ret) *errcode_ret = CL_SUCCESS;
        return new Program(context, std::move(CompleteProgram));
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_program CL_API_CALL
clCreateProgramWithBinary(cl_context                     context_,
    cl_uint                        num_devices,
    const cl_device_id* device_list,
    const size_t* lengths,
    const unsigned char** binaries,
    cl_int* binary_status,
    cl_int* errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!context_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return nullptr;
    }
    Context& context = *static_cast<Context*>(context_);
    auto ReportError = context.GetErrorReporter(errcode_ret);

    if (num_devices == 0 || device_list == nullptr)
    {
        return ReportError("num_devices must not be zero and device_list must not be NULL.", CL_INVALID_VALUE);
    }
    if (!lengths || !binaries)
    {
        if (binary_status)
            std::fill(binary_status, binary_status + num_devices, CL_INVALID_VALUE);
        return ReportError("lengths, binaries, and the entries within must not be NULL.", CL_INVALID_VALUE);
    }

    try
    {
        bool ReturnError = false;

        std::vector<Device::ref_ptr_int> device_refs;
        for (cl_uint i = 0; i < num_devices; ++i)
        {
            Device* device = static_cast<Device*>(device_list[i]);
            if (!context.ValidDeviceForContext(*device))
            {
                return ReportError("Device in device_list does not belong to context.", CL_INVALID_DEVICE);
            }

            if (!lengths[i] || !binaries[i])
            {
                if (binary_status)
                    binary_status[i] = CL_INVALID_VALUE;
                ReportError("lengths, binaries, and the entries within must not be NULL.", CL_INVALID_VALUE);
                ReturnError = true;
            }

            auto header = reinterpret_cast<ProgramBinaryHeader const*>(binaries[i]);
            try
            {
                if (lengths[i] < sizeof(ProgramBinaryHeader))
                    throw std::exception("Binary size too small");
                if (header->HeaderGuid != header->c_ValidHeaderGuid)
                    throw std::exception("Invalid binary header");
                if (lengths[i] < header->ComputeFullBlobSize())
                    throw std::exception("Binary size provided is smaller than expected, binary appears truncated");
            }
            catch (std::exception& ex)
            {
                if (binary_status)
                    binary_status[i] = CL_INVALID_BINARY;
                ReportError(ex.what(), CL_INVALID_BINARY);
                ReturnError = true;
            }
            device_refs.emplace_back(device);
        }
        if (ReturnError)
        {
            return nullptr;
        }

        std::unique_ptr<Program> NewProgram(new Program(context, device_refs));

        for (cl_uint i = 0; i < num_devices; ++i)
        {
            auto header = reinterpret_cast<ProgramBinaryHeader const*>(binaries[i]);
            unique_spirv BinaryHolder(new clc_object,
                [](clc_object* obj)
            {
                if (obj->spvbin.data)
                    delete[] reinterpret_cast<byte*>(obj->spvbin.data);
                delete obj;
            });
            BinaryHolder->spvbin.data = reinterpret_cast<uint32_t*>(new byte[header->BinarySize]);
            BinaryHolder->spvbin.size = header->BinarySize;
            memcpy(BinaryHolder->spvbin.data, header->GetBinary(), header->BinarySize);
            NewProgram->StoreBinary(static_cast<Device*>(device_list[i]), std::move(BinaryHolder), header->BinaryType);

            if (binary_status) *binary_status = CL_SUCCESS;
        }

        if (errcode_ret) *errcode_ret = CL_SUCCESS;
        return NewProgram.release();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clRetainProgram(cl_program program) CL_API_SUFFIX__VERSION_1_0
{
    if (!program)
    {
        return CL_INVALID_PROGRAM;
    }
    static_cast<Program*>(program)->Retain();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clReleaseProgram(cl_program program) CL_API_SUFFIX__VERSION_1_0
{
    if (!program)
    {
        return CL_INVALID_PROGRAM;
    }
    static_cast<Program*>(program)->Release();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clBuildProgram(cl_program           program_,
    cl_uint              num_devices,
    const cl_device_id* device_list,
    const char* options,
    void (CL_CALLBACK* pfn_notify)(cl_program program,
        void* user_data),
    void* user_data) CL_API_SUFFIX__VERSION_1_0
{
    if (!program_)
    {
        return CL_INVALID_PROGRAM;
    }
    Program& program = *static_cast<Program*>(program_);
    Context& context = program.GetContext();
    auto ReportError = context.GetErrorReporter();

    std::vector<Device::ref_ptr_int> device_refs;
    if (device_list)
    {
        for (cl_uint i = 0; i < num_devices; ++i)
        {
            Device* device = static_cast<Device*>(device_list[i]);
            if (!context.ValidDeviceForContext(*device))
            {
                return ReportError("Device in device_list does not belong to context.", CL_INVALID_DEVICE);
            }
            device_refs.emplace_back(device);
        }
    }
    else
    {
        device_refs = context.GetDevices();
    }

    return program.Build(std::move(device_refs), options, pfn_notify, user_data);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clCompileProgram(cl_program           program_,
    cl_uint              num_devices,
    const cl_device_id* device_list,
    const char* options,
    cl_uint              num_input_headers,
    const cl_program* input_headers,
    const char** header_include_names,
    void (CL_CALLBACK* pfn_notify)(cl_program program,
        void* user_data),
    void* user_data) CL_API_SUFFIX__VERSION_1_2
{
    if (!program_)
    {
        return CL_INVALID_PROGRAM;
    }
    Program& program = *static_cast<Program*>(program_);
    Context& context = program.GetContext();
    auto ReportError = context.GetErrorReporter();

    std::vector<Device::ref_ptr_int> device_refs;
    if (device_list)
    {
        for (cl_uint i = 0; i < num_devices; ++i)
        {
            Device* device = static_cast<Device*>(device_list[i]);
            if (!context.ValidDeviceForContext(*device))
            {
                return ReportError("Device in device_list does not belong to context.", CL_INVALID_DEVICE);
            }
            device_refs.emplace_back(device);
        }
    }
    else
    {
        device_refs = context.GetDevices();
    }

    return program.Compile(std::move(device_refs), options, num_input_headers, input_headers, header_include_names, pfn_notify, user_data);
}

extern CL_API_ENTRY cl_program CL_API_CALL
clLinkProgram(cl_context           context_,
    cl_uint              num_devices,
    const cl_device_id* device_list,
    const char* options,
    cl_uint              num_input_programs,
    const cl_program* input_programs,
    void (CL_CALLBACK* pfn_notify)(cl_program program,
        void* user_data),
    void* user_data,
    cl_int* errcode_ret) CL_API_SUFFIX__VERSION_1_2
{
    if (!context_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return nullptr;
    }
    Context& context = *static_cast<Context*>(context_);
    auto ReportError = context.GetErrorReporter(errcode_ret);

    std::vector<Device::ref_ptr_int> device_refs;
    if (device_list)
    {
        for (cl_uint i = 0; i < num_devices; ++i)
        {
            Device* device = static_cast<Device*>(device_list[i]);
            if (!context.ValidDeviceForContext(*device))
            {
                return ReportError("Device in device_list does not belong to context.", CL_INVALID_DEVICE);
            }
            device_refs.emplace_back(device);
        }
    }
    else
    {
        device_refs = context.GetDevices();
    }

    try
    {
        ref_ptr NewProgram(new Program(context, std::move(device_refs)), adopt_ref{});
        cl_int LinkStatus = NewProgram->Link(options, num_input_programs, input_programs, pfn_notify, user_data);
        if (LinkStatus != CL_SUCCESS)
        {
            NewProgram.Release();
            return ReportError("Linking failed.", CL_LINK_PROGRAM_FAILURE);
        }
        return NewProgram.Detach();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetProgramInfo(cl_program         program_,
    cl_program_info    param_name,
    size_t             param_value_size,
    void* param_value,
    size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!program_)
    {
        return CL_INVALID_PROGRAM;
    }

    Program& program = *static_cast<Program*>(program_);
    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };

    switch (param_name)
    {
    case CL_PROGRAM_REFERENCE_COUNT: return RetValue(program.GetRefCount());
    case CL_PROGRAM_CONTEXT: return RetValue((cl_context)&program.GetContext());
    case CL_PROGRAM_NUM_DEVICES: return RetValue((cl_uint)program.m_AssociatedDevices.size());
    case CL_PROGRAM_DEVICES:
        return CopyOutParameterImpl(program.m_AssociatedDevices.data(),
                                    program.m_AssociatedDevices.size() * sizeof(program.m_AssociatedDevices[0]),
                                    param_value_size, param_value, param_value_size_ret);
    case CL_PROGRAM_SOURCE: return RetValue(program.m_Source.c_str());
    case CL_PROGRAM_IL: return RetValue(nullptr);
    case CL_PROGRAM_BINARY_SIZES:
    {
        size_t OutSize = sizeof(size_t) * program.m_AssociatedDevices.size();
        if (param_value_size && param_value_size < OutSize)
        {
            return CL_INVALID_VALUE;
        }
        if (param_value_size_ret)
        {
            *param_value_size_ret = OutSize;
        }
        if (param_value_size)
        {
            std::lock_guard lock(program.m_Lock);
            size_t *Out = reinterpret_cast<size_t*>(param_value);
            for (cl_uint i = 0; i < program.m_AssociatedDevices.size(); ++i)
            {
                Out[i] = 0;
                auto& BuildData = program.m_BuildData[program.m_AssociatedDevices[i].Get()];
                if (BuildData && BuildData->m_BinaryType != CL_PROGRAM_BINARY_TYPE_NONE)
                {
                    ProgramBinaryHeader header(BuildData->m_OwnedBinary.get(), BuildData->m_BinaryType);
                    Out[i] = header.ComputeFullBlobSize();
                }
            }
        }
        return CL_SUCCESS;
    }
    case CL_PROGRAM_BINARIES:
    {
        size_t OutSize = sizeof(void*) * program.m_AssociatedDevices.size();
        if (param_value_size && param_value_size < OutSize)
        {
            return CL_INVALID_VALUE;
        }
        if (param_value_size_ret)
        {
            *param_value_size_ret = OutSize;
        }
        if (param_value_size)
        {
            std::lock_guard lock(program.m_Lock);
            void **Out = reinterpret_cast<void **>(param_value);
            for (cl_uint i = 0; i < program.m_AssociatedDevices.size(); ++i)
            {
                if (!Out[i])
                    continue;

                auto& BuildData = program.m_BuildData[program.m_AssociatedDevices[i].Get()];
                if (BuildData && BuildData->m_BinaryType != CL_PROGRAM_BINARY_TYPE_NONE)
                {
                    new (Out[i]) ProgramBinaryHeader(BuildData->m_OwnedBinary.get(), BuildData->m_BinaryType, ProgramBinaryHeader::CopyBinaryContentsTag{});
                }
            }
        }
        return CL_SUCCESS;
    }
    case CL_PROGRAM_NUM_KERNELS:
    {
        std::lock_guard lock(program.m_Lock);
        for (auto& pair : program.m_BuildData)
        {
            if (pair.second &&
                pair.second->m_BuildStatus == CL_BUILD_SUCCESS &&
                pair.second->m_BinaryType == CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
            {
                return RetValue(pair.second->m_Kernels.size());
            }
        }
        return CL_INVALID_PROGRAM_EXECUTABLE;
    }
    case CL_PROGRAM_KERNEL_NAMES:
    {
        std::lock_guard lock(program.m_Lock);
        for (auto& pair : program.m_BuildData)
        {
            if (pair.second &&
                pair.second->m_BuildStatus == CL_BUILD_SUCCESS &&
                pair.second->m_BinaryType == CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
            {
                size_t stringSize = 0;
                for (auto&& [kernelName, kernel] : pair.second->m_Kernels)
                {
                    stringSize += kernelName.size();
                }
                stringSize += pair.second->m_Kernels.size(); // 1 semicolon between each name + 1 null terminator
                if (param_value_size && param_value_size < stringSize)
                {
                    return CL_INVALID_VALUE;
                }
                if (param_value_size)
                {
                    char* pOut = reinterpret_cast<char*>(param_value);
                    for (auto&& [kernelName, kernel] : pair.second->m_Kernels)
                    {
                        pOut = std::copy(kernelName.begin(), kernelName.end(), pOut);
                        *(pOut++) = ';';
                    }
                    *(--pOut) = '\0';
                }
                if (param_value_size_ret)
                {
                    *param_value_size_ret = stringSize;
                }
                return CL_SUCCESS;
            }
        }
        return CL_INVALID_PROGRAM_EXECUTABLE;
    }
    case CL_PROGRAM_SCOPE_GLOBAL_CTORS_PRESENT: return RetValue((cl_bool)CL_FALSE);
    case CL_PROGRAM_SCOPE_GLOBAL_DTORS_PRESENT: return RetValue((cl_bool)CL_FALSE);
    }

    return program.GetContext().GetErrorReporter()("Unknown param_name", CL_INVALID_VALUE);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetProgramBuildInfo(cl_program            program_,
    cl_device_id          device,
    cl_program_build_info param_name,
    size_t                param_value_size,
    void* param_value,
    size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!program_)
    {
        return CL_INVALID_PROGRAM;
    }

    Program& program = *static_cast<Program*>(program_);
    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };

    if (std::find_if(program.m_AssociatedDevices.begin(), program.m_AssociatedDevices.end(),
                     [device](Device::ref_ptr_int const& d) {return d.Get() == device; }) == program.m_AssociatedDevices.end())
    {
        return program.GetContext().GetErrorReporter()("Invalid device.", CL_INVALID_DEVICE);
    }

    std::lock_guard lock(program.m_Lock);
    auto& BuildData = program.m_BuildData[static_cast<Device*>(device)];
    switch (param_name)
    {
    case CL_PROGRAM_BUILD_STATUS: return RetValue(BuildData ? BuildData->m_BuildStatus : CL_BUILD_NONE);
    case CL_PROGRAM_BUILD_OPTIONS: return RetValue(BuildData ? BuildData->m_LastBuildOptions.c_str() : "");
    case CL_PROGRAM_BUILD_LOG: return RetValue(BuildData ? BuildData->m_BuildLog.c_str() : "");
    case CL_PROGRAM_BINARY_TYPE: return RetValue(BuildData ? BuildData->m_BinaryType : CL_PROGRAM_BINARY_TYPE_NONE);
    case CL_PROGRAM_BUILD_GLOBAL_VARIABLE_TOTAL_SIZE: return RetValue((size_t)0);
    }

    return program.GetContext().GetErrorReporter()("Unknown param_name", CL_INVALID_VALUE);
}

Program::Program(Context& Parent, std::string Source)
    : CLChildBase(Parent)
    , m_Source(std::move(Source))
    , m_AssociatedDevices(Parent.GetDevices())
{
}

Program::Program(Context& Parent, std::vector<Device::ref_ptr_int> Devices)
    : CLChildBase(Parent)
    , m_AssociatedDevices(std::move(Devices))
{
}

cl_int Program::Build(std::vector<Device::ref_ptr_int> Devices, const char* options, Callback pfn_notify, void* user_data)
{
    auto ReportError = GetContext().GetErrorReporter();

    // Parse options
    BuildArgs Args = {};
    AddBuiltinOptions(Devices, Args.Common);
    cl_int ret = ParseOptions(options, Args.Common, true, true);
    if (ret != CL_SUCCESS)
    {
        return ReportError("Invalid options.", CL_INVALID_BUILD_OPTIONS);
    }

    {
        // Ensure that we can build
        std::lock_guard Lock(m_Lock);
        if (m_NumLiveKernels > 0)
        {
            return ReportError("Cannot compile program: program has live kernels.", CL_INVALID_OPERATION);
        }
        for (auto& device : Devices)
        {
            auto &BuildData = m_BuildData[device.Get()];
            if (!BuildData)
            {
                if (m_Source.empty())
                {
                    return ReportError("Build requested for binary program, for device with no binary.", CL_INVALID_BINARY);
                }
                continue;
            }

            if (BuildData->m_BuildStatus == CL_BUILD_IN_PROGRESS)
            {
                return ReportError("Cannot compile program: program currently being compiled.", CL_INVALID_OPERATION);
            }
            if (BuildData->m_NumPendingLinks > 0)
            {
                return ReportError("Cannot compile program: program currently being linked against.", CL_INVALID_OPERATION);
            }
            if (BuildData->m_BinaryType == CL_PROGRAM_BINARY_TYPE_COMPILED_OBJECT ||
                BuildData->m_BinaryType == CL_PROGRAM_BINARY_TYPE_LIBRARY)
            {
                return ReportError("Build requested for compiled objet or library.", CL_INVALID_BINARY);
            }
        }

        if (!m_Source.empty())
        {
            // Update build status to indicate build is starting so nobody else can start a build
            auto BuildData = std::make_shared<PerDeviceData>();
            Args.Common.BuildData = BuildData;
            BuildData->m_Device = Devices[0].Get();
            BuildData->m_LastBuildOptions = options ? options : "";
            for (auto& device : Devices)
            {
                m_BuildData[device.Get()] = BuildData;
            }
        }
        else
        {
            // Update build data state, but don't throw away existing ones
            for (auto& device : Devices)
            {
                auto& BuildData = m_BuildData[device.Get()];
                assert(BuildData && BuildData->m_OwnedBinary);
                BuildData->m_BuildStatus = CL_BUILD_IN_PROGRESS;
                BuildData->m_BuildLog.clear();
                BuildData->m_LastBuildOptions = options ? options : "";
            }
            Args.BinaryBuildDevices = std::move(Devices);
        }
    }

    if (pfn_notify)
    {
        Args.Common.pfn_notify = pfn_notify;
        Args.Common.CallbackUserData = user_data;
        g_Platform->QueueProgramOp([this, Args, selfRef = ref_ptr_int(this)]()
            {
                this->BuildImpl(Args);
            });
        return CL_SUCCESS;
    }
    else
    {
        return BuildImpl(Args);
    }
}

cl_int Program::Compile(std::vector<Device::ref_ptr_int> Devices, const char* options, cl_uint num_input_headers, const cl_program* input_headers, const char** header_include_names, Callback pfn_notify, void* user_data)
{
    auto ReportError = GetContext().GetErrorReporter();
    if (m_Source.empty())
    {
        return ReportError("Program does not contain source.", CL_INVALID_OPERATION);
    }

    // Parse options
    CompileArgs Args = {};
    AddBuiltinOptions(Devices, Args.Common);
    cl_int ret = ParseOptions(options, Args.Common, true, false);
    if (ret != CL_SUCCESS)
    {
        return ReportError("Invalid options.", CL_INVALID_COMPILER_OPTIONS);
    }

    for (cl_uint i = 0; i < num_input_headers; ++i)
    {
        if (!input_headers[i] || !header_include_names[i] || header_include_names[i][0] == '\0')
        {
            return ReportError("Invalid header or header name.", CL_INVALID_VALUE);
        }
        Program& header = *static_cast<Program*>(input_headers[i]);
        if (header.m_Source.empty())
        {
            return ReportError("Header provided has no source.", CL_INVALID_VALUE);
        }
        Args.Headers[header_include_names[i]] = &header;
    }

    {
        // Ensure that we can compile
        std::lock_guard Lock(m_Lock);
        if (m_NumLiveKernels > 0)
        {
            return ReportError("Cannot compile program: program has live kernels.", CL_INVALID_OPERATION);
        }
        for (auto& device : Devices)
        {
            auto &BuildData = m_BuildData[device.Get()];
            if (!BuildData)
                continue;

            if (BuildData->m_BuildStatus == CL_BUILD_IN_PROGRESS)
            {
                return ReportError("Cannot compile program: program currently being compiled.", CL_INVALID_OPERATION);
            }
            if (BuildData->m_NumPendingLinks > 0)
            {
                return ReportError("Cannot compile program: program currently being linked against.", CL_INVALID_OPERATION);
            }
        }

        // Update build status to indicate build is starting so nobody else can start a build
        auto BuildData = std::make_shared<PerDeviceData>();
        Args.Common.BuildData = BuildData;
        BuildData->m_Device = Devices[0].Get();
        BuildData->m_LastBuildOptions = options ? options : "";
        for (auto& device : Devices)
        {
            m_BuildData[device.Get()] = BuildData;
        }
    }

    if (pfn_notify)
    {
        Args.Common.pfn_notify = pfn_notify;
        Args.Common.CallbackUserData = user_data;
        g_Platform->QueueProgramOp([this, Args, selfRef = ref_ptr_int(this)]()
            {
                this->CompileImpl(Args);
            });
        return CL_SUCCESS;
    }
    else
    {
        return CompileImpl(Args);
    }
}

cl_int Program::Link(const char* options, cl_uint num_input_programs, const cl_program* input_programs, Callback pfn_notify, void* user_data)
{
    auto ReportError = GetContext().GetErrorReporter();

    // Parse options
    LinkArgs Args = {};
    cl_int ret = ParseOptions(options, Args.Common, false, true);
    if (ret != CL_SUCCESS)
    {
        return ReportError("Invalid options.", CL_INVALID_LINKER_OPTIONS);
    }

    for (cl_uint i = 0; i < num_input_programs; ++i)
    {
        if (!input_programs[i])
        {
            return ReportError("Invalid header or header name.", CL_INVALID_VALUE);
        }
    }

    // Validation pass
    for (auto& Device : m_AssociatedDevices)
    {
        unsigned ThisDeviceValidPrograms = 0;

        for (cl_uint i = 0; i < num_input_programs; ++i)
        {
            Program& lib = *static_cast<Program*>(input_programs[i]);

            std::lock_guard Lock(lib.m_Lock);
            auto& BuildData = lib.m_BuildData[Device.Get()];
            if (!BuildData)
            {
                if (ThisDeviceValidPrograms)
                    return ReportError("Invalid input program: no build data for one of requested devices.", CL_INVALID_OPERATION);
                continue;
            }
            if (BuildData->m_BuildStatus == CL_BUILD_IN_PROGRESS)
            {
                return ReportError("Invalid input program: program is currently being built.", CL_INVALID_OPERATION);
            }
            if (BuildData->m_BuildStatus == CL_BUILD_ERROR)
            {
                if (ThisDeviceValidPrograms)
                    return ReportError("Invalid input program: program failed to be built.", CL_INVALID_OPERATION);
            }
            if (BuildData->m_BinaryType == CL_PROGRAM_BINARY_TYPE_NONE ||
                BuildData->m_BinaryType == CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
            {
                if (ThisDeviceValidPrograms)
                    return ReportError("Invalid input program: program does not contain library or compiled object.", CL_INVALID_OPERATION);
            }
            ++ThisDeviceValidPrograms;
        }
    }

    bool AllDevicesSameProgram = true;
    for (cl_uint i = 0; i < num_input_programs; ++i)
    {
        Program& lib = *static_cast<Program*>(input_programs[i]);
        Args.LinkPrograms.emplace_back(&lib);

        std::shared_ptr<PerDeviceData> BuildData;
        std::lock_guard Lock(lib.m_Lock);
        for (auto& Device : m_AssociatedDevices)
        {
            auto& ThisDeviceBuildData = lib.m_BuildData[Device.Get()];
            if (BuildData &&
                ThisDeviceBuildData != BuildData)
            {
                AllDevicesSameProgram = false;
            }
            BuildData = ThisDeviceBuildData;
            ++ThisDeviceBuildData->m_NumPendingLinks;
        }
        if (!AllDevicesSameProgram)
        {
            break;
        }
    }

    // Note: Don't need to take our own lock, since no other thread can have access to this object
    // Update build status to indicate compile is starting so nobody else can start a build
    if (AllDevicesSameProgram)
    {
        Args.Common.BuildData = std::make_shared<PerDeviceData>();
        for (auto& Device : m_AssociatedDevices)
        {
            m_BuildData[Device.Get()] = Args.Common.BuildData;
        }
        Args.Common.BuildData->m_Device = m_AssociatedDevices[0].Get();
        Args.Common.BuildData->m_LastBuildOptions = options ? options : "";
    }
    else
    {
        for (auto& Device : m_AssociatedDevices)
        {
            auto& BuildData = m_BuildData[Device.Get()];
            BuildData = std::make_shared<PerDeviceData>();
            BuildData->m_Device = Device.Get();
            BuildData->m_LastBuildOptions = options ? options : "";
        }
    }

    if (pfn_notify)
    {
        Args.Common.pfn_notify = pfn_notify;
        Args.Common.CallbackUserData = user_data;
        g_Platform->QueueProgramOp([this, Args, selfRef = ref_ptr_int(this)]()
            {
                this->LinkImpl(Args);
            });
        return CL_SUCCESS;
    }
    else
    {
        return LinkImpl(Args);
    }
}

void Program::StoreBinary(Device *Device, unique_spirv OwnedBinary, cl_program_binary_type Type)
{
    std::lock_guard Lock(m_Lock);
    auto& BuildData = m_BuildData[Device];
    assert(!BuildData);
    BuildData = std::make_shared<PerDeviceData>();
    BuildData->m_OwnedBinary = std::move(OwnedBinary);
    BuildData->m_BinaryType = Type;
    BuildData->m_BuildStatus = CL_BUILD_NONE;
}

const clc_object* Program::GetSpirV(Device* device) const
{
    std::lock_guard Lock(m_Lock);
    return m_BuildData.find(device)->second->m_OwnedBinary.get();
}

void Program::KernelCreated()
{
    std::lock_guard lock(m_Lock);
    ++m_NumLiveKernels;
}

void Program::KernelFreed()
{
    std::lock_guard lock(m_Lock);
    --m_NumLiveKernels;
}

void Program::AddBuiltinOptions(std::vector<Device::ref_ptr_int> const& devices, CommonOptions& optionsStruct)
{
    optionsStruct.Args.reserve(15);
#ifdef CLON12_SUPPORT_3_0
    optionsStruct.Args.push_back("-D__OPENCL_VERSION__=300");
#else
    optionsStruct.Args.push_back("-D__OPENCL_VERSION__=120");
#endif
    // Disable extensions promoted to optional core features that we don't support
    optionsStruct.Args.push_back("-cl-ext=-cl_khr_fp64");
    optionsStruct.Args.push_back("-cl-ext=-cl_khr_depth_images");
    optionsStruct.Args.push_back("-cl-ext=-cl_khr_subgroups");
    // Disable optional core features that we don't support
    optionsStruct.Args.push_back("-cl-ext=-__opencl_c_fp64");
    optionsStruct.Args.push_back("-cl-ext=-__opencl_c_device_enqueue");
    optionsStruct.Args.push_back("-cl-ext=-__opencl_c_generic_address_space");
    optionsStruct.Args.push_back("-cl-ext=-__opencl_c_pipes");
    optionsStruct.Args.push_back("-cl-ext=-__opencl_c_program_scope_global_variables");
    optionsStruct.Args.push_back("-cl-ext=-__opencl_c_subgroups");
    optionsStruct.Args.push_back("-cl-ext=-__opencl_c_work_group_collective_functions");
    // Query device caps to determine additional things to enable and/or disable
    if (std::any_of(devices.begin(), devices.end(), [](Device::ref_ptr_int const& d) { return d->IsMCDM(); }))
    {
        optionsStruct.Args.push_back("-U__IMAGE_SUPPORT__");
        optionsStruct.Args.push_back("-cl-ext=-__opencl_c_images");
        optionsStruct.Args.push_back("-cl-ext=-__opencl_c_read_write_images");
    }
    else if (!std::all_of(devices.begin(), devices.end(), [](Device::ref_ptr_int const& d) { return d->SupportsTypedUAVLoad(); }))
    {
        optionsStruct.Args.push_back("-cl-ext=-__opencl_c_read_write_images");
    }
}

cl_int Program::ParseOptions(const char* optionsStr, CommonOptions& optionsStruct, bool SupportCompilerOptions, bool SupportLinkerOptions)
{
    using namespace std::string_view_literals;

    std::string curOption;
    auto ValidateAndPushArg = [&]()
    {
        if (curOption.empty())
        {
            return CL_SUCCESS;
        }
        if (curOption[0] != '-' && curOption[0] != '/')
        {
            if (!optionsStruct.Args.empty() &&
                (optionsStruct.Args.back() == "-D"sv ||
                    optionsStruct.Args.back() == "-I"sv))
            {
                optionsStruct.Args.push_back(curOption);
                return CL_SUCCESS;
            }
            else
            {
                return CL_INVALID_BUILD_OPTIONS;
            }
        }
        curOption[0] = '-';
        if (SupportCompilerOptions)
        {
            if (curOption[1] == 'D' ||
                curOption[1] == 'I' ||
                curOption == "-cl-single-precision-constant"sv ||
                // Note: Not valid because we don't claim support for this feature
                //curOption == "cl-fp32-correctly-rounded-divide-sqrt"sv ||
                curOption == "-cl-opt-disable"sv ||
                curOption == "-Werror"sv ||
                curOption[1] == 'w' ||
                curOption.find("-cl-std=") == 0 ||
                curOption == "-cl-kernel-arg-info"sv)
            {
                optionsStruct.Args.push_back(curOption);
                return CL_SUCCESS;
            }
        }
        if (SupportLinkerOptions)
        {
            if (curOption == "-create-library"sv)
            {
                optionsStruct.CreateLibrary = true;
                return CL_SUCCESS;
            }
            else if (curOption == "-enable-link-options")
            {
                optionsStruct.EnableLinkOptions = true;
                return CL_SUCCESS;
            }
        }
        if (curOption == "-cl-denorms-are-zero"sv ||
            curOption == "-cl-no-signed-zeros"sv ||
            curOption == "-cl-unsafe-math-optimizations"sv ||
            curOption == "-cl-finite-math-only"sv ||
            curOption == "-cl-fast-relaxed-math"sv ||
            curOption == "-cl-mad-enable"sv)
        {
            optionsStruct.Args.push_back(curOption);
            return CL_SUCCESS;
        }
        return CL_INVALID_BUILD_OPTIONS;
    };

    bool inQuotes = false;
    while (optionsStr && *optionsStr)
    {
        switch (*optionsStr)
        {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            if (!inQuotes)
            {
                cl_int retVal = ValidateAndPushArg();
                curOption.clear();
                if (retVal != CL_SUCCESS)
                {
                    return retVal;
                }
                break;
            }
            // Fallthrough
        default:
            curOption.push_back(*optionsStr);
            break;
        case '"':
            inQuotes = !inQuotes;
            break;
        case '\\':
            if (!inQuotes)
            {
                curOption.push_back(*optionsStr);
            }
            else
            {
                switch (*(optionsStr + 1))
                {
                case '\\':
                case '"':
                    ++optionsStr;
                default:
                    curOption.push_back(*optionsStr);
                    break;
                }
            }
            break;
        }
        ++optionsStr;
    }
    return ValidateAndPushArg();
}

struct Loggers
{
    Program& m_Program;
    Program::PerDeviceData& m_BuildData;
    Loggers(Program& program, Program::PerDeviceData& buildData);
    void Log(const char* msg);
    static void Log(void* context, const char* msg);
    operator clc_logger();
};

Loggers::Loggers(Program& program, Program::PerDeviceData& buildData)
    : m_Program(program)
    , m_BuildData(buildData)
{
}

void Loggers::Log(const char* message)
{
    std::lock_guard Lock(m_Program.m_Lock);
    m_BuildData.m_BuildLog += message;
}

void Loggers::Log(void* context, const char* message)
{
    static_cast<Loggers*>(context)->Log(message);
}

Loggers::operator clc_logger()
{
    clc_logger ret;
    ret.priv = this;
    ret.error = Log;
    ret.warning = Log;
    return ret;
}

cl_int Program::BuildImpl(BuildArgs const& Args)
{
    cl_int ret = CL_SUCCESS;
    auto& Compiler = g_Platform->GetCompiler();
    auto link = Compiler.proc_address<decltype(&clc_link)>("clc_link");
    auto free = Compiler.proc_address<decltype(&clc_free_object)>("clc_free_object");
    if (!m_Source.empty())
    {
        auto& BuildData = Args.Common.BuildData;

        auto Context = g_Platform->GetCompilerContext(BuildData->m_Device->GetShaderCache());

        Loggers loggers(*this, *BuildData);
        clc_logger loggers_impl = loggers;
        auto compile = Compiler.proc_address<decltype(&clc_compile)>("clc_compile");
        clc_compile_args args = {};

        std::vector<const char*> raw_args;
        raw_args.reserve(Args.Common.Args.size());
        for (auto& def : Args.Common.Args)
        {
            raw_args.push_back(def.c_str());
        }

        args.args = raw_args.data();
        args.num_args = (unsigned)raw_args.size();
        args.source = { "source.cl", m_Source.c_str() };

        unique_spirv compiledObject(compile(Context, &args, &loggers_impl), free);
        const clc_object* rawCompiledObject = compiledObject.get();

        clc_linker_args link_args = {};
        link_args.create_library = Args.Common.CreateLibrary;
        link_args.in_objs = &rawCompiledObject;
        link_args.num_in_objs = 1;
        unique_spirv object(rawCompiledObject ? link(Context, &link_args, &loggers_impl) : nullptr, free);
        BuildData->m_OwnedBinary = std::move(object);

        std::lock_guard Lock(m_Lock);
        if (BuildData->m_OwnedBinary)
        {
            BuildData->m_BinaryType = CL_PROGRAM_BINARY_TYPE_EXECUTABLE;
            BuildData->m_BuildStatus = CL_BUILD_SUCCESS;
            BuildData->CreateKernels(*this);
        }
        else
        {
            ret = CL_BUILD_PROGRAM_FAILURE;
            BuildData->m_BuildStatus = CL_BUILD_ERROR;
        }
    }
    else
    {
        std::lock_guard Lock(m_Lock);
        clc_context* Context = nullptr;
        for (auto& device : Args.BinaryBuildDevices)
        {
            auto& BuildData = m_BuildData[device.Get()];
            if (!Context)
            {
                Context = g_Platform->GetCompilerContext(device->GetShaderCache());
            }
            Loggers loggers(*this, *BuildData);
            clc_logger loggers_impl = loggers;
            const clc_object* rawCompiledObject = BuildData->m_OwnedBinary.get();

            clc_linker_args link_args = {};
            link_args.create_library = Args.Common.CreateLibrary;
            link_args.in_objs = &rawCompiledObject;
            link_args.num_in_objs = 1;
            unique_spirv object(rawCompiledObject ? link(Context, &link_args, &loggers_impl) : nullptr, free);
            BuildData->m_OwnedBinary = std::move(object);

            if (BuildData->m_OwnedBinary)
            {
                BuildData->m_BinaryType = CL_PROGRAM_BINARY_TYPE_EXECUTABLE;
                BuildData->m_BuildStatus = CL_BUILD_SUCCESS;
                BuildData->CreateKernels(*this);
            }
            else
            {
                ret = CL_BUILD_PROGRAM_FAILURE;
                BuildData->m_BuildStatus = CL_BUILD_ERROR;
            }
        }
    }
    if (Args.Common.pfn_notify)
    {
        Args.Common.pfn_notify(this, Args.Common.CallbackUserData);
    }
    return ret;
}

cl_int Program::CompileImpl(CompileArgs const& Args)
{
    cl_int ret = CL_SUCCESS;
    auto& BuildData = Args.Common.BuildData;
    auto& Compiler = g_Platform->GetCompiler();
    auto Context = g_Platform->GetCompilerContext(BuildData->m_Device->GetShaderCache());
    auto compile = Compiler.proc_address<decltype(&clc_compile)>("clc_compile");
    auto free = Compiler.proc_address<decltype(&clc_free_object)>("clc_free_object");
    Loggers loggers(*this, *BuildData);
    clc_logger loggers_impl = loggers;
    clc_compile_args args = {};

    std::vector<const char*> raw_args;
    raw_args.reserve(Args.Common.Args.size());
    for (auto& def : Args.Common.Args)
    {
        raw_args.push_back(def.c_str());
    }
    std::vector<clc_named_value> headers;
    headers.reserve(Args.Headers.size());
    for (auto& h : Args.Headers)
    {
        headers.push_back(clc_named_value{ h.first.c_str(), h.second->m_Source.c_str() });
    }

    args.headers = headers.data();
    args.num_headers = (unsigned)headers.size();
    args.args = raw_args.data();
    args.num_args = (unsigned)raw_args.size();
    args.source = { "source.cl", m_Source.c_str() };

    unique_spirv object(compile(Context, &args, &loggers_impl), free);

    {
        std::lock_guard Lock(m_Lock);
        if (object)
        {
            BuildData->m_OwnedBinary = std::move(object);
            BuildData->m_BinaryType = CL_PROGRAM_BINARY_TYPE_COMPILED_OBJECT;
            BuildData->m_BuildStatus = CL_BUILD_SUCCESS;
        }
        else
        {
            ret = CL_COMPILE_PROGRAM_FAILURE;
            BuildData->m_BuildStatus = CL_BUILD_ERROR;
        }
    }
    if (Args.Common.pfn_notify)
    {
        Args.Common.pfn_notify(this, Args.Common.CallbackUserData);
    }
    return ret;
}

cl_int Program::LinkImpl(LinkArgs const& Args)
{
    cl_int ret = CL_SUCCESS;
    auto& Compiler = g_Platform->GetCompiler();
    clc_context* Context = nullptr;
    auto link = Compiler.proc_address<decltype(&clc_link)>("clc_link");
    auto free = Compiler.proc_address<decltype(&clc_free_object)>("clc_free_object");
    std::vector<const clc_object*> objects;
    objects.reserve(Args.LinkPrograms.size());

    clc_linker_args link_args = {};
    link_args.create_library = Args.Common.CreateLibrary;

    for (auto& Device : m_AssociatedDevices)
    {
        if (!Context)
        {
            Context = g_Platform->GetCompilerContext(Device->GetShaderCache());
        }

        objects.clear();
        for (cl_uint i = 0; i < Args.LinkPrograms.size(); ++i)
        {
            std::lock_guard Lock(Args.LinkPrograms[i]->m_Lock);
            auto& BuildData = Args.LinkPrograms[i]->m_BuildData[Device.Get()];
            if (BuildData)
                objects.push_back(BuildData->m_OwnedBinary.get());
        }

        {
            std::lock_guard Lock(m_Lock);
            auto& BuildData = m_BuildData[Device.Get()];
            if (BuildData->m_BuildStatus == CL_BUILD_IN_PROGRESS)
            {
                Loggers loggers(*this, *BuildData);
                clc_logger loggers_impl = loggers;

                link_args.in_objs = objects.data();
                link_args.num_in_objs = (unsigned)objects.size();
                unique_spirv linkedObject(link(Context, &link_args, &loggers_impl), free);

                if (linkedObject)
                {
                    BuildData->m_OwnedBinary = std::move(linkedObject);
                    BuildData->m_BinaryType = Args.Common.CreateLibrary ?
                        CL_PROGRAM_BINARY_TYPE_LIBRARY : CL_PROGRAM_BINARY_TYPE_EXECUTABLE;
                    BuildData->m_BuildStatus = CL_BUILD_SUCCESS;
                    BuildData->CreateKernels(*this);
                }
                else
                {
                    ret = CL_LINK_PROGRAM_FAILURE;
                    BuildData->m_BuildStatus = CL_BUILD_ERROR;
                }
            }
        }

        for (cl_uint i = 0; i < Args.LinkPrograms.size(); ++i)
        {
            std::lock_guard Lock(Args.LinkPrograms[i]->m_Lock);
            auto& BuildData = Args.LinkPrograms[i]->m_BuildData[Device.Get()];
            if (BuildData)
                --BuildData->m_NumPendingLinks;
        }
    }
    if (Args.Common.pfn_notify)
    {
        Args.Common.pfn_notify(this, Args.Common.CallbackUserData);
    }
    return ret;
}

void Program::PerDeviceData::CreateKernels(Program& program)
{
    if (m_BinaryType != CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
        return;

    auto& Compiler = g_Platform->GetCompiler();
    auto Context = g_Platform->GetCompilerContext(m_Device->GetShaderCache());
    auto get_kernel = Compiler.proc_address<decltype(&clc_to_dxil)>("clc_to_dxil");
    auto free = Compiler.proc_address<decltype(&clc_free_dxil_object)>("clc_free_dxil_object");

    for (auto kernelMeta = m_OwnedBinary->kernels; kernelMeta != m_OwnedBinary->kernels + m_OwnedBinary->num_kernels; ++kernelMeta)
    {
        auto name = kernelMeta->name;
        auto& kernel = m_Kernels.emplace(name, unique_dxil(nullptr, free)).first->second;
        Loggers loggers(program, *this);
        clc_logger loggers_impl = loggers;
        kernel.m_GenericDxil.reset(get_kernel(Context, m_OwnedBinary.get(), name, nullptr /*configuration*/, &loggers_impl));
        if (kernel.m_GenericDxil)
            SignBlob(kernel.m_GenericDxil->binary.data, kernel.m_GenericDxil->binary.size);
    }
}
