#pragma once

#include "context.hpp"

class MapTask;

class Resource : public CLChildBase<Resource, Context, cl_mem>
{
public:
    using UnderlyingResource = D3D12TranslationLayer::Resource;
    using UnderlyingResourcePtr = D3D12TranslationLayer::unique_comptr<UnderlyingResource>;

    const cl_mem_flags m_Flags;
    void* const m_pHostPointer;
    const ref_ptr_int m_ParentBuffer;
    const size_t m_Offset = 0;
    const cl_image_format m_Format = {};
    const cl_image_desc m_Desc;

    static Resource* CreateBuffer(Context& Parent, D3D12TranslationLayer::ResourceCreationArgs& Args, void* pHostPointer, cl_mem_flags flags);
    static Resource* CreateSubBuffer(Resource& ParentBuffer, const cl_buffer_region& region, cl_mem_flags flags);
    static Resource* CreateImage(Context& Parent, D3D12TranslationLayer::ResourceCreationArgs& Args, void* pHostPointer, const cl_image_format& image_format, const cl_image_desc& image_desc, cl_mem_flags flags);
    static Resource* CreateImage1DBuffer(Resource& ParentBuffer, const cl_image_format& image_format, const cl_image_desc& image_desc, cl_mem_flags flags);

    UnderlyingResource* GetUnderlyingResource() const { return m_Underlying.get(); }
    cl_uint GetMapCount() const { std::lock_guard MapLock(m_MapLock); return m_MapCount; }

    D3D12TranslationLayer::SRV& GetSRV() { return m_SRV.value(); }
    D3D12TranslationLayer::UAV& GetUAV() { return m_UAV.value(); }
    ~Resource();

    void AddMapTask(MapTask*);
    MapTask* GetMapTask(void* MapPtr);
    void RemoveMapTask(MapTask*);

protected:
    UnderlyingResourcePtr m_Underlying;
    std::optional<D3D12TranslationLayer::SRV> m_SRV;
    std::optional<D3D12TranslationLayer::UAV> m_UAV;

    mutable std::mutex m_MapLock;
    std::unordered_map<void*, std::vector<::ref_ptr_int<MapTask>>> m_OutstandingMaps;
    cl_uint m_MapCount = 0;

    Resource(Context& Parent, UnderlyingResourcePtr Underlying, void* pHostPointer, size_t size, cl_mem_flags flags);
    Resource(Resource& ParentBuffer, size_t offset, size_t size, const cl_image_format& image_format, cl_mem_object_type type, cl_mem_flags flags);
    Resource(Context& Parent, UnderlyingResourcePtr Underlying, void* pHostPointer, const cl_image_format& image_format, const cl_image_desc& image_desc, cl_mem_flags flags);

    static cl_image_desc GetBufferDesc(size_t size, cl_mem_object_type type);
};
