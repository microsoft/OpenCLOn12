// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#define NOMINMAX

#include "clc_compiler.h"
#include "compiler.hpp"
#include "platform.hpp"
#include "cache.hpp"

#include <dxcapi.h>

template <typename T>
struct unique_object : public T
{
    using DeleterT = void(*)(T*);
    DeleterT m_Deleter = nullptr;
    unique_object() = default;
    unique_object(DeleterT) : T(), m_Deleter(d) {}
    unique_object(T const& t, DeleterT d = nullptr) : T(t), m_Deleter(d) {}
    unique_object(T &&t, DeleterT d) : T(std::move(t)), m_Deleter(d) {}
    unique_object(unique_object &&o) : T(std::move(o)), m_Deleter(o.m_Deleter) { o.m_Deleter = nullptr; }
    ~unique_object()
    {
        if (m_Deleter)
            m_Deleter(this);
    }
};

class CompilerV2 : public Compiler
{
private:
    XPlatHelpers::unique_module m_Compiler;

    std::mutex m_InitializationLock;
    std::unique_ptr<clc_libclc, void(*)(clc_libclc*)> m_Libclc{nullptr, nullptr};

public:
    CompilerV2(XPlatHelpers::unique_module compiler);
    static CompilerV2 *Instance();

    // Compiler functions
    decltype(&clc_libclc_new_dxil) LoadLibclc = nullptr;
    decltype(&clc_libclc_serialize) SerializeLibclc = nullptr;
    decltype(&clc_libclc_deserialize) DeserializeLibclc = nullptr;
    decltype(&clc_libclc_free_serialized) FreeSerializedLibclc = nullptr;
    decltype(&clc_free_libclc) FreeLibclc = nullptr;
    decltype(&clc_compile_c_to_spirv) CompileImpl = nullptr;
    decltype(&clc_link_spirv) LinkImpl = nullptr;
    decltype(&clc_free_spirv) FreeSpirv = nullptr;
    decltype(&clc_parse_spirv) ParseSpirv = nullptr;
    decltype(&clc_free_parsed_spirv) FreeParsedSpirv = nullptr;
    decltype(&clc_specialize_spirv) SpecializeImpl = nullptr;
    decltype(&clc_spirv_to_dxil) GetKernelImpl = nullptr;
    decltype(&clc_free_dxil_object) FreeDxil = nullptr;
    decltype(&clc_compiler_get_version) GetCompilerVersion = nullptr;

    clc_libclc *GetLibclc() const { return m_Libclc.get(); }

    // Inherited via Compiler
    virtual ~CompilerV2() = default;
    virtual bool Initialize(ShaderCache &cache) final;
    virtual std::unique_ptr<ProgramBinary> Compile(CompileArgs const& args, Logger const& logger) const final;
    virtual std::unique_ptr<ProgramBinary> Link(LinkerArgs const& args, Logger const& logger) const final;
    virtual std::unique_ptr<ProgramBinary> Load(const void *data, size_t size) const final;
    virtual std::unique_ptr<ProgramBinary> Specialize(ProgramBinary const& obj, ProgramBinary::SpecConstantValues const& values, Logger const& logger) const final;
    virtual std::unique_ptr<CompiledDxil> GetKernel(const char *name, ProgramBinary const& obj, CompiledDxil::Configuration const *, Logger const *logger) const final;
    virtual std::unique_ptr<CompiledDxil> CompilerV2::LoadKernel(ProgramBinary const &obj, const void *data, size_t size, CompiledDxil::Metadata const &metadata) const final;
    virtual std::byte * CopyWorkProperties(std::byte *WorkPropertiesBuffer, WorkProperties const& props) const final;
    virtual size_t GetWorkPropertiesChunkSize() const final;
    virtual uint64_t GetVersionForCache() const final;
};

