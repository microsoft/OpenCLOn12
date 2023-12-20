// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "program.hpp"
#include "compiler.hpp"
#include "kernel.hpp"

#include <algorithm>

#include "spookyv2.h"

struct ProgramBinaryHeader
{
    static constexpr GUID c_ValidHeaderGuid = { /* 8d46c01e-2977-4234-a5b0-292405fc1d34 */
        0x8d46c01e, 0x2977, 0x4234, {0xa5, 0xb0, 0x29, 0x24, 0x05, 0xfc, 0x1d, 0x34} };
    const GUID HeaderGuid = c_ValidHeaderGuid;
    cl_program_binary_type BinaryType = CL_PROGRAM_BINARY_TYPE_NONE;
    uint32_t BinarySize = 0;

    ProgramBinaryHeader() = default;
    ProgramBinaryHeader(const ProgramBinary* obj, cl_program_binary_type type)
        : BinaryType(type)
        , BinarySize((uint32_t)obj->GetBinarySize())
    {
    }
    struct CopyBinaryContentsTag {};
    ProgramBinaryHeader(const ProgramBinary* obj, cl_program_binary_type type, CopyBinaryContentsTag)
        : ProgramBinaryHeader(obj, type)
    {
        memcpy(GetBinary(), obj->GetBinary(), BinarySize);
    }

