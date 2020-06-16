// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "context.hpp"

class MapTask;
class Task;

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
    D3D12TranslationLayer::ResourceCreationArgs m_CreationArgs;

    static Resource* CreateBuffer(Context& Parent, D3D12TranslationLayer::ResourceCreationArgs& Args, void* pHostPointer, cl_mem_flags flags);
    static Resource* CreateSubBuffer(Resource& ParentBuffer, const cl_buffer_region& region, cl_mem_flags flags);
    static Resource* CreateImage(Context& Parent, D3D12TranslationLayer::ResourceCreationArgs& Args, void* pHostPointer, const cl_image_format& image_format, const cl_image_desc& image_desc, cl_mem_flags flags);
    static Resource* CreateImage1DBuffer(Resource& ParentBuffer, const cl_image_format& image_format, const cl_image_desc& image_desc, cl_mem_flags flags);

    UnderlyingResource* GetUnderlyingResource(Device*);
    void SetActiveDevice(Device*);
    UnderlyingResource* GetActiveUnderlyingResource() const { return m_ActiveUnderlying; }
    cl_uint GetMapCount() const { std::lock_guard MapLock(m_MapLock); return m_MapCount; }
    void UploadInitialData();

    void EnqueueMigrateResource(Device* newDevice, Task* triggeringTask, cl_mem_migration_flags flags);

    D3D12TranslationLayer::SRV& GetSRV(Device*);
    D3D12TranslationLayer::UAV& GetUAV(Device*);
    ~Resource();

    void AddMapTask(MapTask*);
    MapTask* GetMapTask(void* MapPtr);
    void RemoveMapTask(MapTask*);

    void AddDestructionCallback(DestructorCallback::Fn pfn, void* pUserData);

protected:
    std::recursive_mutex m_MultiDeviceLock;
    Device *m_CurrentActiveDevice = nullptr;
    UnderlyingResource *m_ActiveUnderlying = nullptr;
    std::unordered_map<Device*, UnderlyingResourcePtr> m_UnderlyingMap;
    std::unordered_map<Device*, D3D12TranslationLayer::SRV> m_SRVs;
    std::unordered_map<Device*, D3D12TranslationLayer::UAV> m_UAVs;

    std::unique_ptr<byte[]> m_InitialData;
    D3D12TranslationLayer::D3D12_UNORDERED_ACCESS_VIEW_DESC_WRAPPER m_UAVDesc;
    D3D12_SHADER_RESOURCE_VIEW_DESC m_SRVDesc;

    mutable std::mutex m_MapLock;
    std::unordered_map<void*, std::vector<::ref_ptr_int<MapTask>>> m_OutstandingMaps;
    cl_uint m_MapCount = 0;

    mutable std::mutex m_DestructorLock;
    std::vector<DestructorCallback> m_DestructorCallbacks;

    Resource(Context& Parent, decltype(m_CreationArgs) const& CreationArgs, void* pHostPointer, size_t size, cl_mem_flags flags);
    Resource(Resource& ParentBuffer, size_t offset, size_t size, const cl_image_format& image_format, cl_mem_object_type type, cl_mem_flags flags);
    Resource(Context& Parent, decltype(m_CreationArgs) const& CreationArgs, void* pHostPointer, const cl_image_format& image_format, const cl_image_desc& image_desc, cl_mem_flags flags);

    static cl_image_desc GetBufferDesc(size_t size, cl_mem_object_type type);
};
