// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "context.hpp"
#include <optional>

class MapTask;
class Task;

struct mesa_glinterop_export_in;
struct mesa_glinterop_export_out;

class Resource : public CLChildBase<Resource, Context, cl_mem>
{
public:
    using UnderlyingResource = D3D12TranslationLayer::Resource;
    using UnderlyingResourcePtr = D3D12TranslationLayer::unique_comptr<UnderlyingResource>;
    struct DestructorCallback
    {
        using Fn = void(CL_CALLBACK *)(cl_mem, void*);
        Fn m_pfn;
        void* m_userData;
    };

    const cl_mem_flags m_Flags;
    void* const m_pHostPointer;
    const ref_ptr_int m_ParentBuffer;
    const size_t m_Offset = 0;
    const cl_image_format m_Format = {};
    const cl_image_desc m_Desc;
    std::vector<cl_mem_properties> const m_Properties;
    D3D12TranslationLayer::ResourceCreationArgs m_CreationArgs;

    struct GLInfo
    {
        cl_gl_object_type ObjectType;
        cl_GLuint ObjectName;
        cl_GLenum TextureTarget;
        cl_GLint MipLevel;
        size_t BufferOffset;
        uint32_t BaseArray;
    };
    std::optional<GLInfo> m_GLInfo;

    static Resource* CreateBuffer(Context& Parent, D3D12TranslationLayer::ResourceCreationArgs& Args, void* pHostPointer, cl_mem_flags flags, const cl_mem_properties* properties);
    static Resource* CreateSubBuffer(Resource& ParentBuffer, const cl_buffer_region& region, cl_mem_flags flags, const cl_mem_properties *properties);
    static Resource* CreateImage(Context& Parent, D3D12TranslationLayer::ResourceCreationArgs& Args, void* pHostPointer, const cl_image_format& image_format, const cl_image_desc& image_desc, cl_mem_flags flags, const cl_mem_properties *properties);
    static Resource* CreateImage1DBuffer(Resource& ParentBuffer, const cl_image_format& image_format, const cl_image_desc& image_desc, cl_mem_flags flags, const cl_mem_properties *properties);
    static Resource *ImportGLResource(Context &Parent, cl_mem_flags flags, mesa_glinterop_export_in &in, cl_int *error);

    UnderlyingResource* GetUnderlyingResource(D3DDevice*);
    void SetActiveDevice(D3DDevice*);
    UnderlyingResource* GetActiveUnderlyingResource() const { return m_ActiveUnderlying; }
    cl_uint GetMapCount() const { std::lock_guard MapLock(m_MapLock); return m_MapCount; }

    void EnqueueMigrateResource(D3DDevice* newDevice, Task* triggeringTask, cl_mem_migration_flags flags);

    D3D12TranslationLayer::SRV& GetSRV(D3DDevice*);
    D3D12TranslationLayer::UAV& GetUAV(D3DDevice*);
    ~Resource();

    void AddMapTask(MapTask*);
    MapTask* GetMapTask(void* MapPtr);
    void RemoveMapTask(MapTask*);

    void AddDestructionCallback(DestructorCallback::Fn pfn, void* pUserData);

protected:
    std::recursive_mutex m_MultiDeviceLock;
    D3DDevice *m_CurrentActiveDevice = nullptr;
    UnderlyingResource *m_ActiveUnderlying = nullptr;
    std::unordered_map<D3DDevice*, UnderlyingResourcePtr> m_UnderlyingMap;
    std::unordered_map<D3DDevice*, D3D12TranslationLayer::SRV> m_SRVs;
    std::unordered_map<D3DDevice*, D3D12TranslationLayer::UAV> m_UAVs;

    std::unique_ptr<byte[]> m_InitialData;
    D3D12_UNORDERED_ACCESS_VIEW_DESC m_UAVDesc;
    D3D12_SHADER_RESOURCE_VIEW_DESC m_SRVDesc;

    mutable std::mutex m_MapLock;
    std::unordered_map<void*, std::vector<::ref_ptr_int<MapTask>>> m_OutstandingMaps;
    cl_uint m_MapCount = 0;

    mutable std::mutex m_DestructorLock;
    std::vector<DestructorCallback> m_DestructorCallbacks;

    Resource(Context& Parent, decltype(m_CreationArgs) const& CreationArgs, void* pHostPointer, size_t size, cl_mem_flags flags, std::optional<GLInfo> glInfo, const cl_mem_properties *properties);
    Resource(Resource& ParentBuffer, size_t offset, size_t size, const cl_image_format& image_format, cl_mem_object_type type, cl_mem_flags flags, const cl_mem_properties *properties);
    Resource(Context& Parent, decltype(m_CreationArgs) const& CreationArgs, void* pHostPointer, const cl_image_format& image_format, const cl_image_desc& image_desc, cl_mem_flags flags, std::optional<GLInfo> glInfo, const cl_mem_properties *properties);

    static cl_image_desc GetBufferDesc(size_t size, cl_mem_object_type type);
    void UploadInitialData(Task* triggeringTask);
    friend class UploadInitialData;
};
