// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "resources.hpp"
#include "task.hpp"
#include "queue.hpp"
#include "wil/resource.h"

#include "ImmediateContext.inl"

template <typename T>
using unique_comptr = D3D12TranslationLayer::unique_comptr<T>;

class CopyCrossAdapter : public Task
{
    Resource& m_Resource;
    unique_comptr<ID3D12Resource> m_CrossAdapterBuffer;
    ImmCtx& m_ImmCtx;
    bool m_ToCrossAdapter;
public:
    CopyCrossAdapter(Context& Context, Resource& Resource, unique_comptr<ID3D12Resource> CrossAdapterBuffer, D3DDevice& Device, bool ToCrossAdapter)
        : Task(Context, Device)
        , m_Resource(Resource)
        , m_CrossAdapterBuffer(std::move(CrossAdapterBuffer))
        , m_ImmCtx(m_D3DDevice->ImmCtx())
        , m_ToCrossAdapter(ToCrossAdapter)
    {
    }

    void MigrateResources() final
    {
        if (!m_ToCrossAdapter)
        {
            m_Resource.SetActiveDevice(m_D3DDevice);
        }
    }
    void RecordImpl() final
    {
        m_ImmCtx.GetResourceStateManager().TransitionResource(
            m_Resource.GetUnderlyingResource(m_D3DDevice),
            m_ToCrossAdapter ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
        m_ImmCtx.GetResourceStateManager().ApplyAllResourceTransitions();

        ID3D12Resource* CLResource = m_Resource.GetUnderlyingResource(m_D3DDevice)->GetUnderlyingResource();
        if (m_Resource.m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
        {
            ID3D12Resource* Source = m_ToCrossAdapter ? CLResource : m_CrossAdapterBuffer.get();
            ID3D12Resource* Dest = m_ToCrossAdapter ? m_CrossAdapterBuffer.get() : CLResource;
            m_ImmCtx.GetGraphicsCommandList()->CopyBufferRegion(Dest, 0, Source, 0, m_Resource.m_Desc.image_width);
        }
        else
        {
            auto TransRes = m_Resource.GetUnderlyingResource(m_D3DDevice);
            UINT NumSubresources = TransRes->NumSubresources();
            D3D12_TEXTURE_COPY_LOCATION Buffer, Image;
            Buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Buffer.pResource = m_CrossAdapterBuffer.get();
            Image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Image.pResource = CLResource;

            auto Source = m_ToCrossAdapter ? &Image : &Buffer;
            auto Dest = m_ToCrossAdapter ? &Buffer : &Image;
            for (UINT i = 0; i < NumSubresources; ++i)
            {
                Buffer.PlacedFootprint = TransRes->GetSubresourcePlacement(i);
                Image.SubresourceIndex = i;

                m_ImmCtx.GetGraphicsCommandList()->CopyTextureRegion(Dest, 0, 0, 0, Source, nullptr);
            }
        }
        m_ImmCtx.AdditionalCommandsAdded();
    }
};

void Resource::EnqueueMigrateResource(D3DDevice* newDevice, Task* triggeringTask, cl_mem_migration_flags flags)
{
    if (m_ParentBuffer.Get())
    {
        m_ParentBuffer->EnqueueMigrateResource(newDevice, triggeringTask, flags);
        SetActiveDevice(newDevice);
        return;
    }

    if (m_CurrentActiveDevice == newDevice)
        return;

    if (!m_ActiveUnderlying ||
        (flags & CL_MIGRATE_MEM_OBJECT_CONTENT_UNDEFINED))
    {
        SetActiveDevice(newDevice);
        if ((flags & CL_MIGRATE_MEM_OBJECT_CONTENT_UNDEFINED) == 0)
        {
            UploadInitialData(triggeringTask);
        }
        return;
    }

    assert(m_CurrentActiveDevice != nullptr && triggeringTask != nullptr);
    unique_comptr<ID3D12Heap> CrossAdapterHeap;
    D3D12_HEAP_DESC HeapDesc = CD3DX12_HEAP_DESC(GetActiveUnderlyingResource()->GetResourceSize(), D3D12_HEAP_TYPE_DEFAULT, 0,
                                                 D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
    D3D12TranslationLayer::ThrowFailure(m_CurrentActiveDevice->GetDevice()->CreateHeap(&HeapDesc, IID_PPV_ARGS(&CrossAdapterHeap)));
    HANDLE SharedHandle = nullptr;
    D3D12TranslationLayer::ThrowFailure(m_CurrentActiveDevice->GetDevice()->CreateSharedHandle(
        CrossAdapterHeap.get(), nullptr, GENERIC_ALL, nullptr, &SharedHandle
    ));
    auto cleanup = wil::scope_exit([SharedHandle]()
    {
        CloseHandle(SharedHandle);
    });

    unique_comptr<ID3D12Resource> CrossAdapterResource;
    D3D12_RESOURCE_DESC ResDesc = CD3DX12_RESOURCE_DESC::Buffer(GetActiveUnderlyingResource()->GetResourceSize(),
                                                                D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    D3D12TranslationLayer::ThrowFailure(m_CurrentActiveDevice->GetDevice()->CreatePlacedResource(
        CrossAdapterHeap.get(), 0, &ResDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&CrossAdapterResource)
    ));

    std::unique_ptr<Task> CopyToCrossAdapter(new CopyCrossAdapter(
        triggeringTask->m_Parent.get(), *this, std::move(CrossAdapterResource), *m_CurrentActiveDevice, true));

    CrossAdapterHeap.reset();
    D3D12TranslationLayer::ThrowFailure(newDevice->GetDevice()->OpenSharedHandle(SharedHandle, IID_PPV_ARGS(&CrossAdapterHeap)));
    D3D12TranslationLayer::ThrowFailure(newDevice->GetDevice()->CreatePlacedResource(
        CrossAdapterHeap.get(), 0, &ResDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&CrossAdapterResource)
    ));
    std::unique_ptr<Task> CopyFromCrossAdapter(new CopyCrossAdapter(
        triggeringTask->m_Parent.get(), *this, std::move(CrossAdapterResource), *newDevice, false));

    auto Lock = g_Platform->GetTaskPoolLock();

    cl_event e = CopyToCrossAdapter.get();
    CopyFromCrossAdapter->AddDependencies(&e, 1, Lock);
    m_CurrentActiveDevice->SubmitTask(CopyToCrossAdapter.get(), Lock);
    CopyToCrossAdapter.release();

    e = CopyFromCrossAdapter.get();
    triggeringTask->AddDependencies(&e, 1, Lock);
    newDevice->SubmitTask(CopyFromCrossAdapter.get(), Lock);
    CopyFromCrossAdapter.release();

    m_CurrentActiveDevice->Flush(Lock);
}

class UploadInitialData : public Task
{
    Resource& m_Resource;
public:
    UploadInitialData(Context& Context, Resource& Resource, D3DDevice& Device)
        : Task(Context, Device)
        , m_Resource(Resource)
    {
    }

    void MigrateResources() final {}
    void RecordImpl() final
    {
        if (!m_Resource.m_InitialData)
            return;

        assert(m_Resource.m_ActiveUnderlying && m_Resource.m_CurrentActiveDevice);
        std::vector<D3D11_SUBRESOURCE_DATA> InitialData;
        D3D11_SUBRESOURCE_DATA SingleSubresourceInitialData;
        auto pData = &SingleSubresourceInitialData;
        assert(m_Resource.m_CreationArgs.m_appDesc.m_MipLevels == 1);
        if (m_Resource.m_CreationArgs.m_appDesc.m_SubresourcesPerPlane > 1)
        {
            InitialData.resize(m_Resource.m_CreationArgs.m_appDesc.m_SubresourcesPerPlane);
            pData = InitialData.data();
        }
        char* pSubresourceData = reinterpret_cast<char*>(m_Resource.m_InitialData.get());
        for (UINT i = 0; i < m_Resource.m_CreationArgs.m_appDesc.m_SubresourcesPerPlane; ++i)
        {
            pData[i].pSysMem = pSubresourceData;
            pData[i].SysMemPitch = (UINT)m_Resource.m_Desc.image_row_pitch;
            pData[i].SysMemSlicePitch = (UINT)m_Resource.m_Desc.image_slice_pitch;
            pSubresourceData += m_Resource.m_Desc.image_slice_pitch;
        }
        m_Resource.m_CurrentActiveDevice->ImmCtx().UpdateSubresources(
            m_Resource.m_ActiveUnderlying,
            m_Resource.m_ActiveUnderlying->GetFullSubresourceSubset(),
            pData,
            nullptr,
            D3D12TranslationLayer::ImmediateContext::UpdateSubresourcesFlags::ScenarioImmediateContextInternalOp);
        m_Resource.m_InitialData.reset();
    }
};

void Resource::UploadInitialData(Task* triggeringTask)
{
    if (!m_InitialData)
        return;

    std::unique_ptr<Task> UploadTask(new ::UploadInitialData(
        m_Parent.get(), *this, *m_CurrentActiveDevice));

    auto Lock = g_Platform->GetTaskPoolLock();

    cl_event e = UploadTask.get();
    triggeringTask->AddDependencies(&e, 1, Lock);
    m_CurrentActiveDevice->SubmitTask(UploadTask.get(), Lock);
    UploadTask.release();

    m_CurrentActiveDevice->Flush(Lock);
}

class MigrateMemObjects : public Task
{
    std::vector<Resource::ref_ptr_int> m_Resources;
    const cl_mem_migration_flags m_Flags;
public:
    MigrateMemObjects(Context& context, cl_command_queue queue, const cl_mem* mem_objects, cl_uint num_mem_objects, cl_mem_migration_flags flags)
        : Task(context, CL_COMMAND_MIGRATE_MEM_OBJECTS, queue)
        , m_Flags(flags)
    {
        m_Resources.resize(num_mem_objects, nullptr);
        std::transform(mem_objects, mem_objects + num_mem_objects, m_Resources.begin(),
                       [](cl_mem m) { return static_cast<Resource*>(m); });
    }

    void MigrateResources() final
    {
        for (auto& res : m_Resources)
        {
            res->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, m_Flags);
        }
    }
    void RecordImpl() final {}
};

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueMigrateMemObjects(cl_command_queue       command_queue,
    cl_uint                num_mem_objects,
    const cl_mem *         mem_objects,
    cl_mem_migration_flags flags,
    cl_uint                num_events_in_wait_list,
    const cl_event *       event_wait_list,
    cl_event *             event) CL_API_SUFFIX__VERSION_1_2
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }

    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();

    if (num_mem_objects == 0 || !mem_objects)
    {
        return ReportError("Must supply mem_objects.", CL_INVALID_VALUE);
    }

    // TODO validate flags

    try
    {
        std::unique_ptr<Task> task(new MigrateMemObjects(context, command_queue, mem_objects, num_mem_objects, flags));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
    return CL_SUCCESS;
}