    size_t ComputeFullBlobSize() const
    {
        return sizeof(*this) + BinarySize;
    }
    void* GetBinary() { return this + 1; }
    const void* GetBinary() const { return this + 1; }
};

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

        std::vector<D3DDeviceAndRef> device_refs;
        for (cl_uint i = 0; i < num_devices; ++i)
        {
            Device* device = static_cast<Device*>(device_list[i]);
            D3DDevice *d3dDevice = context.D3DDeviceForContext(*device);
            if (!d3dDevice)
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
            device_refs.emplace_back(std::make_pair(device, d3dDevice));
        }
        if (ReturnError)
        {
            return nullptr;
        }

        std::unique_ptr<Program> NewProgram(new Program(context, device_refs));

        for (cl_uint i = 0; i < num_devices; ++i)
        {
            auto header = reinterpret_cast<ProgramBinaryHeader const*>(binaries[i]);
            std::shared_ptr<ProgramBinary> BinaryHolder = g_Platform->GetCompiler()->Load(header->GetBinary(), header->BinarySize);
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

extern CL_API_ENTRY cl_program CL_API_CALL
clCreateProgramWithIL(cl_context    context_,
    const void*    il,
    size_t         length,
    cl_int*        errcode_ret) CL_API_SUFFIX__VERSION_2_1
{
    if (!context_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return nullptr;
    }
    Context& context = *static_cast<Context*>(context_);
    auto ReportError = context.GetErrorReporter(errcode_ret);
    if (!il || !length)
    {
        return ReportError("IL must not be null and length must be nonzero", CL_INVALID_VALUE);
    }

    if (length < 4)
    {
        return ReportError("SPIR-V IL must contain greater than 4 bytes", CL_INVALID_VALUE);
    }
    uint32_t magic = *static_cast<const uint32_t*>(il);
    if (magic != 0x07230203)
    {
        return ReportError("IL does not represent valid SPIR-V", CL_INVALID_VALUE);
    }

    try
    {
        auto pCompiler = g_Platform->GetCompiler();
        std::shared_ptr<ProgramBinary> parsedProgram = pCompiler->Load(il, length);
        if (!parsedProgram || !parsedProgram->Parse(nullptr))
            return ReportError("Failed to parse SPIR-V", CL_INVALID_VALUE);

        if (errcode_ret) *errcode_ret = CL_SUCCESS;
        return new Program(context, std::move(parsedProgram));
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}
extern CL_API_ENTRY cl_program CL_API_CALL
clCreateProgramWithILKHR(cl_context    context,
                         const void*   il,
                         size_t        length,
                         cl_int*       errcode_ret)
{
    return clCreateProgramWithIL(context, il, length, errcode_ret);
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

    std::vector<D3DDeviceAndRef> device_refs;
    if (device_list)
    {
        for (cl_uint i = 0; i < num_devices; ++i)
        {
            Device* device = static_cast<Device*>(device_list[i]);
            D3DDevice *d3dDevice = context.D3DDeviceForContext(*device);
            if (!d3dDevice)
            {
                return ReportError("Device in device_list does not belong to context.", CL_INVALID_DEVICE);
            }
            device_refs.emplace_back(std::make_pair(device, d3dDevice));
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

    std::vector<D3DDeviceAndRef> device_refs;
    if (device_list)
    {
        for (cl_uint i = 0; i < num_devices; ++i)
        {
            Device* device = static_cast<Device*>(device_list[i]);
            D3DDevice *d3dDevice = context.D3DDeviceForContext(*device);
            if (!d3dDevice)
            {
                return ReportError("Device in device_list does not belong to context.", CL_INVALID_DEVICE);
            }
            device_refs.emplace_back(std::make_pair(device, d3dDevice));
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

    std::vector<D3DDeviceAndRef> device_refs;
    if (device_list)
    {
        for (cl_uint i = 0; i < num_devices; ++i)
        {
            Device* device = static_cast<Device*>(device_list[i]);
            D3DDevice *d3dDevice = context.D3DDeviceForContext(*device);
            if (!d3dDevice)
            {
                return ReportError("Device in device_list does not belong to context.", CL_INVALID_DEVICE);
            }
            device_refs.emplace_back(std::make_pair(device, d3dDevice));
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
    {
        size_t expectedSize = program.m_AssociatedDevices.size() * sizeof(cl_device_id);
        if (param_value_size && param_value_size < expectedSize)
        {
            return CL_INVALID_VALUE;
        }
        if (param_value_size)
        {
            std::transform(program.m_AssociatedDevices.begin(),
                           program.m_AssociatedDevices.end(),
                           static_cast<cl_device_id *>(param_value),
                           [](D3DDeviceAndRef const &dev) { return dev.first.Get(); });
        }
        if (param_value_size_ret)
        {
            *param_value_size_ret = expectedSize;
        }
        return CL_SUCCESS;
    }
    case CL_PROGRAM_SOURCE: return RetValue(program.m_Source.c_str());
    case CL_PROGRAM_IL:
        if (program.m_ParsedIL)
            return CopyOutParameterImpl(program.m_ParsedIL->GetBinary(), program.m_ParsedIL->GetBinarySize(),
                                        param_value_size, param_value, param_value_size_ret);
        else
            return CopyOutParameter(nullptr, param_value_size, param_value, param_value_size_ret);
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
                auto& BuildData = program.m_BuildData[program.m_AssociatedDevices[i].first.Get()];
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

                auto& BuildData = program.m_BuildData[program.m_AssociatedDevices[i].first.Get()];
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
                     [device](D3DDeviceAndRef const& d) {return d.first.Get() == device; }) == program.m_AssociatedDevices.end())
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

extern CL_API_ENTRY cl_int CL_API_CALL
clSetProgramSpecializationConstant(cl_program  program_,
    cl_uint     spec_id,
    size_t      spec_size,
    const void* spec_value) CL_API_SUFFIX__VERSION_2_2
{
    if (!program_)
    {
        return CL_INVALID_PROGRAM;
    }
    Program& program = *static_cast<Program*>(program_);
    auto ReportError = program.m_Parent->GetErrorReporter();

    if (!program.m_ParsedIL)
    {
        return ReportError("Program does not have SPIR-V IL", CL_INVALID_PROGRAM);
    }

    auto specConstInfo = program.m_ParsedIL->GetSpecConstantInfo(spec_id);
    if (!specConstInfo)
    {
        return ReportError("Invalid specialization constant ID", CL_INVALID_SPEC_ID);
    }

    if (!spec_value)
    {
        return ReportError("Specialization constant value is null", CL_INVALID_VALUE);
    }
    if (spec_size != specConstInfo->value_size)
    {
        return ReportError("Specialization constant size does not match required size", CL_INVALID_VALUE);
    }

    program.SetSpecConstant(spec_id, spec_size, spec_value);
    return CL_SUCCESS;
}

Program::Program(Context& Parent, std::string Source)
    : CLChildBase(Parent)
    , m_Source(std::move(Source))
    , m_AssociatedDevices(Parent.GetDevices())
{
}

Program::Program(Context& Parent, std::shared_ptr<ProgramBinary> ParsedIL)
    : CLChildBase(Parent)
    , m_ParsedIL(std::move(ParsedIL))
    , m_AssociatedDevices(Parent.GetDevices())
{
}

Program::Program(Context& Parent, std::vector<D3DDeviceAndRef> Devices)
    : CLChildBase(Parent)
    , m_AssociatedDevices(std::move(Devices))
{
}

cl_int Program::Build(std::vector<D3DDeviceAndRef> Devices, const char* options, Callback pfn_notify, void* user_data)
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
            auto &BuildData = m_BuildData[device.first.Get()];
            if (!BuildData)
            {
                if (m_Source.empty() && !m_ParsedIL)
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

        if (!m_Source.empty() || m_ParsedIL)
        {
            // Update build status to indicate build is starting so nobody else can start a build
            auto BuildData = std::make_shared<PerDeviceData>();
            Args.Common.BuildData = BuildData;
            BuildData->m_Device = Devices[0].first.Get();
            BuildData->m_D3DDevice = Devices[0].second;
            BuildData->m_LastBuildOptions = options ? options : "";
            for (auto& [device, _] : Devices)
            {
                m_BuildData[device.Get()] = BuildData;
            }
        }
        else
        {
            // Update build data state, but don't throw away existing ones
            for (auto& [device, _] : Devices)
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

cl_int Program::Compile(std::vector<D3DDeviceAndRef> Devices, const char* options, cl_uint num_input_headers, const cl_program* input_headers, const char** header_include_names, Callback pfn_notify, void* user_data)
{
    auto ReportError = GetContext().GetErrorReporter();
    if (m_Source.empty() && !m_ParsedIL)
    {
        return ReportError("Program does not contain source or IL.", CL_INVALID_OPERATION);
    }

    // Parse options
    CompileArgs Args = {};
    AddBuiltinOptions(Devices, Args.Common);
    cl_int ret = ParseOptions(options, Args.Common, true, false);
    if (ret != CL_SUCCESS)
    {
        return ReportError("Invalid options.", CL_INVALID_COMPILER_OPTIONS);
    }

    // "If program was created using clCreateProgramWithIL, then num_input_headers, input_headers, and header_include_names are ignored."
    if (!m_ParsedIL)
    {
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
    }

    {
        // Ensure that we can compile
        std::lock_guard Lock(m_Lock);
        if (m_NumLiveKernels > 0)
        {
            return ReportError("Cannot compile program: program has live kernels.", CL_INVALID_OPERATION);
        }
        for (auto& [device, _] : Devices)
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
        BuildData->m_Device = Devices[0].first.Get();
        BuildData->m_D3DDevice = Devices[0].second;
        BuildData->m_LastBuildOptions = options ? options : "";
        for (auto& [device, _] : Devices)
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
    for (auto& [Device, _] : m_AssociatedDevices)
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
        for (auto& [Device, _] : m_AssociatedDevices)
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
        for (auto& [Device, _] : m_AssociatedDevices)
        {
            m_BuildData[Device.Get()] = Args.Common.BuildData;
        }
        Args.Common.BuildData->m_Device = m_AssociatedDevices[0].first.Get();
        Args.Common.BuildData->m_D3DDevice = m_AssociatedDevices[0].second;
        Args.Common.BuildData->m_LastBuildOptions = options ? options : "";
    }
    else
    {
        for (auto& [Device, D3DDevice] : m_AssociatedDevices)
        {
            auto& BuildData = m_BuildData[Device.Get()];
            BuildData = std::make_shared<PerDeviceData>();
            BuildData->m_Device = Device.Get();
            BuildData->m_D3DDevice = D3DDevice;
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

void Program::StoreBinary(Device *Device, std::shared_ptr<ProgramBinary> OwnedBinary, cl_program_binary_type Type)
{
    std::lock_guard Lock(m_Lock);
    auto& BuildData = m_BuildData[Device];
    assert(!BuildData);
    BuildData = std::make_shared<PerDeviceData>();
    BuildData->m_Device = Device;
    BuildData->m_OwnedBinary = std::move(OwnedBinary);
    BuildData->m_BinaryType = Type;
    BuildData->m_BuildStatus = CL_BUILD_NONE;
}

const ProgramBinary *Program::GetSpirV(Device* device) const
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

void Program::AddBuiltinOptions(std::vector<D3DDeviceAndRef> const& devices, CommonOptions& optionsStruct)
{
    optionsStruct.Args.reserve(15);
#ifdef CLON12_SUPPORT_3_0
    optionsStruct.Args.push_back("-D__OPENCL_VERSION__=300");
#else
    optionsStruct.Args.push_back("-D__OPENCL_VERSION__=120");
#endif
    if (!optionsStruct.Features.fp64)
    {
        optionsStruct.Args.push_back("-cl-single-precision-constant");
    }
    optionsStruct.Features.int64 = true;
    // Query device caps to determine additional things to enable and/or disable
    if (std::all_of(devices.begin(), devices.end(), [](D3DDeviceAndRef const& d) { return !d.first->IsMCDM(); }))
    {
        optionsStruct.Features.images = true;
        optionsStruct.Features.images_write_3d = true;
        if (std::all_of(devices.begin(), devices.end(), [](D3DDeviceAndRef const &d) { return d.first->SupportsTypedUAVLoad(); }))
        {
            optionsStruct.Features.images_read_write = true;
        }
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
        if (curOption == "-cl-no-signed-zeros"sv ||
            curOption == "-cl-unsafe-math-optimizations"sv ||
            curOption == "-cl-finite-math-only"sv ||
            curOption == "-cl-fast-relaxed-math"sv ||
            curOption == "-cl-mad-enable"sv)
        {
            optionsStruct.Args.push_back(curOption);
            return CL_SUCCESS;
        }
        else if (curOption == "-cl-denorms-are-zero"sv)
        {
            // Hide from Clang, it doesn't have such a flag
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

cl_int Program::BuildImpl(BuildArgs const& Args)
{
    cl_int ret = CL_SUCCESS;
    auto pCompiler = g_Platform->GetCompiler();
    if (!m_Source.empty() || m_ParsedIL)
    {
        auto& BuildData = Args.Common.BuildData;
        auto &Cache = BuildData->m_D3DDevice->GetShaderCache();
        pCompiler->Initialize(Cache);

        Logger loggers(m_Lock, BuildData->m_BuildLog);
        std::shared_ptr<ProgramBinary> compiledObject;

        if (!m_Source.empty())
        {
            if (Cache.HasCache())
            {
                SpookyHash hasher;
                hasher.Init(BuildData->m_Hash[0], BuildData->m_Hash[1]);
                hasher.Update(m_Source.c_str(), m_Source.size());
                hasher.Update(&Args.Common.Features, sizeof(Args.Common.Features));
                for (auto &def : Args.Common.Args)
                {
                    hasher.Update(def.c_str(), def.size());
                }
                hasher.Final(&BuildData->m_Hash[0], &BuildData->m_Hash[1]);

                auto Precompiled = Cache.Find(BuildData->m_Hash, sizeof(BuildData->m_Hash));
                if (Precompiled.first)
                {
                    compiledObject = pCompiler->Load(Precompiled.first.get(), Precompiled.second);
                }
            }

            if (!compiledObject)
            {
                Compiler::CompileArgs args = {};
                args.program_source = m_Source.c_str();
                args.cmdline_args.reserve(Args.Common.Args.size());
                args.features = Args.Common.Features;
                for (auto &def : Args.Common.Args)
                {
                    args.cmdline_args.push_back(def.c_str());
                }

                compiledObject = pCompiler->Compile(args, loggers);
                Cache.Store(BuildData->m_Hash, sizeof(BuildData->m_Hash), compiledObject->GetBinary(), compiledObject->GetBinarySize());
            }
        }
        else
        {
            std::lock_guard Lock(m_Lock);
            if (m_SpecConstants.size())
            {
                compiledObject = pCompiler->Specialize(*m_ParsedIL, m_SpecConstants, loggers);
            }
            else
            {
                compiledObject = m_ParsedIL;
            }
            if (Cache.HasCache())
            {
                SpookyHash::Hash128(compiledObject->GetBinary(), compiledObject->GetBinarySize(), &BuildData->m_Hash[0], &BuildData->m_Hash[1]);
            }
        }

        if (compiledObject)
        {
            Compiler::LinkerArgs link_args = {};
            link_args.create_library = Args.Common.CreateLibrary;
            link_args.objs.push_back(compiledObject.get());
            auto linkedObject = pCompiler->Link(link_args, loggers);
            BuildData->m_OwnedBinary = std::move(linkedObject);
        }

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
        pCompiler->Initialize(Args.BinaryBuildDevices[0].second->GetShaderCache());

        std::lock_guard Lock(m_Lock);
        for (auto& [device, d3dDevice] : Args.BinaryBuildDevices)
        {
            auto& BuildData = m_BuildData[device.Get()];
            Logger loggers(m_Lock, BuildData->m_BuildLog);

            if (d3dDevice->GetShaderCache().HasCache())
            {
                SpookyHash::Hash128(BuildData->m_OwnedBinary->GetBinary(), BuildData->m_OwnedBinary->GetBinarySize(),
                                    &BuildData->m_Hash[0], &BuildData->m_Hash[1]);
            }

            Compiler::LinkerArgs link_args = {};
            link_args.create_library = Args.Common.CreateLibrary;
            link_args.objs.push_back(BuildData->m_OwnedBinary.get());
            auto linkedObject = pCompiler->Link(link_args, loggers);
            BuildData->m_OwnedBinary = std::move(linkedObject);

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
    auto pCompiler = g_Platform->GetCompiler();
    auto &Cache = BuildData->m_D3DDevice->GetShaderCache();
    pCompiler->Initialize(Cache);
    Logger loggers(m_Lock, BuildData->m_BuildLog);

    std::shared_ptr<ProgramBinary> object;

    if (!m_Source.empty())
    {
        if (Cache.HasCache())
        {
            SpookyHash hasher;
            hasher.Init(BuildData->m_Hash[0], BuildData->m_Hash[1]);
            hasher.Update(m_Source.c_str(), m_Source.size());
            hasher.Update(&Args.Common.Features, sizeof(Args.Common.Features));
            for (auto &def : Args.Common.Args)
            {
                hasher.Update(def.c_str(), def.size());
            }
            for (auto &header : Args.Headers)
            {
                hasher.Update(header.first.c_str(), header.first.size());
                hasher.Update(header.second->m_Source.c_str(), header.second->m_Source.size());
            }
            hasher.Final(&BuildData->m_Hash[0], &BuildData->m_Hash[1]);

            auto Precompiled = Cache.Find(BuildData->m_Hash, sizeof(BuildData->m_Hash));
            if (Precompiled.first)
            {
                object = pCompiler->Load(Precompiled.first.get(), Precompiled.second);
            }
        }

        if (!object)
        {
            Compiler::CompileArgs args = {};
            args.cmdline_args.reserve(Args.Common.Args.size());
            for (auto &def : Args.Common.Args)
            {
                args.cmdline_args.push_back(def.c_str());
            }
            args.headers.reserve(Args.Headers.size());
            for (auto &h : Args.Headers)
            {
                args.headers.push_back({ h.first.c_str(), h.second->m_Source.c_str() });
            }
            args.program_source = m_Source.c_str();
            args.features = Args.Common.Features;

            object = pCompiler->Compile(args, loggers);
            Cache.Store(BuildData->m_Hash, sizeof(BuildData->m_Hash), object->GetBinary(), object->GetBinarySize());
        }
    }
    else
    {
        std::lock_guard Lock(m_Lock);
        if (m_SpecConstants.size())
        {
            object = pCompiler->Specialize(*m_ParsedIL, m_SpecConstants, loggers);
        }
        else
        {
            object = m_ParsedIL;
        }
        if (BuildData->m_D3DDevice->GetShaderCache().HasCache())
        {
            SpookyHash::Hash128(object->GetBinary(), object->GetBinarySize(), &BuildData->m_Hash[0], &BuildData->m_Hash[1]);
        }
    }

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
    auto pCompiler = g_Platform->GetCompiler();

    Compiler::LinkerArgs link_args = {};
    link_args.objs.reserve(Args.LinkPrograms.size());
    link_args.create_library = Args.Common.CreateLibrary;

    for (auto& [Device, D3DDevice] : m_AssociatedDevices)
    {
        auto &Cache = D3DDevice->GetShaderCache();
        pCompiler->Initialize(Cache);
        SpookyHash hasher;
        uint64_t singleHash[2] = {};
        if (Cache.HasCache() && Args.LinkPrograms.size() > 1)
        {
            hasher.Init(0, 0);
        }

        link_args.objs.clear();
        for (cl_uint i = 0; i < Args.LinkPrograms.size(); ++i)
        {
            std::lock_guard Lock(Args.LinkPrograms[i]->m_Lock);
            auto& BuildData = Args.LinkPrograms[i]->m_BuildData[Device.Get()];
            if (BuildData)
            {
                link_args.objs.push_back(BuildData->m_OwnedBinary.get());
                if (Cache.HasCache())
                {
                    memcpy(singleHash, BuildData->m_Hash, sizeof(singleHash));
                    hasher.Update(singleHash, sizeof(singleHash));
                }
            }
        }

        {
            std::lock_guard Lock(m_Lock);
            auto& BuildData = m_BuildData[Device.Get()];
            if (BuildData->m_BuildStatus == CL_BUILD_IN_PROGRESS)
            {
                Logger loggers(m_Lock, BuildData->m_BuildLog);
                std::shared_ptr<ProgramBinary> linkedObject = pCompiler->Link(link_args, loggers);

                if (linkedObject)
                {
                    memcpy(BuildData->m_Hash, singleHash, sizeof(singleHash));
                    if (Cache.HasCache() && Args.LinkPrograms.size() > 1)
                    {
                        hasher.Final(&BuildData->m_Hash[0], &BuildData->m_Hash[1]);
                    }
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

    auto pCompiler = g_Platform->GetCompiler();
    pCompiler->Initialize(m_D3DDevice->GetShaderCache());

    auto& kernels = m_OwnedBinary->GetKernelInfo();
    Logger loggers(program.m_Lock, m_BuildLog);
    for (auto& kernelMeta : kernels)
    {
        auto name = kernelMeta.name;
        auto& kernel = m_Kernels.emplace(std::piecewise_construct,
                                         std::forward_as_tuple(name),
                                         std::forward_as_tuple(kernelMeta, unique_dxil{})).first->second;
        kernel.m_GenericDxil = pCompiler->GetKernel(name, *m_OwnedBinary, nullptr /*configuration*/, &loggers);
        if (kernel.m_GenericDxil)
            kernel.m_GenericDxil->Sign();
    }
}

void Program::SetSpecConstant(cl_uint ID, size_t size, const void *value)
{
    std::lock_guard lock(m_Lock);

    auto& rawValue = m_SpecConstants[ID].value;
    assert(size <= sizeof(rawValue));
    memcpy(rawValue, value, size);
}
