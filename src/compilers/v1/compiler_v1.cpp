// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#define NOMINMAX

#include "clc_compiler.h"
#include "compiler.hpp"
#include "cache.hpp"
#include "platform.hpp"

class CompilerV1 : public Compiler
{
private:
    XPlatHelpers::unique_module m_Compiler;

    std::mutex m_InitializationLock;
    std::unique_ptr<clc_context, void(*)(clc_context*)> m_Context{nullptr, nullptr};

public:
    CompilerV1(XPlatHelpers::unique_module compiler);
    static CompilerV1 *Instance();

    // Compiler functions
    decltype(&clc_context_new) CreateContext = nullptr;
    decltype(&clc_context_serialize) SerializeContext = nullptr;
    decltype(&clc_context_deserialize) DeserializeContext = nullptr;
    decltype(&clc_context_free_serialized) FreeSerializedContext = nullptr;
    decltype(&clc_free_context) FreeContext = nullptr;
    decltype(&clc_compile) CompileImpl = nullptr;
    decltype(&clc_link) LinkImpl = nullptr;
    decltype(&clc_free_object) FreeSpirv = nullptr;
    decltype(&clc_to_dxil) GetKernelImpl = nullptr;
    decltype(&clc_free_dxil_object) FreeDxil = nullptr;
    decltype(&clc_compiler_get_version) GetCompilerVersion = nullptr;

    clc_context *GetContext() const { return m_Context.get(); }

    // Inherited via Compiler
    virtual ~CompilerV1() = default;
    virtual bool Initialize(ShaderCache &cache) final;
    virtual std::unique_ptr<ProgramBinary> Compile(CompileArgs const& args, Logger const& logger) const final;
    virtual std::unique_ptr<ProgramBinary> Link(LinkerArgs const& args, Logger const& logger) const final;
    virtual std::unique_ptr<ProgramBinary> Load(const void *data, size_t size) const final;
    virtual std::unique_ptr<CompiledDxil> GetKernel(const char *name, ProgramBinary const& obj, CompiledDxil::Configuration const *, Logger const *logger) const final;
    virtual std::byte * CopyWorkProperties(std::byte *WorkPropertiesBuffer, WorkProperties const& props) const final;
    virtual size_t GetWorkPropertiesChunkSize() const final;
    virtual uint64_t GetVersionForCache() const final;
};

class ProgramBinaryV1 : public ProgramBinary
{
public:
    using unique_ptr = std::unique_ptr<clc_object, void(*)(clc_object*)>;

private:
     unique_ptr m_Object;

public:
    ProgramBinaryV1(unique_ptr obj);
    clc_object *GetRaw() const { return m_Object.get(); }

    // Inherited via ProgramBinary
    virtual ~ProgramBinaryV1() = default;
    virtual bool Parse(Logger const *logger) final;
    virtual size_t GetBinarySize() const final;
    virtual const void *GetBinary() const final;
};

class CompiledDxilV1 : public CompiledDxil
{
public:
    using unique_ptr = std::unique_ptr<clc_dxil_object, void(*)(clc_dxil_object*)>;

private:
    unique_ptr m_Object;

public:
    CompiledDxilV1(ProgramBinaryV1 const& parent, unique_ptr obj);
    clc_dxil_object *GetRaw() const { return m_Object.get(); }

    // Inherited via CompiledDxil
    virtual ~CompiledDxilV1() = default;
    virtual size_t GetBinarySize() const final;
    virtual const void *GetBinary() const final;
    virtual void *GetBinary() final;
};

static clc_logger ConvertLogger(Logger const& logger)
{
    auto log = [](void *ctx, const char *msg) { static_cast<Logger*>(ctx)->Log(msg); };
    clc_logger ret;
    ret.error = log;
    ret.warning = log;
    ret.priv = (void*)&logger;
    return ret;
}

#if 0
#endif