class ProgramBinaryV2 : public ProgramBinary
{
public:
    using unique_obj = unique_object<clc_binary>;

private:
    unique_obj m_Object;
    unique_object<clc_parsed_spirv> m_Parsed;
    bool m_bParsed = false;

public:
    ProgramBinaryV2(unique_obj obj);
    clc_binary const& GetRaw() const { return m_Object; }
    clc_parsed_spirv const& GetParsedInfo() const { return m_Parsed; }

    // Inherited via ProgramBinary
    virtual ~ProgramBinaryV2() = default;
    virtual bool Parse(Logger const *logger) final;
    virtual size_t GetBinarySize() const final;
    virtual const void *GetBinary() const final;
};

class CompiledDxilV2 : public CompiledDxil
{
public:
    using unique_obj = unique_object<clc_dxil_object>;

private:
    unique_obj m_Object;

public:
    CompiledDxilV2(ProgramBinaryV2 const& parent, unique_obj obj);
    CompiledDxilV2(ProgramBinaryV2 const& parent, unique_obj obj, Metadata const &metadata);
    clc_dxil_object const& GetRaw() { return m_Object; }

    // Inherited via CompiledDxil
    virtual ~CompiledDxilV2() = default;
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

CompilerV2::CompilerV2(XPlatHelpers::unique_module compiler)
    : m_Compiler(std::move(compiler))
{
#define GET_FUNC(Member, api) Member = m_Compiler.proc_address<decltype(&api)>(#api)
    GET_FUNC(LoadLibclc, clc_libclc_new_dxil);
    GET_FUNC(SerializeLibclc, clc_libclc_serialize);
    GET_FUNC(DeserializeLibclc, clc_libclc_deserialize);
    GET_FUNC(FreeSerializedLibclc, clc_libclc_free_serialized);
    GET_FUNC(FreeLibclc, clc_free_libclc);
    GET_FUNC(CompileImpl, clc_compile_c_to_spirv);
    GET_FUNC(LinkImpl, clc_link_spirv);
    GET_FUNC(FreeSpirv, clc_free_spirv);
    GET_FUNC(ParseSpirv, clc_parse_spirv);
    GET_FUNC(FreeParsedSpirv, clc_free_parsed_spirv);
    GET_FUNC(SpecializeImpl, clc_specialize_spirv);
    GET_FUNC(GetKernelImpl, clc_spirv_to_dxil);
    GET_FUNC(FreeDxil, clc_free_dxil_object);
    GET_FUNC(GetCompilerVersion, clc_compiler_get_version);
#undef GET_FUNC

    if (!LoadLibclc)
        LoadLibclc = m_Compiler.proc_address<decltype(&clc_libclc_new_dxil)>("clc_libclc_new");

    if (!LoadLibclc ||
        !SerializeLibclc ||
        !DeserializeLibclc ||
        !FreeSerializedLibclc ||
        !FreeLibclc ||
        !CompileImpl ||
        !LinkImpl ||
        !FreeSpirv ||
        !ParseSpirv ||
        !FreeParsedSpirv ||
        !SpecializeImpl ||
        !GetKernelImpl ||
        !FreeDxil ||
        !GetCompilerVersion)
        throw std::runtime_error("Failed to load required compiler entrypoints");

    m_Libclc = decltype(m_Libclc)(nullptr, FreeLibclc);
}

CompilerV2 *CompilerV2::Instance()
{
    return static_cast<CompilerV2*>(g_Platform->GetCompiler());
}

bool CompilerV2::Initialize(ShaderCache &cache)
{
    if (m_Libclc)
        return true;

    std::lock_guard lock(m_InitializationLock);
    if (m_Libclc)
        return true;

    // {1B9DC5F4-545A-4356-98D3-B4C0062E6253}
    static const GUID LibclcKey =
    { 0x1b9dc5f4, 0x545a, 0x4356, { 0x98, 0xd3, 0xb4, 0xc0, 0x6, 0x2e, 0x62, 0x53 } };

    if (DeserializeLibclc)
    {
        if (auto CachedContext = cache.Find(&LibclcKey, sizeof(LibclcKey));
            CachedContext.first)
        {
            m_Libclc.reset(DeserializeLibclc(CachedContext.first.get(), CachedContext.second));
            return true;
        }
    }

    clc_libclc_dxil_options options = {};
    options.optimize = cache.HasCache() && SerializeLibclc && FreeSerializedLibclc;
    m_Libclc.reset(LoadLibclc(nullptr, &options));

    if (m_Libclc && options.optimize)
    {
        void* serialized = nullptr;
        size_t serializedSize = 0;
        SerializeLibclc(m_Libclc.get(), &serialized, &serializedSize);

        if (serialized)
        {
            try
            {
                cache.Store(&LibclcKey, sizeof(LibclcKey), serialized, serializedSize);
            }
            catch (...) {}
            FreeSerializedLibclc(serialized);
        }
    }

    return m_Libclc != nullptr;
}

std::unique_ptr<ProgramBinary> CompilerV2::Compile(CompileArgs const& args, Logger const& logger) const
{
    ProgramBinaryV2::unique_obj obj({}, FreeSpirv);
    clc_compile_args args_impl = {};
    args_impl.args = args.cmdline_args.data();
    args_impl.num_args = (unsigned)args.cmdline_args.size();
    args_impl.source = { "source.cl", args.program_source };

    static_assert(sizeof(clc_named_value) == sizeof(CompileArgs::Header));
    static_assert(offsetof(clc_named_value, name) == offsetof(CompileArgs::Header, name));
    static_assert(offsetof(clc_named_value, value) == offsetof(CompileArgs::Header, contents));
    args_impl.headers = (clc_named_value*)args.headers.data();
    args_impl.num_headers = (unsigned)args.headers.size();

    args_impl.features.fp16 = args.features.fp16;
    args_impl.features.fp64 = args.features.fp64;
    args_impl.features.int64 = args.features.int64;
    args_impl.features.images = args.features.images;
    args_impl.features.images_read_write = args.features.images_read_write;
    args_impl.features.images_write_3d = args.features.images_write_3d;
    args_impl.features.intel_subgroups = args.features.intel_subgroups;
    args_impl.features.subgroups = args.features.subgroups;

    args_impl.spirv_version = CLC_SPIRV_VERSION_MAX;
    args_impl.allowed_spirv_extensions = nullptr;

    args_impl.address_bits = 64;

    auto logger_impl = ConvertLogger(logger);
    if (!CompileImpl(&args_impl, &logger_impl, &obj))
        return nullptr;
    
    return std::make_unique<ProgramBinaryV2>(std::move(obj));
}

std::unique_ptr<ProgramBinary> CompilerV2::Link(LinkerArgs const& args, Logger const& logger) const
{
    ProgramBinaryV2::unique_obj linked({}, FreeSpirv);

    std::vector<clc_binary const*> raw_objs;
    raw_objs.reserve(args.objs.size());
    for (auto& obj : args.objs)
        raw_objs.push_back(&static_cast<ProgramBinaryV2 const*>(obj)->GetRaw());

    clc_linker_args args_impl;
    args_impl.create_library = args.create_library;
    args_impl.num_in_objs = (unsigned)raw_objs.size();
    args_impl.in_objs = raw_objs.data();

    auto logger_impl = ConvertLogger(logger);
    if (!LinkImpl(&args_impl, &logger_impl, &linked))
        return nullptr;

    auto ret = std::make_unique<ProgramBinaryV2>(std::move(linked));
    if (!ret)
        return nullptr;

    if (!ret->Parse(&logger))
        return nullptr;

    return ret;
}

std::unique_ptr<ProgramBinary> CompilerV2::Load(const void *data, size_t size) const
{
    auto deleter = [](clc_binary *obj)
    {
        if (obj->data)
            operator delete(obj->data);
    };
    ProgramBinaryV2::unique_obj obj({}, deleter);

    obj.size = size;
    obj.data = operator new(size);
    memcpy(obj.data, data, size);

    return std::make_unique<ProgramBinaryV2>(std::move(obj));
}

static dxil_shader_model TranslateShaderModel(D3D_SHADER_MODEL sm)
{
    switch (sm)
    {
#define CASE(ver) case D3D_SHADER_MODEL_##ver: return SHADER_MODEL_##ver
        CASE(6_0);
        CASE(6_1);
        CASE(6_2);
        CASE(6_3);
        CASE(6_4);
        CASE(6_5);
        CASE(6_6);
        CASE(6_7);
#undef CASE
    default: return SHADER_MODEL_6_7;
    }
}

static dxil_validator_version GetValidatorVersion(const XPlatHelpers::unique_module &dxil)
{
    if (!dxil)
        return NO_DXIL_VALIDATION;

    auto pfnCreateInstance = dxil.proc_address<decltype(&DxcCreateInstance)>("DxcCreateInstance");
    ComPtr<IDxcVersionInfo> versionInfo;
    if (!pfnCreateInstance ||
        FAILED(pfnCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(versionInfo.ReleaseAndGetAddressOf()))))
        return NO_DXIL_VALIDATION;

