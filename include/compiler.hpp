// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include <vector>
#include <memory>
#include <mutex>
#include <variant>
#include <cstddef>
#include <unordered_map>

#include <directx/d3d12.h>

class Logger
{
protected:
    std::recursive_mutex &m_lock;
    std::string &m_buildLog;

public:
    Logger(std::recursive_mutex &lock, std::string& build_log)
        : m_lock(lock), m_buildLog(build_log)
    {
    }
    void Log(const char *msg) const;
};

// An abstraction over a program binary
class ProgramBinary
{
public:
    struct Kernel
    {
        struct Arg
        {
            enum class AddressSpace
            {
                Private, Constant, Local, Global
            };
            const char *name;
            const char *type_name;
            bool readable, writable;
            bool is_const, is_restrict, is_volatile;
            AddressSpace address_qualifier;
        };
        enum class VecHintType
        {
            Char, Short, Int, Long, Half, Float, Double
        };

        const char *name;
        std::vector<Arg> args;
        unsigned vec_hint_size;
        VecHintType vec_hint_type;
    };

    struct SpecConstantInfo
    {
        unsigned value_size;
    };
    struct SpecConstantValue
    {
        static constexpr size_t MaxValueSize = 8;
        char value[MaxValueSize];
    };
    using SpecConstantValues = std::unordered_map<uint32_t, SpecConstantValue>;

    virtual ~ProgramBinary() = default;
    virtual bool Parse(Logger const *logger) = 0;
    virtual size_t GetBinarySize() const = 0;
    virtual const void* GetBinary() const = 0;

    const std::vector<Kernel> &GetKernelInfo() const;
    const SpecConstantInfo *GetSpecConstantInfo(uint32_t ID) const;

protected:
    std::vector<Kernel> m_KernelInfo;
    std::unordered_map<uint32_t, SpecConstantInfo> m_SpecConstants;
};

// An abstraction over DXIL + metadata
class CompiledDxil
{
public:
    struct Metadata
    {
        struct Arg
        {
            unsigned offset, size;

            struct Image
            {
                unsigned buffer_ids[3];
                unsigned num_buffer_ids;
            };
            struct Sampler
            {
                unsigned sampler_id;
            };
            struct Memory
            {
                unsigned buffer_id;
            };
            struct Local
            {
                unsigned sharedmem_offset;
            };
            std::variant<std::monostate, Image, Sampler, Memory, Local> properties;
        };
        struct Consts
        {
            void *data;
            size_t size;
            unsigned uav_id;
        };
        struct ConstSampler
        {
            unsigned sampler_id;
            unsigned addressing_mode;
            unsigned filter_mode;
            bool normalized_coords;
        };
        struct Printf
        {
            unsigned num_args;
            unsigned *arg_sizes;
            char *str;
        };

        ProgramBinary::Kernel const& program_kernel_info;

        std::vector<Arg> args;
        std::vector<Consts> consts;
        std::vector<ConstSampler> constSamplers;
        std::vector<Printf> printfs;

        unsigned kernel_inputs_cbv_id;
        unsigned kernel_inputs_buf_size;
        unsigned work_properties_cbv_id;
        int printf_uav_id;
        size_t num_uavs;
        size_t num_srvs;
        size_t num_samplers;
        size_t local_mem_size;
        size_t priv_mem_size;

        uint16_t local_size[3];
        uint16_t local_size_hint[3];

        Metadata(ProgramBinary::Kernel const& parent)
            : program_kernel_info(parent)
        {
        }
    };

    struct Configuration
    {
        struct Arg
        {
            struct Local
            {
                unsigned size;
            };
            struct Sampler
            {
                bool normalizedCoords, linearFiltering;
                unsigned addressingMode;
            };
            std::variant<std::monostate, Local, Sampler> config;
        };

        uint16_t local_size[3];
        std::vector<Arg> args;
        bool lower_int64;
        bool lower_int16;
        bool support_global_work_id_offsets;
        bool support_work_group_id_offsets;

        D3D_SHADER_MODEL shader_model;
    };

    virtual ~CompiledDxil() = default;
    virtual size_t GetBinarySize() const = 0;
    virtual const void* GetBinary() const = 0;
    virtual void *GetBinary() = 0;

    CompiledDxil(ProgramBinary const& parent, const char *name);
    void Sign();
    Metadata const& GetMetadata() const;

protected:
    Metadata m_Metadata;
    ProgramBinary const& m_Parent;
};

struct WorkProperties {
    /* Returned from get_global_offset(), and added into get_global_id() */
    unsigned global_offset_x;
    unsigned global_offset_y;
    unsigned global_offset_z;
    /* Returned from get_work_dim() */
    unsigned work_dim;
    /* The number of work groups being launched (i.e. the parameters to Dispatch).
     * If the requested global size doesn't fit in a single Dispatch, these values should
     * indicate the total number of groups that *should* have been launched. */
    unsigned group_count_total_x;
    unsigned group_count_total_y;
    unsigned group_count_total_z;
    unsigned padding;
    /* If the requested global size doesn't fit in a single Dispatch, subsequent dispatches
     * should fill out these offsets to indicate how many groups have already been launched */
    unsigned group_id_offset_x;
    unsigned group_id_offset_y;
    unsigned group_id_offset_z;
};

class ShaderCache;
class Compiler
{
public:
    struct CompileArgs
    {
        struct Header
        {
            const char *name;
            const char *contents;
        };
        std::vector<Header> headers;
        const char *program_source;
        struct Features
        {
            bool fp16;
            bool fp64;
            bool int64;
            bool images;
            bool images_read_write;
            bool images_write_3d;
            bool intel_subgroups;
            bool subgroups;
        } features;
        std::vector<const char*> cmdline_args;
    };

    struct LinkerArgs
    {
        std::vector<ProgramBinary const*> objs;
        bool create_library;
    };

    static std::unique_ptr<Compiler> GetV2();

    virtual ~Compiler() = default;

    // Ensure libclc is loaded and ready to go
    virtual bool Initialize(ShaderCache &cache) = 0;

    // Compile OpenCL C into SPIR-V
    virtual std::unique_ptr<ProgramBinary> Compile(CompileArgs const& args, Logger const& logger) const = 0;

    // Link multiple SPIR-V binaries into one, and remove linkage info
    virtual std::unique_ptr<ProgramBinary> Link(LinkerArgs const& args, Logger const& logger) const = 0;

    // Load a SPIR-V binary from a memory blob
    virtual std::unique_ptr<ProgramBinary> Load(const void *data, size_t size) const = 0;

    // Given a SPIR-V binay, return a new SPIR-V binary that has specialization constant default values replaced with the given ones
    virtual std::unique_ptr<ProgramBinary> Specialize(ProgramBinary const& obj, ProgramBinary::SpecConstantValues const& values, Logger const& logger) const = 0;

    // Convert a kernel from SPIR-V into DXIL with configuration properties
    virtual std::unique_ptr<CompiledDxil> GetKernel(const char *name, ProgramBinary const& obj, CompiledDxil::Configuration const*, Logger const* logger) const = 0;

    // Copy the work properties into a constant buffer
    virtual std::byte* CopyWorkProperties(std::byte* WorkPropertiesBuffer, WorkProperties const& props) const = 0;
    virtual size_t GetWorkPropertiesChunkSize() const = 0;

    // Return a version that can be used for initializing a shader cache
    virtual uint64_t GetVersionForCache() const = 0;
};