CompilerV1::CompilerV1(XPlatHelpers::unique_module compiler)
    : m_Compiler(std::move(compiler))
{
#define GET_FUNC(Member, api) Member = m_Compiler.proc_address<decltype(&api)>(#api)
    GET_FUNC(CreateContext, clc_context_new);
    GET_FUNC(SerializeContext, clc_context_serialize);
    GET_FUNC(DeserializeContext, clc_context_deserialize);
    GET_FUNC(FreeSerializedContext, clc_context_free_serialized);
    GET_FUNC(FreeContext, clc_free_context);
    GET_FUNC(CompileImpl, clc_compile);
    GET_FUNC(LinkImpl, clc_link);
    GET_FUNC(FreeSpirv, clc_free_object);
    GET_FUNC(GetKernelImpl, clc_to_dxil);
    GET_FUNC(FreeDxil, clc_free_dxil_object);
    GET_FUNC(GetCompilerVersion, clc_compiler_get_version);
#undef GET_FUNC

    // Older versions of the v1 compiler interface didn't have support for "context" serialization
    // and didn't have version info exported. These aren't strictly required to work.
    if (!CreateContext ||
        !FreeContext ||
        !CompileImpl ||
        !LinkImpl ||
        !FreeSpirv ||
        !GetKernelImpl ||
        !FreeDxil)
        throw std::runtime_error("Failed to load required compiler entrypoints");

    m_Context = decltype(m_Context)(nullptr, FreeContext);
}

CompilerV1 *CompilerV1::Instance()
{
    return static_cast<CompilerV1*>(g_Platform->GetCompiler());
}

bool CompilerV1::Initialize(ShaderCache &cache)
{
    if (m_Context)
        return true;

    std::lock_guard lock(m_InitializationLock);
    if (m_Context)
        return true;

    // {1B9DC5F4-545A-4356-98D3-B4C0062E6253}
    static const GUID ClcContextKey =
    { 0x1b9dc5f4, 0x545a, 0x4356, { 0x98, 0xd3, 0xb4, 0xc0, 0x6, 0x2e, 0x62, 0x53 } };

    if (DeserializeContext)
    {
        if (auto CachedContext = cache.Find(&ClcContextKey, sizeof(ClcContextKey));
            CachedContext.first)
        {
            m_Context.reset(DeserializeContext(CachedContext.first.get(), CachedContext.second));
            return true;
        }
    }

    clc_context_options options = {};
    options.optimize = cache.HasCache() && SerializeContext && FreeSerializedContext;
    m_Context.reset(CreateContext(nullptr, &options));

    if (m_Context && options.optimize)
    {
        void* serialized = nullptr;
        size_t serializedSize = 0;
        SerializeContext(m_Context.get(), &serialized, &serializedSize);

        if (serialized)
        {
            try
            {
                cache.Store(&ClcContextKey, sizeof(ClcContextKey), serialized, serializedSize);
            }
            catch (...) {}
            FreeSerializedContext(serialized);
        }
    }

    return m_Context != nullptr;
}

std::unique_ptr<ProgramBinary> CompilerV1::Compile(CompileArgs const& args, Logger const& logger) const
{
    ProgramBinaryV1::unique_ptr obj(nullptr, FreeSpirv);
    clc_compile_args args_impl;
    args_impl.args = args.cmdline_args.data();
    args_impl.num_args = (unsigned)args.cmdline_args.size();
    args_impl.source = { "source.cl", args.program_source };

    static_assert(sizeof(clc_named_value) == sizeof(CompileArgs::Header));
    static_assert(offsetof(clc_named_value, name) == offsetof(CompileArgs::Header, name));
    static_assert(offsetof(clc_named_value, value) == offsetof(CompileArgs::Header, contents));
    args_impl.headers = (clc_named_value*)args.headers.data();
    args_impl.num_headers = (unsigned)args.headers.size();

    auto logger_impl = ConvertLogger(logger);
    obj.reset(CompileImpl(GetContext(), &args_impl, &logger_impl));
    
    return obj ? std::make_unique<ProgramBinaryV1>(std::move(obj)) : nullptr;
}

std::unique_ptr<ProgramBinary> CompilerV1::Link(LinkerArgs const& args, Logger const& logger) const
{
    ProgramBinaryV1::unique_ptr linked(nullptr, FreeSpirv);

    std::vector<clc_object *> raw_objs;
    raw_objs.reserve(args.objs.size());
    for (auto& obj : args.objs)
        raw_objs.push_back(static_cast<ProgramBinaryV1 const*>(obj)->GetRaw());

    clc_linker_args args_impl;
    args_impl.create_library = args.create_library;
    args_impl.num_in_objs = (unsigned)raw_objs.size();
    args_impl.in_objs = raw_objs.data();

    auto logger_impl = ConvertLogger(logger);
    linked.reset(LinkImpl(GetContext(), &args_impl, &logger_impl));

    if (!linked)
        return nullptr;

    auto ret = std::make_unique<ProgramBinaryV1>(std::move(linked));
    if (!ret)
        return nullptr;

    if (!ret->Parse(&logger))
        return nullptr;

    return ret;
}