    UINT32 major, minor;
    if (FAILED(versionInfo->GetVersion(&major, &minor)))
        return NO_DXIL_VALIDATION;

    if (major == 1)
        return (enum dxil_validator_version)(DXIL_VALIDATOR_1_0 + std::min(minor, 7u));
    if (major > 1)
        return DXIL_VALIDATOR_1_7;
    return NO_DXIL_VALIDATION;
}

std::unique_ptr<ProgramBinary> CompilerV2::Specialize(ProgramBinary const& obj, ProgramBinary::SpecConstantValues const& values, Logger const& logger) const
{
    std::vector<clc_spirv_specialization> specializations;
    specializations.reserve(values.size());
    for (auto& pair : values)
    {
        clc_spirv_specialization value;
        value.id = pair.first;
        value.defined_on_module = true;
        static_assert(sizeof(value.value) == sizeof(pair.second.value));
        memcpy(&value.value, pair.second.value, sizeof(value.value));
        
        specializations.push_back(value);
    }

    struct clc_spirv_specialization_consts args;
    args.specializations = specializations.data();
    args.num_specializations = (unsigned)specializations.size();

    ProgramBinaryV2::unique_obj result({}, FreeSpirv);
    ProgramBinaryV2 const& objv2 = static_cast<ProgramBinaryV2 const&>(obj);
    if (!SpecializeImpl(&objv2.GetRaw(), &objv2.GetParsedInfo(), &args, &result))
        return nullptr;

    auto ret = std::make_unique<ProgramBinaryV2>(std::move(result));
    if (!ret)
        return nullptr;

    // Re-parse because spec constants can be in places like array sizes, or workgroup sizes/hints
    if (!ret->Parse(&logger))
        return nullptr;

    return ret;
}