std::unique_ptr<ProgramBinary> CompilerV1::Load(const void *data, size_t size) const
{
    auto deleter = [](clc_object *obj)
    {
        if (obj->spvbin.data)
            delete[] obj->spvbin.data;
        delete obj;
    };
    ProgramBinaryV1::unique_ptr obj(new clc_object{}, deleter);

    obj->spvbin.size = size;
    obj->spvbin.data = new uint32_t[size / 4];
    memcpy(obj->spvbin.data, data, size);

    return std::make_unique<ProgramBinaryV1>(std::move(obj));
}

std::unique_ptr<CompiledDxil> CompilerV1::GetKernel(const char *name, ProgramBinary const& obj, CompiledDxil::Configuration const *conf, Logger const *logger) const
{
    CompiledDxilV1::unique_ptr dxil(nullptr, FreeDxil);

    clc_runtime_kernel_conf conf_impl;
    std::vector<clc_runtime_arg_info> conf_args;
    if (conf)
    {
        std::copy(conf->local_size, std::end(conf->local_size), conf_impl.local_size);
        conf_impl.lower_bit_size = (conf->lower_int16 ? 16 : 0) | (conf->lower_int64 ? 64 : 0);
        conf_impl.support_global_work_id_offsets = conf->support_global_work_id_offsets;
        conf_impl.support_work_group_id_offsets = conf->support_work_group_id_offsets;

        conf_args.resize(conf->args.size());
        for (auto& arg : conf->args)
        {
            clc_runtime_arg_info arg_impl;
            if (auto local = std::get_if<CompiledDxil::Configuration::Arg::Local>(&arg.config); local)
            {
                arg_impl.localptr.size = local->size;
            }
            else if (auto sampler = std::get_if<CompiledDxil::Configuration::Arg::Sampler>(&arg.config); sampler)
            {
                arg_impl.sampler.addressing_mode = sampler->addressingMode;
                arg_impl.sampler.linear_filtering = sampler->linearFiltering;
                arg_impl.sampler.normalized_coords = sampler->normalizedCoords;
            }
            conf_args.push_back(arg_impl);
        }
        conf_impl.args = conf_args.data();
    }

    clc_logger logger_impl;
    if (logger)
        logger_impl = ConvertLogger(*logger);
    dxil.reset(GetKernelImpl(GetContext(), static_cast<ProgramBinaryV1 const&>(obj).GetRaw(), name, conf ? &conf_impl : nullptr, logger ? &logger_impl : nullptr));

    return dxil ? std::make_unique<CompiledDxilV1>(static_cast<ProgramBinaryV1 const&>(obj), std::move(dxil)) : nullptr;
}

std::byte *CompilerV1::CopyWorkProperties(std::byte *WorkPropertiesBuffer, WorkProperties const& props) const
{
    static_assert(sizeof(props) == sizeof(clc_work_properties_data));
    static_assert(offsetof(WorkProperties, global_offset_z) == offsetof(clc_work_properties_data, global_offset_z));
    static_assert(offsetof(WorkProperties, work_dim) == offsetof(clc_work_properties_data, work_dim));
    static_assert(offsetof(WorkProperties, group_count_total_z) == offsetof(clc_work_properties_data, group_count_total_z));
    static_assert(offsetof(WorkProperties, group_id_offset_z) == offsetof(clc_work_properties_data, group_id_offset_z));
    memcpy(WorkPropertiesBuffer, &props, sizeof(props));
    return WorkPropertiesBuffer + GetWorkPropertiesChunkSize();
}