std::unique_ptr<CompiledDxil> CompilerV2::GetKernel(const char *name, ProgramBinary const& obj, CompiledDxil::Configuration const *conf, Logger const *logger) const
{
    clc_runtime_kernel_conf conf_impl;
    std::vector<clc_runtime_arg_info> conf_args;
    if (conf)
    {
        std::copy(conf->local_size, std::end(conf->local_size), conf_impl.local_size);
        conf_impl.lower_bit_size = (conf->lower_int16 ? 16 : 0) | (conf->lower_int64 ? 64 : 0);
        conf_impl.support_global_work_id_offsets = conf->support_global_work_id_offsets;
        conf_impl.support_workgroup_id_offsets = conf->support_work_group_id_offsets;

        conf_impl.max_shader_model = TranslateShaderModel(conf->shader_model);
        conf_impl.validator_version = GetValidatorVersion(g_Platform->GetDXIL());

        conf_args.reserve(conf->args.size());
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

    ProgramBinaryV2 const& objv2 = static_cast<ProgramBinaryV2 const&>(obj);
    clc_dxil_object raw_dxil = {};
    if (!GetKernelImpl(GetLibclc(), &objv2.GetRaw(), &objv2.GetParsedInfo(), name, conf ? &conf_impl : nullptr, nullptr, logger ? &logger_impl : nullptr, &raw_dxil))
        return nullptr;

    CompiledDxilV2::unique_obj dxil(raw_dxil, FreeDxil);
    return std::make_unique<CompiledDxilV2>(static_cast<ProgramBinaryV2 const&>(obj), std::move(dxil));
}

std::unique_ptr<CompiledDxil> CompilerV2::LoadKernel(ProgramBinary const &obj, const void *data, size_t size, CompiledDxil::Metadata const &metadata) const
{
    CompiledDxilV2::unique_obj dxil;
    dxil.binary.data = operator new(size);
    dxil.binary.size = size;
    memcpy(dxil.binary.data, data, size);
    dxil.m_Deleter = [](clc_dxil_object *p) { operator delete(p->binary.data); };
    return std::make_unique<CompiledDxilV2>(static_cast<ProgramBinaryV2 const &>(obj), std::move(dxil), metadata);
}

std::byte *CompilerV2::CopyWorkProperties(std::byte *WorkPropertiesBuffer, WorkProperties const& props) const
{
    static_assert(sizeof(props) == sizeof(clc_work_properties_data));
    static_assert(offsetof(WorkProperties, global_offset_z) == offsetof(clc_work_properties_data, global_offset_z));
    static_assert(offsetof(WorkProperties, work_dim) == offsetof(clc_work_properties_data, work_dim));
    static_assert(offsetof(WorkProperties, group_count_total_z) == offsetof(clc_work_properties_data, group_count_total_z));
    static_assert(offsetof(WorkProperties, group_id_offset_z) == offsetof(clc_work_properties_data, group_id_offset_z));
    memcpy(WorkPropertiesBuffer, &props, sizeof(props));
    return WorkPropertiesBuffer + GetWorkPropertiesChunkSize();
}

size_t CompilerV2::GetWorkPropertiesChunkSize() const
{
    return std::max<size_t>(sizeof(clc_work_properties_data), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
}

uint64_t CompilerV2::GetVersionForCache() const
{
    return GetCompilerVersion();
}


ProgramBinaryV2::ProgramBinaryV2(unique_obj obj)
    : m_Object(std::move(obj))
    , m_Parsed({}, CompilerV2::Instance()->FreeParsedSpirv)
{
}

bool ProgramBinaryV2::Parse(Logger const *logger)
{
    if (m_bParsed)
        return true;

    clc_logger logger_impl;
    if (logger)
        logger_impl = ConvertLogger(*logger);

    if (!CompilerV2::Instance()->ParseSpirv(&m_Object, logger ? &logger_impl : nullptr, &m_Parsed))
        return false;

    if (m_Parsed.num_kernels)
    {
        m_KernelInfo.reserve(m_Parsed.num_kernels);
        for (unsigned i = 0; i < m_Parsed.num_kernels; ++i)
        {
            Kernel info;
            info.name = m_Parsed.kernels[i].name;
            info.vec_hint_size = m_Parsed.kernels[i].vec_hint_size;
            static_assert(CLC_VEC_HINT_TYPE_CHAR == (int)Kernel::VecHintType::Char);
            static_assert(CLC_VEC_HINT_TYPE_SHORT == (int)Kernel::VecHintType::Short);
            static_assert(CLC_VEC_HINT_TYPE_INT == (int)Kernel::VecHintType::Int);
            static_assert(CLC_VEC_HINT_TYPE_LONG == (int)Kernel::VecHintType::Long);
            static_assert(CLC_VEC_HINT_TYPE_HALF == (int)Kernel::VecHintType::Half);
            static_assert(CLC_VEC_HINT_TYPE_FLOAT == (int)Kernel::VecHintType::Float);
            static_assert(CLC_VEC_HINT_TYPE_DOUBLE == (int)Kernel::VecHintType::Double);
            info.vec_hint_type = (Kernel::VecHintType)m_Parsed.kernels[i].vec_hint_type;

            info.args.reserve(m_Parsed.kernels[i].num_args);
            for (unsigned j = 0; j < m_Parsed.kernels[i].num_args; ++j)
            {
                Kernel::Arg arg;
                static_assert(CLC_KERNEL_ARG_ADDRESS_PRIVATE == (int)Kernel::Arg::AddressSpace::Private);
                static_assert(CLC_KERNEL_ARG_ADDRESS_CONSTANT == (int)Kernel::Arg::AddressSpace::Constant);
                static_assert(CLC_KERNEL_ARG_ADDRESS_LOCAL == (int)Kernel::Arg::AddressSpace::Local);
                static_assert(CLC_KERNEL_ARG_ADDRESS_GLOBAL == (int)Kernel::Arg::AddressSpace::Global);
                arg.address_qualifier = (Kernel::Arg::AddressSpace)m_Parsed.kernels[i].args[j].address_qualifier;
                arg.is_const = (m_Parsed.kernels[i].args[j].type_qualifier & CLC_KERNEL_ARG_TYPE_CONST) != 0;
                arg.is_restrict = (m_Parsed.kernels[i].args[j].type_qualifier & CLC_KERNEL_ARG_TYPE_RESTRICT) != 0;
                arg.is_volatile = (m_Parsed.kernels[i].args[j].type_qualifier & CLC_KERNEL_ARG_TYPE_VOLATILE) != 0;
                arg.readable = (m_Parsed.kernels[i].args[j].access_qualifier & CLC_KERNEL_ARG_ACCESS_READ) != 0;
                arg.writable = (m_Parsed.kernels[i].args[j].access_qualifier & CLC_KERNEL_ARG_ACCESS_WRITE) != 0;
                arg.name = m_Parsed.kernels[i].args[j].name;
                arg.type_name = m_Parsed.kernels[i].args[j].type_name;
                info.args.push_back(arg);
            }

            m_KernelInfo.push_back(std::move(info));
        }
    }

    if (m_Parsed.num_spec_constants)
    {
        for (unsigned i = 0; i < m_Parsed.num_spec_constants; ++i)
        {
            auto& spec_constant = m_Parsed.spec_constants[i];
            unsigned const_size = 4;
            switch (spec_constant.type)
            {
            case CLC_SPEC_CONSTANT_BOOL:
            case CLC_SPEC_CONSTANT_INT8:
            case CLC_SPEC_CONSTANT_UINT8:
                const_size = 1;
                break;
            case CLC_SPEC_CONSTANT_INT16:
            case CLC_SPEC_CONSTANT_UINT16:
                const_size = 2;
                break;
            case CLC_SPEC_CONSTANT_FLOAT:
            case CLC_SPEC_CONSTANT_INT32:
            case CLC_SPEC_CONSTANT_UINT32:
                const_size = 4;
                break;
            case CLC_SPEC_CONSTANT_DOUBLE:
            case CLC_SPEC_CONSTANT_INT64:
            case CLC_SPEC_CONSTANT_UINT64:
                const_size = 8;
                break;
            default:
                assert(!"Unexpected spec constant type");
            }
            SpecConstantInfo info = { const_size };
            [[maybe_unused]] auto emplaceRet = m_SpecConstants.emplace(spec_constant.id, info);
            assert(emplaceRet.second);
        }
    }
    
    m_bParsed = true;
    return true;
}

size_t ProgramBinaryV2::GetBinarySize() const
{
    return m_Object.size;
}

const void *ProgramBinaryV2::GetBinary() const
{
    return m_Object.data;
}


CompiledDxilV2::CompiledDxilV2(ProgramBinaryV2 const& parent, unique_obj obj)
    : CompiledDxil(parent, obj.kernel->name)
    , m_Object(std::move(obj))
{
    m_Metadata.kernel_inputs_cbv_id = m_Object.metadata.kernel_inputs_cbv_id;
    m_Metadata.kernel_inputs_buf_size = m_Object.metadata.kernel_inputs_buf_size;
    m_Metadata.work_properties_cbv_id = m_Object.metadata.work_properties_cbv_id;
    m_Metadata.printf_uav_id = m_Object.metadata.printf.uav_id;
    m_Metadata.num_uavs = m_Object.metadata.num_uavs;
    m_Metadata.num_srvs = m_Object.metadata.num_srvs;
    m_Metadata.num_samplers = m_Object.metadata.num_samplers;
    m_Metadata.local_mem_size = m_Object.metadata.local_mem_size;
    m_Metadata.priv_mem_size = m_Object.metadata.priv_mem_size;

    std::copy(m_Object.metadata.local_size, std::end(m_Object.metadata.local_size), m_Metadata.local_size);
    std::copy(m_Object.metadata.local_size_hint, std::end(m_Object.metadata.local_size_hint), m_Metadata.local_size_hint);

    m_Metadata.args.reserve(m_Object.kernel->num_args);
    for (unsigned i = 0; i < m_Object.kernel->num_args; ++i)
    {
        CompiledDxil::Metadata::Arg arg;
        auto& argMeta = m_Object.metadata.args[i];
        auto& argInfo = m_Object.kernel->args[i];
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

    m_Metadata.consts.reserve(m_Object.metadata.num_consts);
    for (unsigned i = 0; i < m_Object.metadata.num_consts; ++i)
    {
        CompiledDxil::Metadata::Consts consts;
        consts.data = m_Object.metadata.consts[i].data;
        consts.size = m_Object.metadata.consts[i].size;
        consts.uav_id = m_Object.metadata.consts[i].uav_id;
        m_Metadata.consts.push_back(consts);
    }

    m_Metadata.constSamplers.reserve(m_Object.metadata.num_const_samplers);
    for (unsigned i = 0; i < m_Object.metadata.num_const_samplers; ++i)
    {
        CompiledDxil::Metadata::ConstSampler sampler;
        sampler.addressing_mode = m_Object.metadata.const_samplers[i].addressing_mode;
        sampler.filter_mode = m_Object.metadata.const_samplers[i].filter_mode;
        sampler.normalized_coords = m_Object.metadata.const_samplers[i].normalized_coords;
        sampler.sampler_id = m_Object.metadata.const_samplers[i].sampler_id;
        m_Metadata.constSamplers.push_back(sampler);
    }

    m_Metadata.printfs.reserve(m_Object.metadata.printf.info_count);
    for (unsigned i = 0; i < m_Object.metadata.printf.info_count; ++i)
    {
        CompiledDxil::Metadata::Printf printf;
        printf.arg_sizes = m_Object.metadata.printf.infos[i].arg_sizes;
        printf.num_args = m_Object.metadata.printf.infos[i].num_args;
        printf.str = m_Object.metadata.printf.infos[i].str;
        m_Metadata.printfs.push_back(printf);
    }
}

CompiledDxilV2::CompiledDxilV2(ProgramBinaryV2 const &parent, unique_obj obj, Metadata const &metadata)
    : CompiledDxil(parent, metadata)
    , m_Object(std::move(obj))
{
}

size_t CompiledDxilV2::GetBinarySize() const
{
    return m_Object.binary.size;
}

const void *CompiledDxilV2::GetBinary() const
{
    return m_Object.binary.data;
}

void *CompiledDxilV2::GetBinary()
{
    return m_Object.binary.data;
}

std::unique_ptr<Compiler> Compiler::GetV2()
{
    XPlatHelpers::unique_module compiler;
    compiler.load("CLOn12Compiler.dll");
    if (!compiler)
        LoadFromNextToSelf(compiler, "CLOn12Compiler.dll");

    if (!compiler)
        return nullptr;

    return std::make_unique<CompilerV2>(std::move(compiler));
}