size_t CompilerV1::GetWorkPropertiesChunkSize() const
{
    return std::max<size_t>(sizeof(clc_work_properties_data), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
}

uint64_t CompilerV1::GetVersionForCache() const
{
    if (GetCompilerVersion)
    {
        return GetCompilerVersion();
    }
#if _WIN32
    else
    {
        WCHAR FileName[MAX_PATH];
        DWORD FileNameLength = GetModuleFileNameW(m_Compiler.get(), FileName, ARRAYSIZE(FileName));

        if (FileNameLength != 0 && FileNameLength != ARRAYSIZE(FileName))
        {
            HANDLE hFile = CreateFileW(FileName, GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE)
            {
                FILETIME Time = {};
                GetFileTime(hFile, nullptr, nullptr, &Time);
                CloseHandle(hFile);
                return reinterpret_cast<UINT64&>(Time);
            }
        }
    }
#endif
    return 0;
}


ProgramBinaryV1::ProgramBinaryV1(unique_ptr obj)
    : m_Object(std::move(obj))
{
}

bool ProgramBinaryV1::Parse(Logger const *)
{
    if (m_KernelInfo.size())
        return true;

    if (m_Object->num_kernels)
    {
        m_KernelInfo.reserve(m_Object->num_kernels);
        for (unsigned i = 0; i < m_Object->num_kernels; ++i)
        {
            Kernel info;
            info.name = m_Object->kernels[i].name;
            info.vec_hint_size = m_Object->kernels[i].vec_hint_size;
            static_assert(CLC_VEC_HINT_TYPE_CHAR == (int)Kernel::VecHintType::Char);
            static_assert(CLC_VEC_HINT_TYPE_SHORT == (int)Kernel::VecHintType::Short);
            static_assert(CLC_VEC_HINT_TYPE_INT == (int)Kernel::VecHintType::Int);
            static_assert(CLC_VEC_HINT_TYPE_LONG == (int)Kernel::VecHintType::Long);
            static_assert(CLC_VEC_HINT_TYPE_HALF == (int)Kernel::VecHintType::Half);
            static_assert(CLC_VEC_HINT_TYPE_FLOAT == (int)Kernel::VecHintType::Float);
            static_assert(CLC_VEC_HINT_TYPE_DOUBLE == (int)Kernel::VecHintType::Double);
            info.vec_hint_type = (Kernel::VecHintType)m_Object->kernels[i].vec_hint_type;

            info.args.reserve(m_Object->kernels[i].num_args);
            for (unsigned j = 0; j < m_Object->kernels[i].num_args; ++j)
            {
                Kernel::Arg arg;
                static_assert(CLC_KERNEL_ARG_ADDRESS_PRIVATE == (int)Kernel::Arg::AddressSpace::Private);
                static_assert(CLC_KERNEL_ARG_ADDRESS_CONSTANT == (int)Kernel::Arg::AddressSpace::Constant);
                static_assert(CLC_KERNEL_ARG_ADDRESS_LOCAL == (int)Kernel::Arg::AddressSpace::Local);
                static_assert(CLC_KERNEL_ARG_ADDRESS_GLOBAL == (int)Kernel::Arg::AddressSpace::Global);
                arg.address_qualifier = (Kernel::Arg::AddressSpace)m_Object->kernels[i].args[j].address_qualifier;
                arg.is_const = (m_Object->kernels[i].args[j].type_qualifier & CLC_KERNEL_ARG_TYPE_CONST) != 0;
                arg.is_restrict = (m_Object->kernels[i].args[j].type_qualifier & CLC_KERNEL_ARG_TYPE_RESTRICT) != 0;
                arg.is_volatile = (m_Object->kernels[i].args[j].type_qualifier & CLC_KERNEL_ARG_TYPE_VOLATILE) != 0;
                arg.readable = (m_Object->kernels[i].args[j].access_qualifier & CLC_KERNEL_ARG_ACCESS_READ) != 0;
                arg.writable = (m_Object->kernels[i].args[j].access_qualifier & CLC_KERNEL_ARG_ACCESS_WRITE) != 0;
                arg.name = m_Object->kernels[i].args[j].name;
                arg.type_name = m_Object->kernels[i].args[j].type_name;
                info.args.push_back(arg);
            }

            m_KernelInfo.push_back(std::move(info));
        }
        return true;
    }
    return false;
}

size_t ProgramBinaryV1::GetBinarySize() const
{
    return m_Object->spvbin.size;
}

const void *ProgramBinaryV1::GetBinary() const
{
    return m_Object->spvbin.data;
}


CompiledDxilV1::CompiledDxilV1(ProgramBinaryV1 const& parent, unique_ptr obj)
    : CompiledDxil(parent, obj->kernel->name)
    , m_Object(std::move(obj))
{
    m_Metadata.kernel_inputs_cbv_id = m_Object->metadata.kernel_inputs_cbv_id;
    m_Metadata.kernel_inputs_buf_size = m_Object->metadata.kernel_inputs_buf_size;
    m_Metadata.work_properties_cbv_id = m_Object->metadata.work_properties_cbv_id;
    m_Metadata.printf_uav_id = m_Object->metadata.printf.uav_id;
    m_Metadata.num_uavs = m_Object->metadata.num_uavs;
    m_Metadata.num_srvs = m_Object->metadata.num_srvs;
    m_Metadata.num_samplers = m_Object->metadata.num_samplers;
    m_Metadata.local_mem_size = m_Object->metadata.local_mem_size;
    m_Metadata.priv_mem_size = m_Object->metadata.priv_mem_size;

    std::copy(m_Object->metadata.local_size, std::end(m_Object->metadata.local_size), m_Metadata.local_size);
    std::copy(m_Object->metadata.local_size_hint, std::end(m_Object->metadata.local_size_hint), m_Metadata.local_size_hint);

    m_Metadata.args.reserve(m_Object->kernel->num_args);
    for (unsigned i = 0; i < m_Object->kernel->num_args; ++i)
    {
        CompiledDxil::Metadata::Arg arg;
        auto& argMeta = m_Object->metadata.args[i];
        auto& argInfo = m_Object->kernel->args[i];
        arg.offset = argMeta.offset;
        arg.size = argMeta.size;
        if (argInfo.address_qualifier == CLC_KERNEL_ARG_ADDRESS_GLOBAL ||
            argInfo.address_qualifier == CLC_KERNEL_ARG_ADDRESS_CONSTANT)
        {
            if (argInfo.access_qualifier)
            {
                CompiledDxil::Metadata::Arg::Image imageMeta;
                imageMeta.num_buffer_ids = argMeta.image.num_buf_ids;
                std::copy(argMeta.image.buf_ids, std::end(argMeta.image.buf_ids), imageMeta.buffer_ids);
                arg.properties = imageMeta;
            }
            else
            {
                arg.properties = CompiledDxil::Metadata::Arg::Memory{ argMeta.globconstptr.buf_id };
            }
        }
        else if (argInfo.address_qualifier == CLC_KERNEL_ARG_ADDRESS_LOCAL)
        {
            arg.properties = CompiledDxil::Metadata::Arg::Local{ argMeta.localptr.sharedmem_offset };
        }
        else if (strcmp(argInfo.type_name, "sampler_t") == 0)
        {
            arg.properties = CompiledDxil::Metadata::Arg::Sampler{ argMeta.sampler.sampler_id };
        }
        m_Metadata.args.push_back(arg);
    }

    m_Metadata.consts.reserve(m_Object->metadata.num_consts);
    for (unsigned i = 0; i < m_Object->metadata.num_consts; ++i)
    {
        CompiledDxil::Metadata::Consts consts;
        consts.data = m_Object->metadata.consts[i].data;
        consts.size = m_Object->metadata.consts[i].size;
        consts.uav_id = m_Object->metadata.consts[i].uav_id;
        m_Metadata.consts.push_back(consts);
    }

    m_Metadata.constSamplers.reserve(m_Object->metadata.num_const_samplers);
    for (unsigned i = 0; i < m_Object->metadata.num_const_samplers; ++i)
    {
        CompiledDxil::Metadata::ConstSampler sampler;
        sampler.addressing_mode = m_Object->metadata.const_samplers[i].addressing_mode;
        sampler.filter_mode = m_Object->metadata.const_samplers[i].filter_mode;
        sampler.normalized_coords = m_Object->metadata.const_samplers[i].normalized_coords;
        sampler.sampler_id = m_Object->metadata.const_samplers[i].sampler_id;
        m_Metadata.constSamplers.push_back(sampler);
    }

    m_Metadata.printfs.reserve(m_Object->metadata.printf.info_count);
    for (unsigned i = 0; i < m_Object->metadata.printf.info_count; ++i)
    {
        CompiledDxil::Metadata::Printf printf;
        printf.arg_sizes = m_Object->metadata.printf.infos[i].arg_sizes;
        printf.num_args = m_Object->metadata.printf.infos[i].num_args;
        printf.str = m_Object->metadata.printf.infos[i].str;
        m_Metadata.printfs.push_back(printf);
    }
}

size_t CompiledDxilV1::GetBinarySize() const
{
    return m_Object->binary.size;
}

const void *CompiledDxilV1::GetBinary() const
{
    return m_Object->binary.data;
}

void *CompiledDxilV1::GetBinary()
{
    return m_Object->binary.data;
}

std::unique_ptr<Compiler> Compiler::GetV1()
{
    XPlatHelpers::unique_module compiler;
    compiler.load("CLGLOn12Compiler.dll");
    if (!compiler)
        LoadFromNextToSelf(compiler, "CLGLOn12Compiler.dll");

    if (!compiler)
        return nullptr;

    return std::make_unique<CompilerV1>(std::move(compiler));
}
