// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

namespace D3D12TranslationLayer
{

//==================================================================================================================================
// 
//==================================================================================================================================

void ImmediateContext::SStageState::ClearState() noexcept
{
    m_CBs.Clear();
    m_SRVs.Clear();
    m_Samplers.Clear();
}

void ImmediateContext::SState::ClearState() noexcept
{
    GetStageState().ClearState();

    m_CSUAVs.Clear();
    m_pPSO = nullptr;
}

ImmediateContext::SStageState& ImmediateContext::SState::GetStageState() noexcept
{
    return m_CS;
}

//----------------------------------------------------------------------------------------------------------------------------------
ImmediateContext::ImmediateContext(UINT nodeIndex, D3D12_FEATURE_DATA_D3D12_OPTIONS& caps, 
    ID3D12Device* pDevice, ID3D12CommandQueue* pQueue, TranslationLayerCallbacks const& callbacks, CreationArgs args) noexcept(false)
    : m_nodeIndex(nodeIndex)
    , m_caps(caps)
    , m_FeatureLevel(GetHardwareFeatureLevel(pDevice))
    , m_pDevice12(pDevice)
    , m_SRVAllocator(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, true, 1 << nodeIndex)
    , m_UAVAllocator(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, true, 1 << nodeIndex)
    , m_RTVAllocator(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 64, true, 1 << nodeIndex)
    , m_DSVAllocator(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 64, true, 1 << nodeIndex)
    , m_SamplerAllocator(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 64, true, 1 << nodeIndex)
    , m_DirtyStates(e_DirtyOnFirstCommandList)
    , m_StatesToReassert(e_ReassertOnNewCommandList)
    , m_UploadBufferPool(m_BufferPoolTrimThreshold, true)
    , m_ReadbackBufferPool(m_BufferPoolTrimThreshold, true)
    , m_uStencilRef(0)
    , m_PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
    , m_PredicateValue(false)
    , m_IndexBufferFormat(DXGI_FORMAT_UNKNOWN)
    , m_uNumScissors(0)
    , m_uNumViewports(0)
    , m_ScissorRectEnable(false)
    , m_uIndexBufferOffset(0)
    , m_callbacks(callbacks)
    , m_DeferredDeletionQueueManager(this)

    , m_UploadHeapSuballocator(
        std::forward_as_tuple(cBuddyMaxBlockSize, cBuddyAllocatorThreshold, this, AllocatorHeapType::Upload),
        std::forward_as_tuple(this, AllocatorHeapType::Upload),
        ResourceNeedsOwnAllocation)

    , m_ReadbackHeapSuballocator(
        std::forward_as_tuple(cBuddyMaxBlockSize, cBuddyAllocatorThreshold, this, AllocatorHeapType::Readback),
        std::forward_as_tuple(this, AllocatorHeapType::Readback),
        ResourceNeedsOwnAllocation)

    , m_CreationArgs(args)
    , m_ResourceStateManager(*this)
    , m_bUseRingBufferDescriptorHeaps(false)
    , m_residencyManager(*this)
{
    memset(m_BlendFactor, 0, sizeof(m_BlendFactor));
    memset(m_auVertexOffsets, 0, sizeof(m_auVertexOffsets));
    memset(m_auVertexStrides, 0, sizeof(m_auVertexStrides));
    memset(m_aScissors, 0, sizeof(m_aScissors));
    memset(m_aViewports, 0, sizeof(m_aViewports));

    HRESULT hr = S_OK;

    D3D12_COMMAND_QUEUE_DESC SyncOnlyQueueDesc = { D3D12_COMMAND_LIST_TYPE_NONE };
    (void)m_pDevice12->CreateCommandQueue(&SyncOnlyQueueDesc, IID_PPV_ARGS(&m_pSyncOnlyQueue));

    LUID adapterLUID = pDevice->GetAdapterLuid();
    {
        CComPtr<IDXCoreAdapterFactory> pFactory;
#if DYNAMIC_LOAD_DXCORE
        m_DXCore.load("dxcore");
        auto pfnDXCoreCreateAdapterFactory = m_DXCore.proc_address<HRESULT(APIENTRY*)(REFIID, void**)>("DXCoreCreateAdapterFactory");
        if (m_DXCore && pfnDXCoreCreateAdapterFactory && SUCCEEDED(pfnDXCoreCreateAdapterFactory(IID_PPV_ARGS(&pFactory))))
#else
        if (SUCCEEDED(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&pFactory))))
#endif
        {
            (void)pFactory->GetAdapterByLuid(adapterLUID, IID_PPV_ARGS(&m_pDXCoreAdapter));
        }
    }

    m_residencyManager.Initialize(nodeIndex, m_pDXCoreAdapter.get());

    m_UAVDeclScratch.reserve(D3D11_1_UAV_SLOT_COUNT); // throw( bad_alloc )
    m_vUAVBarriers.reserve(D3D11_1_UAV_SLOT_COUNT); // throw( bad_alloc )

    m_ViewHeap.m_MaxHeapSize = (DWORD)D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1;
    const UINT32 viewHeapStartingCount = m_bUseRingBufferDescriptorHeaps ? 4096 : m_ViewHeap.m_MaxHeapSize;
    m_ViewHeap.m_DescriptorRingBuffer = CFencedRingBuffer(viewHeapStartingCount);
    m_ViewHeap.m_Desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    m_ViewHeap.m_Desc.NumDescriptors = viewHeapStartingCount;
    m_ViewHeap.m_Desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    m_ViewHeap.m_Desc.NodeMask = GetNodeMask();

    m_SamplerHeap.m_MaxHeapSize = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
    const UINT32 samplerHeapStartingCount = m_bUseRingBufferDescriptorHeaps ? 512 : m_SamplerHeap.m_MaxHeapSize;
    m_SamplerHeap.m_DescriptorRingBuffer = CFencedRingBuffer(samplerHeapStartingCount);
    m_SamplerHeap.m_Desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    m_SamplerHeap.m_Desc.NumDescriptors = samplerHeapStartingCount;
    m_SamplerHeap.m_Desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    m_SamplerHeap.m_Desc.NodeMask = GetNodeMask();

    // Create initial objects
    hr = m_pDevice12->CreateDescriptorHeap(&m_ViewHeap.m_Desc, IID_PPV_ARGS(&m_ViewHeap.m_pDescriptorHeap));
    ThrowFailure(hr); //throw( _com_error )
    m_ViewHeap.m_DescriptorSize = m_pDevice12->GetDescriptorHandleIncrementSize(m_ViewHeap.m_Desc.Type);
    m_ViewHeap.m_DescriptorHeapBase = m_ViewHeap.m_pDescriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr;
    m_ViewHeap.m_DescriptorHeapBaseCPU = m_ViewHeap.m_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    m_ViewHeap.m_BitsToSetOnNewHeap = e_ViewsDirty;

    if (!ComputeOnly())
    {
        hr = m_pDevice12->CreateDescriptorHeap(&m_SamplerHeap.m_Desc, IID_PPV_ARGS(&m_SamplerHeap.m_pDescriptorHeap));
        ThrowFailure(hr); //throw( _com_error )
        m_SamplerHeap.m_DescriptorSize = m_pDevice12->GetDescriptorHandleIncrementSize(m_SamplerHeap.m_Desc.Type);
        m_SamplerHeap.m_DescriptorHeapBase = m_SamplerHeap.m_pDescriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr;
        m_SamplerHeap.m_DescriptorHeapBaseCPU = m_SamplerHeap.m_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        m_SamplerHeap.m_BitsToSetOnNewHeap = e_SamplersDirty;
    }

    for (UINT i = 0; i <= (UINT)RESOURCE_DIMENSION::TEXTURECUBEARRAY; ++i)
    {
        auto ResourceDimension = ComputeOnly() ? RESOURCE_DIMENSION::BUFFER : (RESOURCE_DIMENSION)i;
        D3D12_SHADER_RESOURCE_VIEW_DESC NullSRVDesc = {};
        D3D12_UNORDERED_ACCESS_VIEW_DESC NullUAVDesc = {};
        NullSRVDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        NullSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        NullUAVDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        switch (ResourceDimension)
        {
            case RESOURCE_DIMENSION::BUFFER:
            case RESOURCE_DIMENSION::UNKNOWN:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                NullSRVDesc.Buffer.FirstElement = 0;
                NullSRVDesc.Buffer.NumElements = 0;
                NullSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
                NullSRVDesc.Buffer.StructureByteStride = 0;
                NullUAVDesc.Buffer.FirstElement = 0;
                NullUAVDesc.Buffer.NumElements = 0;
                NullUAVDesc.Buffer.StructureByteStride = 0;
                NullUAVDesc.Buffer.CounterOffsetInBytes = 0;
                NullUAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
                if (ComputeOnly())
                {
                    // Compute only will use a raw view instead of typed
                    NullSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                    NullUAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                    NullSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                    NullUAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                }
                break;
            case RESOURCE_DIMENSION::TEXTURE1D:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                NullSRVDesc.Texture1D.MipLevels = 1;
                NullSRVDesc.Texture1D.MostDetailedMip = 0;
                NullSRVDesc.Texture1D.ResourceMinLODClamp = 0.0f;
                NullUAVDesc.Texture1D.MipSlice = 0;
                break;
            case RESOURCE_DIMENSION::TEXTURE1DARRAY:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                NullSRVDesc.Texture1DArray.MipLevels = 1;
                NullSRVDesc.Texture1DArray.ArraySize = 1;
                NullSRVDesc.Texture1DArray.MostDetailedMip = 0;
                NullSRVDesc.Texture1DArray.FirstArraySlice = 0;
                NullSRVDesc.Texture1DArray.ResourceMinLODClamp = 0.0f;
                NullUAVDesc.Texture1DArray.ArraySize = 1;
                NullUAVDesc.Texture1DArray.MipSlice = 0;
                NullUAVDesc.Texture1DArray.FirstArraySlice = 0;
                break;
            case RESOURCE_DIMENSION::TEXTURE2D:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                NullSRVDesc.Texture2D.MipLevels = 1;
                NullSRVDesc.Texture2D.MostDetailedMip = 0;
                NullSRVDesc.Texture2D.PlaneSlice = 0;
                NullSRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
                NullUAVDesc.Texture2D.MipSlice = 0;                
                NullUAVDesc.Texture2D.PlaneSlice = 0;
                break;
            case RESOURCE_DIMENSION::TEXTURE2DARRAY:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                NullSRVDesc.Texture2DArray.MipLevels = 1;
                NullSRVDesc.Texture2DArray.ArraySize = 1;
                NullSRVDesc.Texture2DArray.MostDetailedMip = 0;
                NullSRVDesc.Texture2DArray.FirstArraySlice = 0;
                NullSRVDesc.Texture2DArray.PlaneSlice = 0;
                NullSRVDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
                NullUAVDesc.Texture2DArray.ArraySize = 1;
                NullUAVDesc.Texture2DArray.MipSlice = 0;
                NullUAVDesc.Texture2DArray.FirstArraySlice = 0;
                NullUAVDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case RESOURCE_DIMENSION::TEXTURE2DMS:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_UNKNOWN;
                break;
            case RESOURCE_DIMENSION::TEXTURE2DMSARRAY:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_UNKNOWN;
                NullSRVDesc.Texture2DMSArray.ArraySize = 1;
                NullSRVDesc.Texture2DMSArray.FirstArraySlice = 0;
                break;
            case RESOURCE_DIMENSION::TEXTURE3D:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                NullSRVDesc.Texture3D.MipLevels = 1;
                NullSRVDesc.Texture3D.MostDetailedMip = 0;
                NullSRVDesc.Texture3D.ResourceMinLODClamp = 0.0f;
                NullUAVDesc.Texture3D.WSize = 1;
                NullUAVDesc.Texture3D.MipSlice = 0;
                NullUAVDesc.Texture3D.FirstWSlice = 0;
                break;
            case RESOURCE_DIMENSION::TEXTURECUBE:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_UNKNOWN;
                NullSRVDesc.TextureCube.MipLevels = 1;
                NullSRVDesc.TextureCube.MostDetailedMip = 0;
                NullSRVDesc.TextureCube.ResourceMinLODClamp = 0.0f;
                break;
            case RESOURCE_DIMENSION::TEXTURECUBEARRAY:
                NullSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                NullUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_UNKNOWN;
                NullSRVDesc.TextureCubeArray.MipLevels = 1;
                NullSRVDesc.TextureCubeArray.NumCubes = 1;
                NullSRVDesc.TextureCubeArray.MostDetailedMip = 0;
                NullSRVDesc.TextureCubeArray.First2DArrayFace = 0;
                NullSRVDesc.TextureCubeArray.ResourceMinLODClamp = 0.0f;
                break;
        }

        if (NullSRVDesc.ViewDimension != D3D12_SRV_DIMENSION_UNKNOWN)
        {
            m_NullSRVs[i] = m_SRVAllocator.AllocateHeapSlot(); // throw( _com_error )
            m_pDevice12->CreateShaderResourceView(nullptr, &NullSRVDesc, m_NullSRVs[i]);
        }

        if (NullUAVDesc.ViewDimension != D3D12_UAV_DIMENSION_UNKNOWN)
        {
            m_NullUAVs[i] = m_UAVAllocator.AllocateHeapSlot(); // throw( _com_error )
            m_pDevice12->CreateUnorderedAccessView(nullptr, nullptr, &NullUAVDesc, m_NullUAVs[i]);
        }
    }
    if (!ComputeOnly())
    {
        m_NullRTV = m_RTVAllocator.AllocateHeapSlot(); // throw( _com_error )
        D3D12_RENDER_TARGET_VIEW_DESC NullRTVDesc;
        NullRTVDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        NullRTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        NullRTVDesc.Texture2D.MipSlice = 0;
        NullRTVDesc.Texture2D.PlaneSlice = 0;
        m_pDevice12->CreateRenderTargetView(nullptr, &NullRTVDesc, m_NullRTV);
    }

    if (!ComputeOnly())
    {
        m_NullSampler = m_SamplerAllocator.AllocateHeapSlot(); // throw( _com_error )
        // Arbitrary parameters used, this sampler should never actually be used
        D3D12_SAMPLER_DESC NullSamplerDesc;
        NullSamplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
        NullSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        NullSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        NullSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        NullSamplerDesc.MipLODBias = 0.0f;
        NullSamplerDesc.MaxAnisotropy = 0;
        NullSamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        NullSamplerDesc.MinLOD = 0.0f;
        NullSamplerDesc.MaxLOD = 0.0f;
        memset(NullSamplerDesc.BorderColor, 0, sizeof(NullSamplerDesc.BorderColor));
        m_pDevice12->CreateSampler(&NullSamplerDesc, m_NullSampler);
    }

    (void)m_pDevice12->QueryInterface(&m_pDevice12_1);
    (void)m_pDevice12->QueryInterface(&m_pDevice12_2);
    m_pDevice12->QueryInterface(&m_pCompatDevice);

    m_CommandList.reset(new CommandListManager(this, pQueue)); // throw( bad_alloc )
    m_CommandList->InitCommandList();
}

bool ImmediateContext::Shutdown() noexcept
{
    if (m_CommandList)
    {
        // The device is being destroyed so no point executing any authored work
        m_CommandList->DiscardCommandList();

        // Make sure any GPU work still in the pipe is finished
        try
        {
            if (!m_CommandList->WaitForCompletion()) // throws
            {
                return false;
            }
        }
        catch (_com_error&)
        {
            return false;
        }
        catch (std::bad_alloc&)
        {
            return false;
        }
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------------------
ImmediateContext::~ImmediateContext() noexcept
{
    Shutdown();

    //Ensure all remaining allocations are cleaned up
    TrimDeletedObjects(true);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::AddResourceToDeferredDeletionQueue(ID3D12Object* pUnderlying, std::unique_ptr<ResidencyManagedObjectWrapper> &&pResidencyHandle, UINT64 lastCommandListID)
{
    // Note: Due to the below routines being called after deferred deletion queue destruction,
    // all callers of the generic AddObjectToQueue should ensure that the object really needs to be in the queue.
    if (!RetiredD3D12Object::ReadyToDestroy(this, lastCommandListID))
    {
        m_DeferredDeletionQueueManager.GetLocked()->AddObjectToQueue(pUnderlying, std::move(pResidencyHandle), lastCommandListID);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::AddObjectToDeferredDeletionQueue(ID3D12Object* pUnderlying, UINT64 lastCommandListID)
{
    // Note: May be called after the deferred deletion queue has been destroyed, but in all such cases,
    // the ReadyToDestroy function will return true.
    if (!RetiredD3D12Object::ReadyToDestroy(this, lastCommandListID))
    {
        std::unique_ptr<ResidencyManagedObjectWrapper> nullUniquePtr;
        m_DeferredDeletionQueueManager.GetLocked()->AddObjectToQueue(pUnderlying, std::move(nullUniquePtr), lastCommandListID);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
bool DeferredDeletionQueueManager::TrimDeletedObjects(bool deviceBeingDestroyed)
{
    bool AnyObjectsDestroyed = false;
    while (m_DeferredObjectDeletionQueue.empty() == false &&
        (m_DeferredObjectDeletionQueue.front().ReadyToDestroy(m_pParent) || deviceBeingDestroyed))
    {
        AnyObjectsDestroyed = true;
        m_DeferredObjectDeletionQueue.pop();
    }

    while (SuballocationsReadyToBeDestroyed(deviceBeingDestroyed))
    {
        AnyObjectsDestroyed = true;
        m_DeferredSuballocationDeletionQueue.front().Destroy();
        m_DeferredSuballocationDeletionQueue.pop();
    }

    return AnyObjectsDestroyed;
}

//----------------------------------------------------------------------------------------------------------------------------------
UINT64 DeferredDeletionQueueManager::GetFenceValueForObjectDeletion()
{
    if (!m_DeferredObjectDeletionQueue.empty())
    {
        return m_DeferredObjectDeletionQueue.front().m_lastCommandListID;
    }
    return ~0ull;
}

UINT64 DeferredDeletionQueueManager::GetFenceValueForSuballocationDeletion()
{
    if (!m_DeferredSuballocationDeletionQueue.empty())
    {
        return m_DeferredSuballocationDeletionQueue.front().m_lastCommandListID;
    }
    return ~0ull;
}

bool DeferredDeletionQueueManager::SuballocationsReadyToBeDestroyed(bool deviceBeingDestroyed)
{
    return m_DeferredSuballocationDeletionQueue.empty() == false &&
        (m_DeferredSuballocationDeletionQueue.front().ReadyToDestroy(m_pParent) || deviceBeingDestroyed);
}

bool ImmediateContext::TrimDeletedObjects(bool deviceBeingDestroyed)
{
    return m_DeferredDeletionQueueManager.GetLocked()->TrimDeletedObjects(deviceBeingDestroyed);
}

bool ImmediateContext::TrimResourcePools()
{
    m_UploadBufferPool.Trim(GetCompletedFenceValue());
    m_ReadbackBufferPool.Trim(GetCompletedFenceValue());

    return true;
}

void ImmediateContext::PostSubmitNotification()
{
    if (m_callbacks.m_pfnPostSubmit)
    {
        m_callbacks.m_pfnPostSubmit();
    }
    TrimDeletedObjects();
    TrimResourcePools();

    const UINT64 completedFence = GetCompletedFenceValue();

    if (m_bUseRingBufferDescriptorHeaps)
    {
        m_ViewHeap.m_DescriptorRingBuffer.Deallocate(completedFence);
        m_SamplerHeap.m_DescriptorRingBuffer.Deallocate(completedFence);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::RollOverHeap(OnlineDescriptorHeap& Heap) noexcept(false)
{
    auto pfnCreateNew = [this](D3D12_DESCRIPTOR_HEAP_DESC const& Desc) -> unique_comptr<ID3D12DescriptorHeap>
    {
        unique_comptr<ID3D12DescriptorHeap> spHeap;
        ThrowFailure(m_pDevice12->CreateDescriptorHeap(&Desc, IID_PPV_ARGS(&spHeap)));
        return std::move(spHeap);
    };

    // If we are in the growth phase don't bother using pools
    if (Heap.m_Desc.NumDescriptors < Heap.m_MaxHeapSize)
    {
        // Defer delete the current heap
        AddObjectToDeferredDeletionQueue(Heap.m_pDescriptorHeap.get(), GetCommandListID());

        // Grow
        Heap.m_Desc.NumDescriptors *= 2;
        Heap.m_Desc.NumDescriptors = min(Heap.m_Desc.NumDescriptors, Heap.m_MaxHeapSize);

        Heap.m_pDescriptorHeap = TryAllocateResourceWithFallback([&]() { return pfnCreateNew(Heap.m_Desc); },
            ResourceAllocationContext::ImmediateContextThreadLongLived);
    }
    else
    {
        // If we reach this point they are really heavy heap users so we can fall back the roll over strategy
        Heap.m_HeapPool.ReturnToPool(std::move(Heap.m_pDescriptorHeap), GetCommandListID());

        UINT64 CurrentFenceValue = GetCompletedFenceValue();
        Heap.m_pDescriptorHeap = TryAllocateResourceWithFallback([&]() {
            return Heap.m_HeapPool.RetrieveFromPool(CurrentFenceValue, pfnCreateNew, Heap.m_Desc); // throw( _com_error )
            }, ResourceAllocationContext::ImmediateContextThreadLongLived);
    }

    Heap.m_DescriptorRingBuffer = CFencedRingBuffer(Heap.m_Desc.NumDescriptors);
    Heap.m_DescriptorHeapBase = Heap.m_pDescriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr;
    Heap.m_DescriptorHeapBaseCPU = Heap.m_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr;

    ID3D12DescriptorHeap* pHeaps[2] = {m_ViewHeap.m_pDescriptorHeap.get(), m_SamplerHeap.m_pDescriptorHeap.get()};
    GetGraphicsCommandList()->SetDescriptorHeaps(ComputeOnly() ? 1 : 2, pHeaps);

    m_DirtyStates |= Heap.m_BitsToSetOnNewHeap;
}

//----------------------------------------------------------------------------------------------------------------------------------
UINT ImmediateContext::ReserveSlots(OnlineDescriptorHeap& Heap, UINT NumSlots) noexcept(false)
{
    assert(NumSlots <= Heap.m_Desc.NumDescriptors);

    UINT offset = 0;
    HRESULT hr = S_OK;

    do
    {
        hr = Heap.m_DescriptorRingBuffer.Allocate(NumSlots, GetCommandListID(), offset);

        if (FAILED(hr))
        {
            RollOverHeap(Heap);
        }

    } while (FAILED(hr));

    assert(offset < Heap.m_Desc.NumDescriptors);
    assert(offset + NumSlots <= Heap.m_Desc.NumDescriptors);

    return offset;
}

//----------------------------------------------------------------------------------------------------------------------------------
UINT ImmediateContext::ReserveSlotsForBindings(OnlineDescriptorHeap& Heap, UINT (ImmediateContext::*pfnCalcRequiredSlots)()) noexcept(false)
{
    UINT NumSlots = (this->*pfnCalcRequiredSlots)();
    
    assert(NumSlots <= Heap.m_Desc.NumDescriptors);

    UINT offset = 0;
    HRESULT hr = S_OK;

    do
    {
        hr = Heap.m_DescriptorRingBuffer.Allocate(NumSlots, GetCommandListID(), offset);

        if (FAILED(hr))
        {
            RollOverHeap(Heap);
            NumSlots = (this->*pfnCalcRequiredSlots)();
        }

    } while (FAILED(hr));

    assert(offset < Heap.m_Desc.NumDescriptors);
    assert(offset + NumSlots <= Heap.m_Desc.NumDescriptors);

    return offset;
}

//----------------------------------------------------------------------------------------------------------------------------------
RootSignature* ImmediateContext::CreateOrRetrieveRootSignature(RootSignatureDesc const& desc) noexcept(false)
{
    auto& result = m_RootSignatures[desc];
    if (!result)
    {
        result.reset(new RootSignature(this, desc));
    }
    return result.get();
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::PostDispatch()
{
    m_CommandList->DispatchCommandAdded();
}

D3D12_BOX ImmediateContext::GetSubresourceBoxFromBox(Resource *pSrc, UINT RequestedSubresource, UINT BaseSubresource, D3D12_BOX const& SrcBox)
{
    assert(BaseSubresource <= RequestedSubresource);
    D3D12_BOX box = SrcBox;

    // This method should not be used with empty rects, as it ensures its output rect is not empty.
    assert(box.left < box.right && box.top < box.bottom && box.front < box.back);

    // RequestedSubresource is a D3D12 subresource, before calling into 11 it needs to be converted back
    UINT ApiRequestedSubresource = RequestedSubresource;
    UINT ApiBaseSubresource = BaseSubresource;
    if (pSrc->SubresourceMultiplier() > 1)
    {
        assert(pSrc->AppDesc()->NonOpaquePlaneCount() == 1);
        ApiRequestedSubresource = ConvertSubresourceIndexRemovePlane(RequestedSubresource, pSrc->AppDesc()->SubresourcesPerPlane());
        ApiBaseSubresource = ConvertSubresourceIndexRemovePlane(BaseSubresource, pSrc->AppDesc()->SubresourcesPerPlane());
    }

    {
        auto& footprint = pSrc->GetSubresourcePlacement(RequestedSubresource).Footprint;
        // Planar textures do not support mipmaps, and in this case coordinates should
        // not be divided by width / height alignment.
        if (pSrc->AppDesc()->NonOpaquePlaneCount() > 1)
        {
            UINT PlaneIndex = GetPlaneIdxFromSubresourceIdx(ApiRequestedSubresource, pSrc->AppDesc()->SubresourcesPerPlane());
            UINT BasePlaneIndex = GetPlaneIdxFromSubresourceIdx(ApiBaseSubresource, pSrc->AppDesc()->SubresourcesPerPlane());

            if (PlaneIndex > 0 && BasePlaneIndex == 0)
            {
                // Adjust for subsampling.
                UINT subsampleX, subsampleY;
                CD3D11FormatHelper::GetYCbCrChromaSubsampling(pSrc->AppDesc()->Format(), subsampleX, subsampleY);
                // Round up on the right bounds to prevent empty rects.
                box.right =  min(footprint.Width,  (box.right  + (subsampleX - 1)) / subsampleX);
                box.left =   min(box.right,         box.left                       / subsampleX);
                box.bottom = min(footprint.Height, (box.bottom + (subsampleY - 1)) / subsampleY);
                box.top =    min(box.bottom,        box.top                        / subsampleY);
            }
            else
            {
                // Make sure the box is at least contained within the subresource.
                box.right =  min(footprint.Width,  box.right);
                box.left =   min(box.right,        box.left);
                box.bottom = min(footprint.Height, box.bottom);
                box.top =    min(box.bottom,       box.top);
            }
        }
        else
        {
            // Get the mip level of the subresource
            const UINT mipLevel = DecomposeSubresourceIdxExtendedGetMip(ApiRequestedSubresource, pSrc->AppDesc()->MipLevels());
            const UINT baseMipLevel = DecomposeSubresourceIdxExtendedGetMip(ApiBaseSubresource, pSrc->AppDesc()->MipLevels());
            const UINT mipTransform = mipLevel - baseMipLevel;
            static_assert(D3D12_REQ_MIP_LEVELS < 32, "Bitshifting by number of mips should be fine for a UINT.");

            const UINT WidthAlignment = CD3D11FormatHelper::GetWidthAlignment(pSrc->AppDesc()->Format());
            const UINT HeightAlignment = CD3D11FormatHelper::GetHeightAlignment(pSrc->AppDesc()->Format());
            const UINT DepthAlignment = CD3D11FormatHelper::GetDepthAlignment(pSrc->AppDesc()->Format());

            // AlignAtLeast is chosen for right bounds to prevent bitshifting from resulting in empty rects.
            box.right =  min(footprint.Width,  AlignAtLeast(box.right  >> mipTransform, WidthAlignment));
            box.left =   min(box.right,        Align(       box.left   >> mipTransform, WidthAlignment));
            box.bottom = min(footprint.Height, AlignAtLeast(box.bottom >> mipTransform, HeightAlignment));
            box.top =    min(box.bottom,       Align(       box.top    >> mipTransform, HeightAlignment));
            box.back =   min(footprint.Depth,  AlignAtLeast(box.back   >> mipTransform, DepthAlignment));
            box.front =  min(box.back,         Align(       box.front  >> mipTransform, DepthAlignment));
        }
    }

    // This method should not generate empty rects.
    assert(box.left < box.right && box.top < box.bottom && box.front < box.back);
    return box;
}

D3D12_BOX ImmediateContext::GetBoxFromResource(Resource *pSrc, UINT SrcSubresource)
{
    return GetSubresourceBoxFromBox(pSrc, SrcSubresource, 0, CD3DX12_BOX(0, 0, 0, pSrc->AppDesc()->Width(), pSrc->AppDesc()->Height(), pSrc->AppDesc()->Depth()));
}

// Handles copies that are either:
// * A copy to/from the same subresource but at different offsets
// * A copy to/from suballocated resources that are both from the same underlying heap
void ImmediateContext::SameResourceCopy(Resource *pDst, UINT DstSubresource, Resource *pSrc, UINT SrcSubresource, UINT dstX, UINT dstY, UINT dstZ, const D3D12_BOX *pSrcBox)
{
    D3D12_BOX PatchedBox = {};
    if (!pSrcBox)
    {
        PatchedBox = GetBoxFromResource(pSrc, SrcSubresource);
        pSrcBox = &PatchedBox;
    }

    const bool bIsBoxEmpty = (pSrcBox->left >= pSrcBox->right || pSrcBox->top >= pSrcBox->bottom || pSrcBox->front >= pSrcBox->back);
    if (bIsBoxEmpty)
    {
        return;
    }

    // TODO: Profile the best strategy for handling same resource copies based on perf from games running on 9on12/11on12.
    // The default strategy is keep a per-context buffer that we re-use whenever we need to handle copies that require an intermediate 
    // buffer. The trade-off is that the GPU needs to swizzle-deswizzle when copying in and out of the resource. The alternative strategy is 
    // to instead allocate a resource everytime a same-resource copy is done but the intermediate resource will match the src/dst 
    // resource, avoiding any need to swizzle/deswizzle.
    // Task captured in VSO #7121286

    ResourceCreationArgs StagingResourceCreateArgs = {};
    D3D12_RESOURCE_DESC &StagingDesc = StagingResourceCreateArgs.m_desc12;
    StagingDesc = pSrc->Parent()->m_desc12;
    bool bUseBufferCopy = StagingDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;

    // Use pSrcBox to determine the minimal size we need to allocate for the staging resource
    UINT StagingHeight = StagingDesc.Height = pSrcBox->bottom - pSrcBox->top;
    StagingDesc.Width = pSrcBox->right - pSrcBox->left;
    UINT StagingWidth = static_cast<UINT>(StagingDesc.Width);
    UINT StagingDepth = StagingDesc.DepthOrArraySize = static_cast<UINT16>(pSrcBox->back - pSrcBox->front);

    // Query the footprint, this is necessary for resources that have varying formats per subresource (planar formats such as NV12)
    DXGI_FORMAT StagingFormat = StagingDesc.Format = bUseBufferCopy ? DXGI_FORMAT_UNKNOWN : pSrc->GetSubresourcePlacement(SrcSubresource).Footprint.Format;

    StagingDesc.MipLevels = 1;
    StagingDesc.Flags = (D3D12_RESOURCE_FLAGS)0;
    StagingResourceCreateArgs.m_appDesc = AppResourceDesc(0, 1, 1, 1, 1, StagingDepth, StagingWidth, StagingHeight, StagingFormat, StagingDesc.SampleDesc.Count,
        StagingDesc.SampleDesc.Quality, RESOURCE_USAGE_DEFAULT, (RESOURCE_CPU_ACCESS)0, (RESOURCE_BIND_FLAGS)0, StagingDesc.Dimension);

    UINT64 resourceSize = 0;
    m_pDevice12->GetCopyableFootprints(&StagingDesc, 0, 1, 0, nullptr, nullptr, nullptr, &resourceSize);
    bool bReallocateStagingBuffer = false;

    auto &pStagingResource = bUseBufferCopy ? m_pStagingBuffer : m_pStagingTexture;
    StagingDesc = CD3DX12_RESOURCE_DESC::Buffer(resourceSize);
    StagingResourceCreateArgs.m_isPlacedTexture = !bUseBufferCopy;
    if (!pStagingResource || pStagingResource->Parent()->m_heapDesc.SizeInBytes < resourceSize)
    {
        bReallocateStagingBuffer = true;
    }

    if (bReallocateStagingBuffer)
    {
        StagingResourceCreateArgs.m_heapDesc = CD3DX12_HEAP_DESC(resourceSize, GetHeapProperties(D3D12_HEAP_TYPE_DEFAULT));

        pStagingResource = Resource::CreateResource(this, StagingResourceCreateArgs, ResourceAllocationContext::ImmediateContextThreadLongLived);
    }
    else
    {
        pStagingResource->UpdateAppDesc(StagingResourceCreateArgs.m_appDesc);
    }
    assert(pStagingResource);

    const D3D12_BOX StagingSrcBox = CD3DX12_BOX(0, 0, 0, pSrcBox->right - pSrcBox->left, pSrcBox->bottom - pSrcBox->top, pSrcBox->back - pSrcBox->front);

    // Pick just one of the resources to call transitions on (don't need to transition both the src and dst since the underlying resource is the same),
    // we pick pDst since PostCopy will revert it back to COPY_SOURCE if it's an upload heap
    const UINT TransitionSubresource = DstSubresource;
    Resource *pTransitionResource = pDst;

    m_ResourceStateManager.TransitionResource(pStagingResource.get(), D3D12_RESOURCE_STATE_COPY_DEST);
    m_ResourceStateManager.TransitionSubresource(pTransitionResource, TransitionSubresource, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_ResourceStateManager.ApplyAllResourceTransitions();
    CopyAndConvertSubresourceRegion(pStagingResource.get(), 0, pSrc, SrcSubresource, 0, 0, 0, reinterpret_cast<const D3D12_BOX*>(pSrcBox));

    m_ResourceStateManager.TransitionResource(pStagingResource.get(), D3D12_RESOURCE_STATE_GENERIC_READ);
    m_ResourceStateManager.TransitionSubresource(pTransitionResource, TransitionSubresource, D3D12_RESOURCE_STATE_COPY_DEST);
    m_ResourceStateManager.ApplyAllResourceTransitions();
    CopyAndConvertSubresourceRegion(pDst, DstSubresource, pStagingResource.get(), 0, dstX, dstY, dstZ, reinterpret_cast<const D3D12_BOX*>(&StagingSrcBox));
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::PostCopy(Resource *pSrc, UINT srcSubresource, Resource *pDst, UINT dstSubresource, UINT totalNumSubresources)
{
    bool bUsesSameUnderlyingSubresource = Resource::IsSameUnderlyingSubresource(pSrc, srcSubresource, pDst, dstSubresource);
#if DBG
    for (UINT i = 1; i < totalNumSubresources; i++)
    {
        assert(bUsesSameUnderlyingSubresource == Resource::IsSameUnderlyingSubresource(pSrc, srcSubresource + i, pDst, dstSubresource + i));
    }
#else
    UNREFERENCED_PARAMETER(totalNumSubresources);
#endif


    // Revert suballocated resource's owning heap back to the default state
    bool bResourceTransitioned = false;

    if (pSrc && !pSrc->GetIdentity()->m_bOwnsUnderlyingResource && pSrc->GetAllocatorHeapType() == AllocatorHeapType::Readback &&
        !bUsesSameUnderlyingSubresource) // Will automatically be transitioned back to COPY_DEST if this is part of a same resource copy
    {
        m_ResourceStateManager.TransitionResource(pSrc, GetDefaultPoolState(pSrc->GetAllocatorHeapType()));
        bResourceTransitioned = true;
    }

    if (pDst && !pDst->GetIdentity()->m_bOwnsUnderlyingResource && pDst->GetAllocatorHeapType() == AllocatorHeapType::Upload)
    {
        m_ResourceStateManager.TransitionResource(pDst, GetDefaultPoolState(pDst->GetAllocatorHeapType()));
        bResourceTransitioned = true;
    }

    if (bResourceTransitioned)
    {
        m_ResourceStateManager.ApplyAllResourceTransitions();
    }
    AdditionalCommandsAdded();
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::AddObjectToResidencySet(Resource *pResource)
{
    m_CommandList->AddResourceToResidencySet(pResource);
}

//----------------------------------------------------------------------------------------------------------------------------------
bool ImmediateContext::Flush()
{
    bool bSubmitCommandList = false;
    m_ResourceStateManager.ApplyAllResourceTransitions();

    if (m_CommandList && m_CommandList->HasCommands())
    {
        m_CommandList->SubmitCommandList();
        bSubmitCommandList = true;
    }

    // Even if there are no commands, the app could have still done things like delete resources,
    // these are expected to be cleaned up on a per-flush basis
    PostSubmitNotification();
    return bSubmitCommandList;
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::PrepForCommandQueueSync()
{
    Flush();
    if (m_CommandList)
    {
        assert(!m_CommandList->HasCommands());
        m_CommandList->PrepForCommandQueueSync();
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CsSetUnorderedAccessViews(UINT Start, __in_range(0, D3D11_1_UAV_SLOT_COUNT) UINT NumViews, __in_ecount(NumViews) UAV* const* ppUAVs, __in_ecount(NumViews) CONST UINT* pInitialCounts)
{
    for (UINT i = 0; i < NumViews; ++i)
    {
        UINT slot = i + Start;
        UAV* pUAV = ppUAVs[i];

        // Ensure a counter resource is allocated for the UAV if necessary
        m_CurrentState.m_CSUAVs.UpdateBinding(slot, pUAV);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::UAVBarrier() noexcept
{
    D3D12_RESOURCE_BARRIER BarrierDesc;
    ZeroMemory(&BarrierDesc, sizeof(BarrierDesc));
    BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;

    GetGraphicsCommandList()->ResourceBarrier(1, &BarrierDesc);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CopyAndConvertSubresourceRegion(Resource* pDst, UINT DstSubresource, Resource* pSrc, UINT SrcSubresource, UINT dstX, UINT dstY, UINT dstZ, const D3D12_BOX* pSrcBox) noexcept
{
    assert(!Resource::IsSameUnderlyingSubresource(pSrc, SrcSubresource, pDst, DstSubresource));

    struct CopyDesc
    {
        Resource* pResource;
        D3D12_TEXTURE_COPY_LOCATION View;
    } Descs[2];


    Descs[0].pResource = pSrc;
    Descs[0].View.SubresourceIndex = SrcSubresource;
    Descs[0].View.pResource = pSrc->GetUnderlyingResource();
    Descs[1].pResource = pDst;
    Descs[1].View.SubresourceIndex = DstSubresource;
    Descs[1].View.pResource = pDst->GetUnderlyingResource();

    DXGI_FORMAT DefaultResourceFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_BOX PatchedBox = {};
    if (!pSrcBox)
    {
        PatchedBox = GetBoxFromResource(pSrc, SrcSubresource);
        pSrcBox = &PatchedBox;
    }

    for (UINT i = 0; i < 2; ++i)
    {
        if (Descs[i].pResource->m_Identity->m_bOwnsUnderlyingResource && !Descs[i].pResource->m_Identity->m_bPlacedTexture)
        {
            Descs[i].View.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            DefaultResourceFormat = Descs[i].pResource->GetSubresourcePlacement(Descs[i].View.SubresourceIndex).Footprint.Format;
        }
        else
        {
            Descs[i].View.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Descs[i].View.PlacedFootprint = Descs[i].pResource->GetSubresourcePlacement(Descs[i].View.SubresourceIndex);
        }
    }

    bool bConvertSrc = false, bConvertDst = false;

    if (pSrc->AppDesc()->NonOpaquePlaneCount() == 1 && pDst->AppDesc()->NonOpaquePlaneCount() == 1)
    {
        if (DefaultResourceFormat == DXGI_FORMAT_UNKNOWN)
        {
            // No default resources, or two buffers, check if we have to convert one
            DefaultResourceFormat = Descs[1].pResource->GetSubresourcePlacement(DstSubresource).Footprint.Format;
            bConvertDst = Descs[0].pResource->GetSubresourcePlacement(SrcSubresource).Footprint.Format != DefaultResourceFormat;
            if (!bConvertDst && DefaultResourceFormat == DXGI_FORMAT_UNKNOWN)
            {
                // This is a buffer to buffer copy
                // Special-case buffer to buffer copies
                assert(Descs[0].pResource->AppDesc()->ResourceDimension() == D3D11_RESOURCE_DIMENSION_BUFFER &&
                       Descs[1].pResource->AppDesc()->ResourceDimension() == D3D11_RESOURCE_DIMENSION_BUFFER);

                UINT64 SrcOffset = pSrcBox->left + GetDynamicBufferOffset(pSrc);
                UINT64 Size = pSrcBox->right - pSrcBox->left;
                GetGraphicsCommandList()->CopyBufferRegion(pDst->GetUnderlyingResource(), dstX + GetDynamicBufferOffset(pDst),
                                                 pSrc->GetUnderlyingResource(), SrcOffset, Size);
                return;
            }
        }
        else if (Descs[0].View.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
                 Descs[1].View.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        {
            // No conversion
        }
        else if (Descs[0].View.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
                 Descs[0].pResource->GetSubresourcePlacement(SrcSubresource).Footprint.Format != DefaultResourceFormat)
        {
            assert(Descs[1].View.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
            bConvertSrc = true;
        }
        else if (Descs[1].pResource->GetSubresourcePlacement(DstSubresource).Footprint.Format != DefaultResourceFormat)
        {
            assert(Descs[0].View.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
            assert(Descs[1].View.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
            bConvertDst = true;
        }
    }

    // Convert placement struct
    if (bConvertSrc || bConvertDst)
    {
        assert(pSrc->AppDesc()->NonOpaquePlaneCount() == 1 && pDst->AppDesc()->NonOpaquePlaneCount() == 1);
        
        UINT ConversionIndex = bConvertDst ? 1 : 0;
        UINT WidthAlignment[2], HeightAlignment[2];
        auto& Placement = Descs[ConversionIndex].View.PlacedFootprint.Footprint;
        DXGI_FORMAT& Format = Placement.Format;

        WidthAlignment[0] = CD3D11FormatHelper::GetWidthAlignment( Format );
        HeightAlignment[0] = CD3D11FormatHelper::GetHeightAlignment( Format );
        Format = DefaultResourceFormat;
        WidthAlignment[1] = CD3D11FormatHelper::GetWidthAlignment( Format );
        HeightAlignment[1] = CD3D11FormatHelper::GetHeightAlignment( Format );

        Placement.Width  = Placement.Width  * WidthAlignment[1] / WidthAlignment[0];
        Placement.Height = Placement.Height * HeightAlignment[1] / HeightAlignment[0];

        // Convert coordinates/box
        if (bConvertSrc)
        {
            assert(pSrcBox);
            if (pSrcBox != &PatchedBox)
            {
                PatchedBox = *pSrcBox;
                pSrcBox = &PatchedBox;
            }

            PatchedBox.left   = PatchedBox.left   * WidthAlignment[1] / WidthAlignment[0];
            PatchedBox.right  = PatchedBox.right  * WidthAlignment[1] / WidthAlignment[0];
            PatchedBox.top    = PatchedBox.top    * HeightAlignment[1] / HeightAlignment[0];
            PatchedBox.bottom = PatchedBox.bottom * HeightAlignment[1] / HeightAlignment[0];
        }
        else if (bConvertDst)
        {
            dstX = dstX * WidthAlignment[1] / WidthAlignment[0];
            dstY = dstY * HeightAlignment[1] / HeightAlignment[0];
        }
    }
    
    // Actually issue the copy!
    GetGraphicsCommandList()->CopyTextureRegion(&Descs[1].View, dstX, dstY, dstZ, &Descs[0].View, pSrcBox);
}


//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::ResourceCopy(Resource* pDst, Resource* pSrc )
{
    assert(pSrc->NumSubresources() == pDst->NumSubresources());
    if (Resource::IsSameUnderlyingSubresource(pSrc, 0, pDst, 0))
    {
        for (UINT Subresource = 0; Subresource < pSrc->NumSubresources(); ++Subresource)
        {
            SameResourceCopy(pDst, Subresource, pSrc, Subresource, 0, 0, 0, nullptr);
        }
    }
    else
    {
        m_ResourceStateManager.TransitionResource(pDst, D3D12_RESOURCE_STATE_COPY_DEST);
        m_ResourceStateManager.TransitionResource(pSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_ResourceStateManager.ApplyAllResourceTransitions();
        
        // Note that row-major placed textures must not be passed to CopyResource, because their underlying
        // buffer's size does not include padding for alignment to pool sizes like for staging textures.
        if ((pSrc->m_Identity->m_bOwnsUnderlyingResource && pDst->m_Identity->m_bOwnsUnderlyingResource &&
            !pSrc->m_Identity->m_bPlacedTexture && !pDst->m_Identity->m_bPlacedTexture &&
            pSrc->IsBloatedConstantBuffer() == pDst->IsBloatedConstantBuffer()))
        {
            // Neither resource should be suballocated, so no offset adjustment required
            // We can do a straight resource copy from heap to heap
#if DBG
            auto& DstPlacement = pDst->GetSubresourcePlacement(0);
            auto& SrcPlacement = pSrc->GetSubresourcePlacement(0);
            assert(SrcPlacement.Offset == 0 && DstPlacement.Offset == 0 && SrcPlacement.Footprint.RowPitch == DstPlacement.Footprint.RowPitch);
#endif

            auto pAPIDst = pDst->GetUnderlyingResource();
            auto pAPISrc = pSrc->GetUnderlyingResource();
            GetGraphicsCommandList()->CopyResource(pAPIDst, pAPISrc);
        }
        else
        {
            for (UINT Subresource = 0; Subresource < pSrc->NumSubresources(); ++Subresource)
            {
                CopyAndConvertSubresourceRegion(pDst, Subresource, pSrc, Subresource, 0, 0, 0, nullptr);
            }
        }
    }
    PostCopy(pSrc, 0, pDst, 0, pSrc->NumSubresources());
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::ResourceResolveSubresource(Resource* pDst, UINT DstSubresource, Resource* pSrc, UINT SrcSubresource, DXGI_FORMAT Format )
{
    assert(pDst->m_Identity->m_bOwnsUnderlyingResource);
    assert(pSrc->m_Identity->m_bOwnsUnderlyingResource);
    
    assert(pSrc->SubresourceMultiplier() == pDst->SubresourceMultiplier());

    // Originally this would loop based on SubResourceMultiplier, allowing multiple planes to be resolved.
    // In practice, this was really only hit by depth+stencil formats, but we can only resolve the depth portion
    // since resolving the S8_UINT bit using averaging isn't valid.
    // Input subresources are plane-extended, except when dealing with depth+stencil
    UINT CurSrcSub = pSrc->GetExtendedSubresourceIndex(SrcSubresource, 0);
    UINT CurDstSub = pDst->GetExtendedSubresourceIndex(DstSubresource, 0);
    m_ResourceStateManager.TransitionSubresource(pDst, CurDstSub, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    m_ResourceStateManager.TransitionSubresource(pSrc, CurSrcSub, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    m_ResourceStateManager.ApplyAllResourceTransitions();

    auto pAPIDst = pDst->GetUnderlyingResource();
    auto pAPISrc = pSrc->GetUnderlyingResource();

    // ref for formats supporting MSAA resolve https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-11-1-feature-level-hardware

    switch(Format)
    {
        // Can't resolve due to stencil UINT. Claim it's typeless and just resolve the depth plane
        case DXGI_FORMAT_D24_UNORM_S8_UINT: Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        // Can't resolve this particular flavor of depth format. Claim it's R16_UNORM instead
        case DXGI_FORMAT_D16_UNORM: Format = DXGI_FORMAT_R16_UNORM; break;
    }
    GetGraphicsCommandList()->ResolveSubresource(pAPIDst, CurDstSub, pAPISrc, CurSrcSub, Format);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::ResourceCopyRegion(Resource* pDst, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, Resource* pSrc, UINT SrcSubresource, const D3D12_BOX* pSrcBox)
{
    assert(pSrc->SubresourceMultiplier() == pDst->SubresourceMultiplier());
    UINT SubresourceMultiplier = pSrc->SubresourceMultiplier();
    for (UINT i = 0; i < SubresourceMultiplier; ++i)
    {
        // Input subresources are plane-extended, except when dealing with depth+stencil
        UINT CurSrcSub = pSrc->GetExtendedSubresourceIndex(SrcSubresource, i);
        UINT CurDstSub = pDst->GetExtendedSubresourceIndex(DstSubresource, i);

        if (Resource::IsSameUnderlyingSubresource(pSrc, CurSrcSub, pDst, CurDstSub))
        {
            SameResourceCopy(pDst, CurDstSub, pSrc, CurSrcSub, DstX, DstY, DstZ, pSrcBox);
        }
        else
        {
            m_ResourceStateManager.TransitionSubresource(pDst, CurDstSub, D3D12_RESOURCE_STATE_COPY_DEST);
            m_ResourceStateManager.TransitionSubresource(pSrc, CurSrcSub, D3D12_RESOURCE_STATE_COPY_SOURCE);
            m_ResourceStateManager.ApplyAllResourceTransitions();

            CopyAndConvertSubresourceRegion(pDst, CurDstSub, pSrc, CurSrcSub, DstX, DstY, DstZ, pSrcBox);
        }

        PostCopy(pSrc, CurSrcSub, pDst, CurDstSub, 1);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::UploadDataToMappedBuffer(_In_reads_bytes_(Placement.Depth * DepthPitch) const void* pData, UINT SrcPitch, UINT SrcDepth,
                                       _Out_writes_bytes_(Placement.Depth * DepthPitch) void* pMappedData,
                                       D3D12_SUBRESOURCE_FOOTPRINT& Placement, UINT DepthPitch, UINT TightRowPitch) noexcept
{
    bool bPlanar = !!CD3D11FormatHelper::Planar(Placement.Format);
    UINT NumRows = bPlanar ? Placement.Height : Placement.Height / CD3D11FormatHelper::GetHeightAlignment(Placement.Format);

    ASSUME(TightRowPitch <= DepthPitch);
    ASSUME(TightRowPitch <= Placement.RowPitch);
    ASSUME(NumRows <= Placement.Height);
    ASSUME(Placement.RowPitch * NumRows <= DepthPitch);

    // Fast-path: app gave us aligned memory
    if ((Placement.RowPitch == SrcPitch || Placement.Height == 1) &&
        (DepthPitch == SrcDepth || Placement.Depth == 1))
    {
        // Allow last row to be non-padded
        UINT CopySize = DepthPitch * (Placement.Depth - 1) +
            Placement.RowPitch * (NumRows - 1) +
            TightRowPitch;
        memcpy(pMappedData, pData, CopySize);
    }
    else
    {
        // Slow path: row-by-row memcpy
        D3D12_MEMCPY_DEST Dest = { pMappedData, Placement.RowPitch, DepthPitch };
        D3D12_SUBRESOURCE_DATA Src = { pData, (LONG_PTR)SrcPitch, (LONG_PTR)SrcDepth };
        MemcpySubresource(&Dest, &Src, TightRowPitch, NumRows, Placement.Depth);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
template<typename TInterleaved, typename TPlanar, TInterleaved Mask, TPlanar Shift>
void DeInterleaving2DCopy(
    _In_reads_(_Inexpressible_(sizeof(TInterleaved) * Width + SrcRowPitch * (Height - 1))) const BYTE* pSrcData,
    UINT SrcRowPitch,
    _Out_writes_(_Inexpressible_(sizeof(TPlanar) * Width + DstRowPitch * (Height - 1))) BYTE* pDstData,
    UINT DstRowPitch, UINT Width, UINT Height)
{
    static_assert(sizeof(TInterleaved) >= sizeof(TPlanar), "Invalid types used for interleaving copy.");

    for (UINT y = 0; y < Height; ++y)
    {
        const TInterleaved* pSrcRow = reinterpret_cast<const TInterleaved*>(pSrcData + SrcRowPitch * y);
        TPlanar* pDstRow = reinterpret_cast<TPlanar*>(pDstData + DstRowPitch * y);
        for (UINT x = 0; x < Width; ++x)
        {
            pDstRow[x] = static_cast<TPlanar>((pSrcRow[x] & Mask) >> Shift);
        }
    }
}

template<typename TInterleaved, typename TPlanar, TPlanar Mask, TPlanar Shift>
void Interleaving2DCopy(
    _In_reads_(_Inexpressible_(sizeof(TPlanar) * Width + SrcRowPitch * (Height - 1))) const BYTE* pSrcData,
    UINT SrcRowPitch,
    _Out_writes_(_Inexpressible_(sizeof(TInterleaved) * Width + DstRowPitch * (Height - 1))) BYTE* pDstData,
    UINT DstRowPitch, UINT Width, UINT Height)
{
    static_assert(sizeof(TInterleaved) >= sizeof(TPlanar), "Invalid types used for interleaving copy.");

    for (UINT y = 0; y < Height; ++y)
    {
        const TPlanar* pSrcRow = reinterpret_cast<const TPlanar*>(pSrcData + SrcRowPitch * y);
        TInterleaved* pDstRow = reinterpret_cast<TInterleaved*>(pDstData + DstRowPitch * y);
        for (UINT x = 0; x < Width; ++x)
        {
            pDstRow[x] |= (static_cast<TInterleaved>(pSrcRow[x] & Mask) << Shift);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void DepthStencilDeInterleavingUpload(DXGI_FORMAT ParentFormat, UINT PlaneIndex, const BYTE* pSrcData, UINT SrcRowPitch, BYTE* pDstData, UINT DstRowPitch, UINT Width, UINT Height)
{
    ASSUME(PlaneIndex == 0 || PlaneIndex == 1);
    switch (ParentFormat)
    {
        case DXGI_FORMAT_R24G8_TYPELESS:
        {
            if (PlaneIndex == 0)
                DeInterleaving2DCopy<UINT, UINT, 0x00ffffff, 0>(pSrcData, SrcRowPitch, pDstData, DstRowPitch, Width, Height);
            else
                DeInterleaving2DCopy<UINT, UINT8, 0xff000000, 24>(pSrcData, SrcRowPitch, pDstData, DstRowPitch, Width, Height);
        } break;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        {
            if (PlaneIndex == 0)
                DeInterleaving2DCopy<UINT64, UINT, 0x00000000ffffffff, 0>(pSrcData, SrcRowPitch, pDstData, DstRowPitch, Width, Height);
            else
                DeInterleaving2DCopy<UINT64, UINT8, 0x000000ff00000000, 32>(pSrcData, SrcRowPitch, pDstData, DstRowPitch, Width, Height);
        } break;
        default: ASSUME(false);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void DepthStencilInterleavingReadback(DXGI_FORMAT ParentFormat, UINT PlaneIndex, const BYTE* pSrcData, UINT SrcRowPitch, BYTE* pDstData, UINT DstRowPitch, UINT Width, UINT Height)
{
    ASSUME(PlaneIndex == 0 || PlaneIndex == 1);
    switch (ParentFormat)
    {
        case DXGI_FORMAT_R24G8_TYPELESS:
        {
            if (PlaneIndex == 0)
                Interleaving2DCopy<UINT, UINT, 0x00ffffff, 0>(pSrcData, SrcRowPitch, pDstData, DstRowPitch, Width, Height);
            else
                Interleaving2DCopy<UINT, UINT8, 0xff, 24>(pSrcData, SrcRowPitch, pDstData, DstRowPitch, Width, Height);
        } break;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        {
            if (PlaneIndex == 0)
                Interleaving2DCopy<UINT64, UINT, 0xffffffff, 0>(pSrcData, SrcRowPitch, pDstData, DstRowPitch, Width, Height);
            else
                Interleaving2DCopy<UINT64, UINT8, 0xff, 32>(pSrcData, SrcRowPitch, pDstData, DstRowPitch, Width, Height);
        } break;
        default: ASSUME(false);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT Swap10bitRBPixel(UINT pixel)
{
    constexpr UINT alphaMask = 3u << 30u;
    constexpr UINT blueMask = 0x3FFu << 20u;
    constexpr UINT greenMask = 0x3FFu << 10u;
    constexpr UINT redMask = 0x3FFu;
    return (pixel & (alphaMask | greenMask)) |
        ((pixel & blueMask) >> 20u) |
        ((pixel & redMask) << 20u);
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void Swap10bitRBUpload(const BYTE* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch,
                              BYTE* pDstData, UINT DstRowPitch, UINT DstDepthPitch,
                              UINT Width, UINT Height, UINT Depth)
{
    for (UINT z = 0; z < Depth; ++z)
    {
        auto pSrcSlice = pSrcData + SrcDepthPitch * z;
        auto pDstSlice = pDstData + DstDepthPitch * z;
        for (UINT y = 0; y < Height; ++y)
        {
            auto pSrcRow = pSrcSlice + SrcRowPitch * y;
            auto pDstRow = pDstSlice + DstRowPitch * y;
            for (UINT x = 0; x < Width; ++x)
            {
                reinterpret_cast<UINT*>(pDstRow)[x] = Swap10bitRBPixel(reinterpret_cast<const UINT*>(pSrcRow)[x]);
            }
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
_Use_decl_annotations_
void ImmediateContext::FinalizeUpdateSubresources(Resource* pDst, PreparedUpdateSubresourcesOperation const& PreparedStorage, D3D12_PLACED_SUBRESOURCE_FOOTPRINT const* LocalPlacementDescs)
{
    bool bUseLocalPlacement = LocalPlacementDescs != nullptr;

    const UINT8 PlaneCount = (pDst->SubresourceMultiplier() * pDst->AppDesc()->NonOpaquePlaneCount());

    D3D12ResourceSuballocation mappableResource = PreparedStorage.EncodedBlock.Decode();
    CViewSubresourceSubset SubresourceIteration(PreparedStorage.EncodedSubresourceSubset, pDst->AppDesc()->MipLevels(), pDst->AppDesc()->ArraySize(), PlaneCount);

    // Copy contents over from the temporary upload heap 
    ID3D12GraphicsCommandList *pGraphicsCommandList = GetGraphicsCommandList();

    m_ResourceStateManager.TransitionSubresources(pDst, SubresourceIteration, D3D12_RESOURCE_STATE_COPY_DEST);
    m_ResourceStateManager.ApplyAllResourceTransitions();

    auto DoFinalize = [&]()
    {
        UINT PlacementIdx = 0;
        for (const auto& it : SubresourceIteration)
        {
            for (UINT Subresource = it.first; Subresource < it.second; ++Subresource, ++PlacementIdx)
            {
                auto& Placement = bUseLocalPlacement ? LocalPlacementDescs[PlacementIdx] : pDst->GetSubresourcePlacement(Subresource);
                D3D12_BOX srcBox = { 0, 0, 0, Placement.Footprint.Width, Placement.Footprint.Height, Placement.Footprint.Depth };
                if (pDst->AppDesc()->ResourceDimension() == D3D12_RESOURCE_DIMENSION_BUFFER)
                {
                    ASSUME(Placement.Footprint.Height == 1 && Placement.Footprint.Depth == 1);
                    UINT64 srcOffset = mappableResource.GetOffset();
                    UINT64 dstOffset =
                        pDst->GetSubresourcePlacement(0).Offset +
                        (PreparedStorage.bDstBoxPresent ? PreparedStorage.DstX : 0);
                    pGraphicsCommandList->CopyBufferRegion(pDst->GetUnderlyingResource(),
                                                           dstOffset,
                                                           mappableResource.GetResource(),
                                                           srcOffset,
                                                           Placement.Footprint.Width);
                }
                else
                {
                    D3D12_TEXTURE_COPY_LOCATION SrcDesc = mappableResource.GetCopyLocation(Placement);
                    SrcDesc.PlacedFootprint.Offset -= PreparedStorage.OffsetAdjustment;
                    bool bDstPlacedTexture = !pDst->m_Identity->m_bOwnsUnderlyingResource || pDst->m_Identity->m_bPlacedTexture;
                    D3D12_TEXTURE_COPY_LOCATION DstDesc = bDstPlacedTexture ?
                        CD3DX12_TEXTURE_COPY_LOCATION(pDst->GetUnderlyingResource(), pDst->GetSubresourcePlacement(Subresource)) :
                        CD3DX12_TEXTURE_COPY_LOCATION(pDst->GetUnderlyingResource(), Subresource);
                    pGraphicsCommandList->CopyTextureRegion(&DstDesc,
                                                            PreparedStorage.DstX,
                                                            PreparedStorage.DstY,
                                                            PreparedStorage.DstZ,
                                                            &SrcDesc,
                                                            &srcBox);
                }
            }
        }
    };

    DoFinalize();

    AdditionalCommandsAdded();

    ReleaseSuballocatedHeap(AllocatorHeapType::Upload, mappableResource, GetCommandListID());
}

//----------------------------------------------------------------------------------------------------------------------------------
ImmediateContext::CPrepareUpdateSubresourcesHelper::CPrepareUpdateSubresourcesHelper(
    Resource& Dst,
    CSubresourceSubset const& Subresources,
    const D3D11_SUBRESOURCE_DATA* pSrcData,
    const D3D12_BOX* pDstBox,
    UpdateSubresourcesFlags flags,
    const void* pClearPattern,
    UINT ClearPatternSize,
    ImmediateContext& ImmCtx)
    : Dst(Dst)
    , Subresources(Subresources)
    , bDstBoxPresent(pDstBox != nullptr)
{
#if DBG
    AssertPreconditions(pSrcData, pClearPattern);
#endif
    bool bEmptyBox = InitializePlacementsAndCalculateSize(pDstBox, ImmCtx.m_pDevice12.get());
    if (bEmptyBox)
    {
        return;
    }

    InitializeMappableResource(flags, ImmCtx, pDstBox);
    FinalizeNeeded = CachedNeedsTemporaryUploadHeap;

    UploadDataToMappableResource(pSrcData, ImmCtx, pDstBox, pClearPattern, ClearPatternSize, flags);

    if (FinalizeNeeded)
    {
        WriteOutputParameters(pDstBox, flags);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
#if DBG
void ImmediateContext::CPrepareUpdateSubresourcesHelper::AssertPreconditions(const D3D11_SUBRESOURCE_DATA* pSrcData, const void* pClearPattern)
{
    // Currently only handles initial data and UpdateSubresource-type operations
    // This means: 1 plane, 1 legacy subresource, or all subresources with no box
    assert(NumSrcData == 1U || (NumSrcData == static_cast<UINT>(Dst.AppDesc()->MipLevels() * Dst.AppDesc()->ArraySize()) && !bDstBoxPresent && !pClearPattern));
    assert(NumDstSubresources == 1U || NumDstSubresources == Dst.SubresourceMultiplier() || (NumDstSubresources == Dst.NumSubresources() && !bDstBoxPresent && !pClearPattern));

    // This routine accepts either a clear color (one pixel worth of data) or a SUBRESOURCE_DATA struct (minimum one row of data)
    assert(!(pClearPattern && pSrcData));
    ASSUME(!bUseLocalPlacement || NumDstSubresources == 1 || (NumDstSubresources == 2 && Subresources.m_EndPlane - Subresources.m_BeginPlane == 2));

    CViewSubresourceSubset SubresourceIteration(Subresources, Dst.AppDesc()->MipLevels(), Dst.AppDesc()->ArraySize(), PlaneCount);
    assert(!SubresourceIteration.IsEmpty());
}
#endif

//----------------------------------------------------------------------------------------------------------------------------------
bool ImmediateContext::CPrepareUpdateSubresourcesHelper::InitializePlacementsAndCalculateSize(const D3D12_BOX* pDstBox, ID3D12Device* pDevice)
{
    auto& LocalPlacementDescs = PreparedStorage.LocalPlacementDescs;

    // How big of an intermediate do we need?
    // If we need to use local placement structs, fill those out as well
    if (bUseLocalPlacement)
    {
        for (UINT i = 0; i < NumDstSubresources; ++i)
        {
            auto& PlacementDesc = LocalPlacementDescs[i];
            UINT Subresource = ComposeSubresourceIdxExtended(Subresources.m_BeginMip, Subresources.m_BeginArray, Subresources.m_BeginPlane + i, Dst.AppDesc()->MipLevels(), Dst.AppDesc()->ArraySize());
            UINT SlicePitch;
            if (pDstBox)
            {
                // No-op
                if (pDstBox->right <= pDstBox->left ||
                    pDstBox->bottom <= pDstBox->top ||
                    pDstBox->back <= pDstBox->front)
                {
                    return true;
                }

                // Note: D3D11 provides a subsampled box, so for planar formats, we need to use the plane format to avoid subsampling again
                Resource::FillSubresourceDesc(pDevice,
                                                Dst.GetSubresourcePlacement(Subresource).Footprint.Format,
                                                pDstBox->right - pDstBox->left,
                                                pDstBox->bottom - pDstBox->top,
                                                pDstBox->back - pDstBox->front,
                                                PlacementDesc);

                CD3D11FormatHelper::CalculateMinimumRowMajorSlicePitch(PlacementDesc.Footprint.Format,
                                                                        PlacementDesc.Footprint.RowPitch,
                                                                        PlacementDesc.Footprint.Height,
                                                                        SlicePitch);
            }
            else
            {
                PlacementDesc = Dst.GetSubresourcePlacement(Subresource);
                SlicePitch = Dst.DepthPitch(Subresource);
            }
            PlacementDesc.Offset = TotalSize;

            TotalSize += Align<UINT64>(static_cast<UINT64>(SlicePitch) * PlacementDesc.Footprint.Depth, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        }
    }
    else
    {
        auto& Placement = Dst.GetSubresourcePlacement(LastDstSubresource);

        // If the destination is suballocated, make sure to factor out the suballocated offset when calculating how
        // large the resource needs to be
        UINT64 suballocationOffset = Dst.m_Identity->m_bOwnsUnderlyingResource ? 0 : Dst.m_Identity->GetSuballocatedOffset();

        TotalSize =
            (Placement.Offset - suballocationOffset) +
            static_cast<UINT64>(Dst.DepthPitch(LastDstSubresource)) * Placement.Footprint.Depth -
            (Dst.GetSubresourcePlacement(FirstDstSubresource).Offset - suballocationOffset);
    }
    return false;
}

//----------------------------------------------------------------------------------------------------------------------------------
bool ImmediateContext::CPrepareUpdateSubresourcesHelper::NeedTemporaryUploadHeap(UpdateSubresourcesFlags flags , ImmediateContext& ImmCtx) const
{
    UpdateSubresourcesFlags scenario = (flags & UpdateSubresourcesFlags::ScenarioMask);
    bool bCanWriteDirectlyToResource =
        scenario != UpdateSubresourcesFlags::ScenarioBatchedContext &&      // If we aren't explicitly requesting a copy to a temp...
        !Dst.GetIdentity()->m_bOwnsUnderlyingResource &&             // And the resource came from a pool...
        Dst.GetAllocatorHeapType() != AllocatorHeapType::Readback;   // And it's not the readback pool...
    if (bCanWriteDirectlyToResource && scenario != UpdateSubresourcesFlags::ScenarioInitialData)
    {
        // Check if resource is idle.
        CViewSubresourceSubset SubresourceIteration(Subresources, Dst.AppDesc()->MipLevels(), Dst.AppDesc()->ArraySize(), PlaneCount);
        for (auto&& range : SubresourceIteration)
        {
            for (UINT i = range.first; i < range.second; ++i)
            {
                if (!ImmCtx.SynchronizeForMap(&Dst, i, MAP_TYPE_WRITE, true))
                {
                    bCanWriteDirectlyToResource = false;
                    break;
                }
            }
            if (!bCanWriteDirectlyToResource)
            {
                break;
            }
        }
    }
    // ... And it's not busy, then we can do this upload operation directly into the final destination resource.
    return !bCanWriteDirectlyToResource;
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CPrepareUpdateSubresourcesHelper::InitializeMappableResource(UpdateSubresourcesFlags flags, ImmediateContext& ImmCtx, D3D12_BOX const* pDstBox)
{
    UpdateSubresourcesFlags scenario = flags & UpdateSubresourcesFlags::ScenarioMask;
    CachedNeedsTemporaryUploadHeap = NeedTemporaryUploadHeap(flags, ImmCtx);
    if (CachedNeedsTemporaryUploadHeap)
    {
        ResourceAllocationContext threadingContext = ResourceAllocationContext::ImmediateContextThreadTemporary;
        if (scenario == UpdateSubresourcesFlags::ScenarioInitialData ||
            scenario == UpdateSubresourcesFlags::ScenarioBatchedContext)
        {
            threadingContext = ResourceAllocationContext::FreeThread;
        }
        mappableResource = ImmCtx.AcquireSuballocatedHeap(AllocatorHeapType::Upload, TotalSize, threadingContext); // throw( _com_error )
    }
    else
    {
        if (pDstBox)
        {
            // Only DX9 managed vertex buffers and dx11 padded constant buffers hit this path, so extra complexity isn't required yet
            assert(Dst.Parent()->m_desc12.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
            assert(!bDeInterleavingUpload && Dst.GetSubresourcePlacement(0).Footprint.Format == DXGI_FORMAT_UNKNOWN);
            assert(pDstBox->top == 0 && pDstBox->front == 0);

            bufferOffset = pDstBox ? pDstBox->left : 0;
        }

        mappableResource = Dst.GetIdentity()->m_suballocation;
    }
    assert(mappableResource.IsInitialized());
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CPrepareUpdateSubresourcesHelper::UploadSourceDataToMappableResource(void* pDstData, D3D11_SUBRESOURCE_DATA const* pSrcData, ImmediateContext& ImmCtx, UpdateSubresourcesFlags flags)
{
    // The source data array provided is indexed by D3D11.0 subresource indices
    for (UINT SrcDataIdx = 0; SrcDataIdx < NumSrcData; ++SrcDataIdx)
    {
        auto& SrcData = pSrcData[SrcDataIdx];
        UINT ArraySlice, MipLevel;
        DecomposeSubresourceIdxNonExtended(SrcDataIdx, Subresources.m_EndMip - Subresources.m_BeginMip, MipLevel, ArraySlice);

        const BYTE* pSrcPlaneData = reinterpret_cast<const BYTE*>(SrcData.pSysMem);

        // Even though the next subresource is supposed to be the next mip, planes are iterated last so that the pointer adjustment
        // for planar source data doesn't need to be calculated a second time
        for (UINT Plane = Subresources.m_BeginPlane; Plane < Subresources.m_EndPlane; ++Plane)
        {
            const UINT Subresource = ComposeSubresourceIdxExtended(MipLevel + Subresources.m_BeginMip, ArraySlice + Subresources.m_BeginArray, Plane,
                                                                    Dst.AppDesc()->MipLevels(), Dst.AppDesc()->ArraySize());
            auto& Placement = bUseLocalPlacement ? PreparedStorage.LocalPlacementDescs[Plane - Subresources.m_BeginPlane] : Dst.GetSubresourcePlacement(Subresource);

            BYTE* pDstSubresourceData = reinterpret_cast<BYTE*>(pDstData) + Placement.Offset - PreparedStorage.Base.OffsetAdjustment;

            // If writing directly into the resource, we need to account for the dstBox instead of leaving it to the GPU copy
            if (!CachedNeedsTemporaryUploadHeap)
            {
                pDstSubresourceData += bufferOffset;
            }

            if (bDeInterleavingUpload)
            {
                DepthStencilDeInterleavingUpload(ImmCtx.GetParentForFormat(Dst.AppDesc()->Format()), Plane,
                                                    pSrcPlaneData, SrcData.SysMemPitch,
                                                    pDstSubresourceData, Placement.Footprint.RowPitch,
                                                    Placement.Footprint.Width, Placement.Footprint.Height);
                // Intentionally not advancing the src pointer, since the next copy reads from the same data
            }
            else if ((flags & UpdateSubresourcesFlags::ChannelSwapR10G10B10A2) != UpdateSubresourcesFlags::None)
            {
                Swap10bitRBUpload(pSrcPlaneData, SrcData.SysMemPitch, SrcData.SysMemSlicePitch,
                                  pDstSubresourceData, Placement.Footprint.RowPitch, Dst.DepthPitch(Subresource),
                                  Placement.Footprint.Width, Placement.Footprint.Height, Placement.Footprint.Depth);
            }
            else
            {
                // Tight row pitch is how much data to copy per row
                UINT TightRowPitch;
                CD3D11FormatHelper::CalculateMinimumRowMajorRowPitch(Placement.Footprint.Format, Placement.Footprint.Width, TightRowPitch);

                // Slice pitches are provided to enable fast paths which use a single memcpy
                UINT SrcSlicePitch = Dst.Parent()->ResourceDimension12() < D3D12_RESOURCE_DIMENSION_TEXTURE3D ?
                    (SrcData.SysMemPitch * Placement.Footprint.Height) : SrcData.SysMemSlicePitch;
                UINT DstSlicePitch;
                if (bDstBoxPresent)
                {
                    CD3D11FormatHelper::CalculateMinimumRowMajorSlicePitch(Placement.Footprint.Format,
                                                                            Placement.Footprint.RowPitch,
                                                                            Placement.Footprint.Height,
                                                                            DstSlicePitch);
                }
                else
                {
                    DstSlicePitch = Dst.DepthPitch(Subresource);
                }

                ImmediateContext::UploadDataToMappedBuffer(pSrcPlaneData, SrcData.SysMemPitch, SrcSlicePitch,
                                                            pDstSubresourceData, Placement.Footprint,
                                                            DstSlicePitch, TightRowPitch);

                pSrcPlaneData += SrcData.SysMemPitch * Placement.Footprint.Height;
            }
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CPrepareUpdateSubresourcesHelper::UploadDataToMappableResource(D3D11_SUBRESOURCE_DATA const* pSrcData, ImmediateContext& ImmCtx, D3D12_BOX const* pDstBox, const void* pClearPattern, UINT ClearPatternSize, UpdateSubresourcesFlags flags)
{
    // Now that we have something we can upload the data to, map it
    void* pDstData;
    const D3D12_RANGE ReadRange = {};
    ThrowFailure(mappableResource.Map(0, &ReadRange, &pDstData)); // throw( _com_error )

    // Now, upload the data for each subresource

    // The offset adjustment is subtracted from the offset of the given subresource, to calculate a location to write to
    // If we are doing UpdateSubresource on subresources 3 and 4, the offset to write to for subresource 4 is (offset of 4 - offset of 3)
    auto& FirstSubresourcePlacement = bUseLocalPlacement ? PreparedStorage.LocalPlacementDescs[0] : Dst.GetSubresourcePlacement(FirstDstSubresource);
    PreparedStorage.Base.OffsetAdjustment = FirstSubresourcePlacement.Offset;

    if (pSrcData != nullptr)
    {
        UploadSourceDataToMappableResource(pDstData, pSrcData, ImmCtx, flags);
    }
    else
    {
        // Just zero/fill the memory
        assert(TotalSize < size_t(-1));
        UINT64 CopySize = TotalSize;

        // If writing directly into the resource, we need to account for the dstBox instead of leaving it to the GPU copy
        if (!CachedNeedsTemporaryUploadHeap && pDstBox)
        {
            CopySize = min<UINT64>(CopySize, pDstBox->right - pDstBox->left);
        }

        if (pClearPattern)
        {
            assert(!CD3D11FormatHelper::Planar(Dst.AppDesc()->Format()) && CD3D11FormatHelper::GetBitsPerElement(Dst.AppDesc()->Format()) % 8 == 0);
            assert(NumDstSubresources == 1);
            // What we're clearing here may not be one pixel, so intentionally using GetByteAlignment to determine the minimum size
            // for a fully aligned block of pixels. (E.g. YUY2 is 8 bits per element * 2 elements per pixel * 2 pixel subsampling = 32 bits of clear data).
            const UINT SizeOfClearPattern = ClearPatternSize != 0 ? ClearPatternSize :
                CD3D11FormatHelper::GetByteAlignment(Dst.AppDesc()->Format());
            UINT ClearByteIndex = 0;
            auto generator = [&]()
            {
                auto result = *(reinterpret_cast<const BYTE*>(pClearPattern) + ClearByteIndex);
                ClearByteIndex = (ClearByteIndex + 1) % SizeOfClearPattern;
                return result;
            };
            if (FirstSubresourcePlacement.Footprint.RowPitch % SizeOfClearPattern != 0)
            {
                UINT SlicePitch;
                CD3D11FormatHelper::CalculateMinimumRowMajorSlicePitch(
                    FirstSubresourcePlacement.Footprint.Format,
                    FirstSubresourcePlacement.Footprint.RowPitch,
                    FirstSubresourcePlacement.Footprint.Height,
                    SlicePitch);

                // We need to make sure to leave a gap in the pattern so that it starts on byte 0 for every row
                for (UINT z = 0; z < FirstSubresourcePlacement.Footprint.Depth; ++z)
                {
                    for (UINT y = 0; y < FirstSubresourcePlacement.Footprint.Height; ++y)
                    {
                        BYTE* pDstRow = (BYTE*)pDstData + bufferOffset +
                            FirstSubresourcePlacement.Footprint.RowPitch * y +
                            SlicePitch * z;
                        ClearByteIndex = 0;
                        std::generate_n(pDstRow, FirstSubresourcePlacement.Footprint.RowPitch, generator);
                    }
                }
            }
            else
            {
                std::generate_n((BYTE*)pDstData + bufferOffset, CopySize, generator);
            }
        }
        else
        {
            ZeroMemory((BYTE *)pDstData + bufferOffset, static_cast<size_t>(CopySize));
        }
    }

    CD3DX12_RANGE WrittenRange(0, SIZE_T(TotalSize));
    mappableResource.Unmap(0, &WrittenRange);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CPrepareUpdateSubresourcesHelper::WriteOutputParameters(D3D12_BOX const* pDstBox, UpdateSubresourcesFlags flags)
{
    // Write output parameters
    UpdateSubresourcesFlags scenario = flags & UpdateSubresourcesFlags::ScenarioMask;
    if (pDstBox)
    {
        PreparedStorage.Base.DstX = pDstBox->left;
        PreparedStorage.Base.DstY = pDstBox->top;
        PreparedStorage.Base.DstZ = pDstBox->front;
    }
    else
    {
        PreparedStorage.Base.DstX = 0;
        PreparedStorage.Base.DstY = 0;
        PreparedStorage.Base.DstZ = 0;
    }
    PreparedStorage.Base.EncodedBlock = EncodedResourceSuballocation(mappableResource);
    PreparedStorage.Base.EncodedSubresourceSubset = Subresources;
    PreparedStorage.Base.bDisablePredication =
        (scenario == UpdateSubresourcesFlags::ScenarioInitialData || scenario == UpdateSubresourcesFlags::ScenarioImmediateContextInternalOp);
    PreparedStorage.Base.bDstBoxPresent = bDstBoxPresent;
}

//----------------------------------------------------------------------------------------------------------------------------------
_Use_decl_annotations_
void ImmediateContext::UpdateSubresources(Resource* pDst, D3D12TranslationLayer::CSubresourceSubset const& Subresources, const D3D11_SUBRESOURCE_DATA* pSrcData, const D3D12_BOX* pDstBox, UpdateSubresourcesFlags flags, const void* pClearColor )
{
    CPrepareUpdateSubresourcesHelper PrepareHelper(*pDst, Subresources, pSrcData, pDstBox, flags, pClearColor, 0, *this);
    if (PrepareHelper.FinalizeNeeded)
    {
        FinalizeUpdateSubresources(pDst, PrepareHelper.PreparedStorage.Base, PrepareHelper.bUseLocalPlacement ? PrepareHelper.PreparedStorage.LocalPlacementDescs : nullptr);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::ResourceUpdateSubresourceUP(Resource* pResource, UINT DstSubresource, _In_opt_ const D3D12_BOX* pDstBox, _In_ const VOID* pMem, UINT SrcPitch, UINT SrcDepth)
{
    D3D11_SUBRESOURCE_DATA SubresourceDesc = { pMem, SrcPitch, SrcDepth };
    UINT8 MipLevel, PlaneSlice;
    UINT16 ArraySlice;
    DecomposeSubresourceIdxExtended(DstSubresource, pResource->AppDesc()->MipLevels(), pResource->AppDesc()->ArraySize(), MipLevel, ArraySlice, PlaneSlice);
    UpdateSubresources(pResource,
        CSubresourceSubset(1, 1, pResource->SubresourceMultiplier(), MipLevel, ArraySlice, PlaneSlice),
        &SubresourceDesc, pDstBox);
}

unique_comptr<ID3D12Resource> ImmediateContext::AcquireTransitionableUploadBuffer(AllocatorHeapType HeapType, UINT64 Size) noexcept(false)
{
    TDynamicBufferPool& Pool = GetBufferPool(HeapType);
    auto pfnCreateNew = [this, HeapType](UINT64 Size) -> unique_comptr<ID3D12Resource> // noexcept(false)
    {
        return std::move(AllocateHeap(Size, 0, HeapType));
    };

    UINT64 CurrentFence = GetCompletedFenceValue();

    return Pool.RetrieveFromPool(Size, CurrentFence, pfnCreateNew); // throw( _com_error )
}

D3D12ResourceSuballocation ImmediateContext::AcquireSuballocatedHeapForResource(_In_ Resource* pResource, ResourceAllocationContext threadingContext) noexcept(false)
{
    UINT64 ResourceSize = pResource->GetResourceSize();
    
    // SRV buffers do not allow offsets to be specified in bytes but instead by number of Elements. This requires that the offset must 
    // always be aligned to an element size, which cannot be predicted since buffers can be created as DXGI_FORMAT_UNKNOWN and SRVs 
    // can later be created later with an arbitrary DXGI_FORMAT. To handle this, we don't allow a suballocated offset for this case
    bool bCannotBeOffset = (pResource->AppDesc()->BindFlags() & RESOURCE_BIND_SHADER_RESOURCE) && (pResource->AppDesc()->ResourceDimension() == D3D12_RESOURCE_DIMENSION_BUFFER);
    
    AllocatorHeapType HeapType = pResource->GetAllocatorHeapType();
    return AcquireSuballocatedHeap(HeapType, ResourceSize, threadingContext, bCannotBeOffset); // throw( _com_error )
}

//----------------------------------------------------------------------------------------------------------------------------------
D3D12ResourceSuballocation ImmediateContext::AcquireSuballocatedHeap(AllocatorHeapType HeapType, UINT64 Size, ResourceAllocationContext threadingContext, bool bCannotBeOffset) noexcept(false)
{
    if (threadingContext == ResourceAllocationContext::ImmediateContextThreadTemporary)
    {
        UploadHeapSpaceAllocated(Size);
    }

    auto &allocator = GetAllocator(HeapType);
    
    HeapSuballocationBlock suballocation =
        TryAllocateResourceWithFallback([&]()
    {
        auto block = allocator.Allocate(Size, bCannotBeOffset);
        if (block.GetSize() == 0)
        {
            throw _com_error(E_OUTOFMEMORY);
        }
        return block;
    }, threadingContext);

    return D3D12ResourceSuballocation(allocator.GetInnerAllocation(suballocation), suballocation);
}

//----------------------------------------------------------------------------------------------------------------------------------
bool ImmediateContext::ResourceAllocationFallback(ResourceAllocationContext threadingContext)
{
    if (TrimDeletedObjects())
    {
        return true;
    }

    UINT64 SyncPoint;
    {
        auto DeletionManagerLocked = m_DeferredDeletionQueueManager.GetLocked();
        SyncPoint = std::min(DeletionManagerLocked->GetFenceValueForObjectDeletion(),
                             DeletionManagerLocked->GetFenceValueForSuballocationDeletion());
    }
    // If one is strictly less than the other, wait just for that one.
    const bool ImmediateContextThread = threadingContext != ResourceAllocationContext::FreeThread;

    auto CommandListManager = GetCommandListManager();
    if (CommandListManager)
    {
        CommandListManager->WaitForFenceValueInternal(ImmediateContextThread, SyncPoint); // throws
    }

    // DeferredDeletionQueueManager::TrimDeletedObjects() is the only place where we pop() 
    // items from the deletion queues. This means that, if the sync points are different after
    // the WaitForSyncPoint call, we must have called TrimDeletedObjects and freed some memory. 
    UINT64 newSyncPoint;
    {
        auto DeletionManagerLocked = m_DeferredDeletionQueueManager.GetLocked();
        newSyncPoint = std::min(DeletionManagerLocked->GetFenceValueForObjectDeletion(),
                                DeletionManagerLocked->GetFenceValueForSuballocationDeletion());
    }
    bool freedMemory = newSyncPoint < ~0ull && newSyncPoint != SyncPoint;

    // If we've already freed up memory go ahead and return true, else try to Trim now and return that result
    return freedMemory || TrimDeletedObjects();
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::ReturnAllBuffersToPool(Resource& UnderlyingResource) noexcept
{
    if (!UnderlyingResource.m_Identity)
    {
        return;
    }

    if (!UnderlyingResource.m_Identity->m_bOwnsUnderlyingResource)
    {
        assert(UnderlyingResource.m_Identity->m_spUnderlyingResource.get() == nullptr);
        AllocatorHeapType HeapType = UnderlyingResource.GetAllocatorHeapType();

        if (!UnderlyingResource.m_Identity->m_suballocation.IsInitialized())
        {
            return;
        }

        assert(UnderlyingResource.AppDesc()->CPUAccessFlags() != 0);

        ReleaseSuballocatedHeap(
            HeapType, 
            UnderlyingResource.m_Identity->m_suballocation,
            UnderlyingResource.m_LastUsedCommandListID);
    }
}


// This is for cases where we're copying a small subrect from one surface to another,
// specifically when we only want to copy the first part of each row. 
void MemcpySubresourceWithCopySize(
    _In_ const D3D12_MEMCPY_DEST* pDest,
    _In_ const D3D12_SUBRESOURCE_DATA* pSrc,
    SIZE_T /*RowSizeInBytes*/,
    UINT CopySize,
    UINT NumRows,
    UINT NumSlices)
{
    for (UINT z = 0; z < NumSlices; ++z)
    {
        BYTE* pDestSlice = reinterpret_cast<BYTE*>(pDest->pData) + pDest->SlicePitch * z;
        const BYTE* pSrcSlice = reinterpret_cast<const BYTE*>(pSrc->pData) + pSrc->SlicePitch * z;
        for (UINT y = 0; y < NumRows; ++y)
        {
            memcpy(pDestSlice + pDest->RowPitch * y,
                pSrcSlice + pSrc->RowPitch * y,
                CopySize);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::ReturnTransitionableBufferToPool(AllocatorHeapType HeapType, UINT64 Size, unique_comptr<ID3D12Resource>&& spResource, UINT64 FenceValue) noexcept
{
    TDynamicBufferPool& Pool = GetBufferPool(HeapType);

    Pool.ReturnToPool(
        Size,
        std::move(spResource),
        FenceValue);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::ReleaseSuballocatedHeap(AllocatorHeapType HeapType, D3D12ResourceSuballocation &resource, UINT64 FenceValue) noexcept
{
    auto &allocator = GetAllocator(HeapType);

    m_DeferredDeletionQueueManager.GetLocked()->AddSuballocationToQueue(resource.GetBufferSuballocation(), allocator, FenceValue);
    resource.Reset();
}

//----------------------------------------------------------------------------------------------------------------------------------
bool ImmediateContext::MapDynamicTexture(Resource* pResource, UINT Subresource, MAP_TYPE MapType, bool DoNotWait, _In_opt_ const D3D12_BOX *pReadWriteRange, MappedSubresource* pMap )
{
    assert(pResource->GetEffectiveUsage() == RESOURCE_USAGE_DYNAMIC);
    assert(MapType != MAP_TYPE_WRITE_NOOVERWRITE);

    UINT MipIndex, PlaneIndex, ArrayIndex;
    pResource->DecomposeSubresource(Subresource, MipIndex, ArrayIndex, PlaneIndex);

    const bool bNeedsReadbackCopy = MapType == MAP_TYPE_READ || MapType == MAP_TYPE_READWRITE
        // Note that MAP_TYPE_WRITE could be optimized to keep around a copy of the last-written
        // data and simply re-upload that on unmap, rather than doing a full read-modify-write loop
        // What we can't do, is skip the readback, because we don't have the current contents available
        // to be modified, and this isn't a discard operation.
        // Currently the only scenario that hits this is a shared vertex/index buffer in 9on12, so we don't care yet.
        || MapType == MAP_TYPE_WRITE;

    // If the app uses DoNotWait with a read flag, the translation layer is guaranteed to need to do GPU work
    // in order to copy the GPU data to a readback heap. As a result the first call to map will initiate the copy
    // and return that a draw is still in flight. The next map that's called after the copy is finished will
    // succeed.
    bool bReadbackCopyInFlight = pResource->GetCurrentCpuHeap(Subresource) != nullptr &&
        !pResource->GetDynamicTextureData(Subresource).AnyPlaneMapped();
    if (bReadbackCopyInFlight)
    {
        // If an app modifies the resource after a readback copy has been initiated but not mapped again,
        // make sure to invalidate the old copy so that the app 
        auto& state = pResource->m_Identity->m_currentState.GetSubresourceState(Subresource);
        const bool bPreviousCopyInvalid = 
            // If the app changed it's mind and decided it doesn't need to readback anymore,
            // we can throw out the copy since they'll be modifying the resource anyways
            !bNeedsReadbackCopy ||
            // The copy was done on the graphics queue so it's been modified
            // if the last write state was outside of graphics or newer than the
            // last exclusive state's fence value
            state.WriteFenceValue > pResource->GetLastCopyCommandListID(Subresource);

        if (bPreviousCopyInvalid)
        {
            pResource->SetCurrentCpuHeap(Subresource, nullptr);
            bReadbackCopyInFlight = false;
        }
    }

    // For planar textures, the upload buffer is created for all planes when all planes
    // were previously not mapped
    if (!pResource->GetDynamicTextureData(Subresource).AnyPlaneMapped())
    {
        if (!bReadbackCopyInFlight)
        {
            assert(pResource->GetEffectiveUsage() == RESOURCE_USAGE_DYNAMIC);

            auto desc12 = pResource->m_creationArgs.m_desc12;

            auto& Placement = pResource->GetSubresourcePlacement(Subresource);
            desc12.MipLevels = 1;
            desc12.Width = Placement.Footprint.Width;
            desc12.Height = Placement.Footprint.Height;
            desc12.DepthOrArraySize = static_cast<UINT16>(
                desc12.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ?
                    Placement.Footprint.Depth : 1);

            RESOURCE_CPU_ACCESS cpuAccess =   (bNeedsReadbackCopy ? RESOURCE_CPU_ACCESS_READ : RESOURCE_CPU_ACCESS_NONE) 
                                            | (MapType != MAP_TYPE_READ ? RESOURCE_CPU_ACCESS_WRITE : RESOURCE_CPU_ACCESS_NONE);

            auto creationArgsCopy = pResource->m_creationArgs;
            creationArgsCopy.m_appDesc = AppResourceDesc(desc12, RESOURCE_USAGE_STAGING, cpuAccess, RESOURCE_BIND_NONE);

            UINT64 resourceSize = 0;
            m_pDevice12->GetCopyableFootprints(&desc12, 0, creationArgsCopy.m_appDesc.NonOpaquePlaneCount(), 0, nullptr, nullptr, nullptr, &resourceSize);

            creationArgsCopy.m_heapDesc = CD3DX12_HEAP_DESC(resourceSize, Resource::GetD3D12HeapType(RESOURCE_USAGE_STAGING, cpuAccess));
            creationArgsCopy.m_heapType = AllocatorHeapType::None;
            creationArgsCopy.m_flags11.BindFlags = 0;
            creationArgsCopy.m_flags11.MiscFlags = 0;
            creationArgsCopy.m_flags11.CPUAccessFlags = (bNeedsReadbackCopy ? D3D11_CPU_ACCESS_READ : 0) 
                                            | (MapType != MAP_TYPE_READ ? D3D11_CPU_ACCESS_WRITE : 0);
            creationArgsCopy.m_flags11.StructureByteStride = 0;

            unique_comptr<Resource> renameResource = Resource::CreateResource(this, creationArgsCopy, ResourceAllocationContext::FreeThread);
            pResource->SetCurrentCpuHeap(Subresource, renameResource.get());
            
            CD3DX12_RANGE ReadRange(0, 0); // Map(DISCARD) is write-only
            D3D12_RANGE MappedRange = renameResource->GetSubresourceRange(0, pReadWriteRange);

            if (bNeedsReadbackCopy)
            {
                UINT DstX = pReadWriteRange ? pReadWriteRange->left : 0u;
                UINT DstY = pReadWriteRange ? pReadWriteRange->top : 0u;
                UINT DstZ = pReadWriteRange ? pReadWriteRange->front : 0u;

                // Copy each plane.
                for (UINT iPlane = 0; iPlane < pResource->AppDesc()->NonOpaquePlaneCount(); ++iPlane)
                {
                    UINT planeSubresource = pResource->GetSubresourceIndex(iPlane, MipIndex, ArrayIndex);
                    ResourceCopyRegion(renameResource.get(), iPlane, DstX, DstY, DstZ, pResource, planeSubresource, pReadWriteRange);
                }

                pResource->SetLastCopyCommandListID(Subresource, GetCommandListID());
            }
        }
    }

    {
        // Synchronize for the Map.  Renamed resource has no mips or array slices.
        Resource* pRenameResource = pResource->GetCurrentCpuHeap(Subresource);
        assert(pRenameResource->AppDesc()->MipLevels() == 1);
        assert(pRenameResource->AppDesc()->ArraySize() == 1);

        if (!MapUnderlyingSynchronize(pRenameResource, PlaneIndex, MapType, DoNotWait, pReadWriteRange, pMap))
        {
            return false;
        }
    }

    // Record that the given plane was mapped and is now dirty
    pResource->GetDynamicTextureData(Subresource).m_MappedPlaneRefCount[PlaneIndex]++;
    pResource->GetDynamicTextureData(Subresource).m_DirtyPlaneMask |= (1 << PlaneIndex);

    return true;
}

//----------------------------------------------------------------------------------------------------------------------------------
bool  ImmediateContext::MapUnderlying(Resource* pResource, UINT Subresource, MAP_TYPE MapType, _In_opt_ const D3D12_BOX *pReadWriteRange, MappedSubresource* pMap )
{
    assert(pResource->AppDesc()->Usage() == RESOURCE_USAGE_DYNAMIC || pResource->AppDesc()->Usage() == RESOURCE_USAGE_STAGING);
    assert(pResource->OwnsReadbackHeap() || pResource->UnderlyingResourceIsSuballocated());

    auto pResource12 = pResource->GetUnderlyingResource();
    void* pData;

    // Write-only means read range can be empty
    D3D12_RANGE MappedRange = pResource->GetSubresourceRange(Subresource, pReadWriteRange);
    D3D12_RANGE ReadRange =
        MapType == MAP_TYPE_WRITE ? CD3DX12_RANGE(0, 0) : MappedRange;
    HRESULT hr = pResource12->Map(0, &ReadRange, &pData);
    ThrowFailure(hr); // throw( _com_error )

    auto& SubresourceInfo = pResource->GetSubresourcePlacement(Subresource);
    pMap->pData = reinterpret_cast<void*>(reinterpret_cast<size_t>(pData) + MappedRange.Begin);
    pMap->RowPitch = SubresourceInfo.Footprint.RowPitch;
    pMap->DepthPitch = pResource->DepthPitch(Subresource);

    return true;
}

//----------------------------------------------------------------------------------------------------------------------------------
bool ImmediateContext::SynchronizeForMap(Resource* pResource, UINT Subresource, MAP_TYPE MapType, bool DoNotWait)
{
    if (MapType == MAP_TYPE_READ || MapType == MAP_TYPE_READWRITE)
    {
        GetCommandListManager()->ReadbackInitiated();
    }

    auto& CurrentState = pResource->m_Identity->m_currentState;
    assert(CurrentState.SupportsSimultaneousAccess() ||
           // Disabling simultaneous access for suballocated buffers but they're technically okay to map this way
           !pResource->GetIdentity()->m_bOwnsUnderlyingResource ||
           // 9on12 special case
           pResource->OwnsReadbackHeap() ||
           // For Map(DEFAULT) we should've made sure we're in common
           CurrentState.GetSubresourceState(Subresource).State == D3D12_RESOURCE_STATE_COMMON ||
           // Or we're not mapping the actual resource
           pResource->GetCurrentCpuHeap(Subresource) != nullptr);

    // We want to synchronize against the last command list to write to this subresource if
    // either the last op to it was a write (or in the same command list as a write),
    // or if we're only mapping it for read.
    auto& SubresourceState = CurrentState.GetSubresourceState(Subresource);
    UINT64 FenceValue = MapType == MAP_TYPE_READ ? SubresourceState.WriteFenceValue : SubresourceState.ReadFenceValue;
    return WaitForFenceValue(FenceValue, DoNotWait); // throws
}


//----------------------------------------------------------------------------------------------------------------------------------
bool ImmediateContext::WaitForFenceValue(UINT64 FenceValue, bool DoNotWait)
{
    if (DoNotWait)
    {
        if (FenceValue == GetCommandListID())
        {
            SubmitCommandList(); // throws on CommandListManager::CloseCommandList(...)
        }
        if (FenceValue > GetCompletedFenceValue())
        {
            return false;
        }
        return true;
    }
    else
    {
        return WaitForFenceValue(FenceValue); // throws
    }
}


//----------------------------------------------------------------------------------------------------------------------------------
bool ImmediateContext::Map(Resource* pResource, UINT Subresource, MAP_TYPE MapType, bool DoNotWait, _In_opt_ const D3D12_BOX *pReadWriteRange, MappedSubresource* pMappedSubresource)
{
    switch (pResource->AppDesc()->Usage())
    {
    case RESOURCE_USAGE_DEFAULT:
        return MapDefault(pResource, Subresource, MapType, false, pReadWriteRange, pMappedSubresource);
    case RESOURCE_USAGE_DYNAMIC:
        switch (MapType)
        {
            case MAP_TYPE_READ:
            case MAP_TYPE_READWRITE:
                if (pResource->m_creationArgs.m_heapDesc.Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
                {
                    return MapUnderlyingSynchronize(pResource, Subresource, MapType, DoNotWait, pReadWriteRange, pMappedSubresource);
                }
                else
                {
                    return MapDynamicTexture(pResource, Subresource, MapType, DoNotWait, pReadWriteRange, pMappedSubresource);
                }
            case MAP_TYPE_WRITE_NOOVERWRITE:
                assert(pResource->AppDesc()->CPUAccessFlags() == RESOURCE_CPU_ACCESS_WRITE);
                assert(pResource->AppDesc()->ResourceDimension() == D3D12_RESOURCE_DIMENSION_BUFFER);
                return MapUnderlying(pResource, Subresource, MapType, pReadWriteRange, pMappedSubresource);
            case MAP_TYPE_WRITE:
                assert(pResource->AppDesc()->ResourceDimension() == D3D12_RESOURCE_DIMENSION_BUFFER);
                if (pResource->m_creationArgs.m_heapDesc.Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
                {
                    return MapUnderlyingSynchronize(pResource, Subresource, MapType, DoNotWait, pReadWriteRange, pMappedSubresource);
                }
                else
                {
                    return MapDynamicTexture(pResource, Subresource, MapType, DoNotWait, pReadWriteRange, pMappedSubresource);
                }
        }
        break;
    case RESOURCE_USAGE_STAGING:
        return MapUnderlyingSynchronize(pResource, Subresource, MapType, DoNotWait, pReadWriteRange, pMappedSubresource);
    case RESOURCE_USAGE_IMMUTABLE:
    default:
        assert(false);
    }
    return false;
}

void ImmediateContext::Unmap(Resource* pResource, UINT Subresource, MAP_TYPE MapType, _In_opt_ const D3D12_BOX *pReadWriteRange)
{
    switch (pResource->AppDesc()->Usage())
    {
    case RESOURCE_USAGE_DEFAULT:
        UnmapDefault(pResource, Subresource, pReadWriteRange);
        break;
    case RESOURCE_USAGE_DYNAMIC:
        if (pResource->m_creationArgs.m_heapDesc.Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
        {
            UnmapUnderlyingSimple(pResource, Subresource, pReadWriteRange);
        }
        else
        {
            UnmapDynamicTexture(pResource, Subresource, pReadWriteRange, MapType != MAP_TYPE_READ);
        }
        break;
    case RESOURCE_USAGE_STAGING:
        UnmapUnderlyingStaging(pResource, Subresource, pReadWriteRange);
        break;
    case RESOURCE_USAGE_IMMUTABLE:
        assert(false);
        break;
    }
}


//----------------------------------------------------------------------------------------------------------------------------------
bool  ImmediateContext::MapDefault(Resource* pResource, UINT Subresource, MAP_TYPE MapType, bool DoNotWait, _In_opt_ const D3D12_BOX *pReadWriteRange, MappedSubresource* pMap )
{
    auto pResource12 = pResource->GetUnderlyingResource();

    if (pResource->Parent()->ResourceDimension12() != D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        m_ResourceStateManager.TransitionSubresource(pResource, Subresource, D3D12_RESOURCE_STATE_COMMON, SubresourceTransitionFlags::StateMatchExact | SubresourceTransitionFlags::NotUsedInCommandListIfNoStateChange);
        m_ResourceStateManager.ApplyAllResourceTransitions();
        if (MapType == MAP_TYPE_WRITE_NOOVERWRITE)
        {
            MapType = MAP_TYPE_WRITE;
        }
    }

    // Write-only means read range can be empty
    bool bWriteOnly = (MapType == MAP_TYPE_WRITE || MapType == MAP_TYPE_WRITE_NOOVERWRITE);
    D3D12_RANGE MappedRange = pResource->GetSubresourceRange(Subresource, pReadWriteRange);
    D3D12_RANGE ReadRange = bWriteOnly ? CD3DX12_RANGE(0, 0) : MappedRange;
    // If we know we are not reading, pass an empty range, otherwise pass a null (full) range
    D3D12_RANGE* pNonStandardReadRange = bWriteOnly ? &ReadRange : nullptr;

    assert(pResource->AppDesc()->Usage() == RESOURCE_USAGE_DEFAULT);
    bool bSynchronizationSucceeded = true;
    bool bSyncronizationNeeded = MapType != MAP_TYPE_WRITE_NOOVERWRITE;
    if (bSyncronizationNeeded)
    {
        bSynchronizationSucceeded = SynchronizeForMap(pResource, Subresource, MapType, DoNotWait);
    }

    if (bSynchronizationSucceeded)
    {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &Placement = pResource->GetSubresourcePlacement(Subresource);
        if (pResource->m_Identity->m_bPlacedTexture ||
            pResource->Parent()->ResourceDimension12() == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            // Map default row-major texture or buffer
            pResource12->Map(0, &ReadRange, &pMap->pData);

            pMap->pData = reinterpret_cast<BYTE *>(pMap->pData) + MappedRange.Begin;
            pMap->RowPitch = Placement.Footprint.RowPitch;
            pMap->DepthPitch = pResource->DepthPitch(Subresource);
        }
        else
        {
            // Opaque: Simply cache the map
            pResource12->Map(Subresource, pNonStandardReadRange, nullptr);
        }
    }
    return bSynchronizationSucceeded;
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::WriteToSubresource(Resource* pDstResource, UINT DstSubresource, _In_opt_ const D3D11_BOX* pDstBox, 
    const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch)
{
    pDstResource->GetUnderlyingResource()->WriteToSubresource(DstSubresource, reinterpret_cast<const D3D12_BOX*>(pDstBox), pSrcData, SrcRowPitch, SrcDepthPitch);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::ReadFromSubresource(void* pDstData, UINT DstRowPitch, UINT DstDepthPitch,
    Resource* pSrcResource, UINT SrcSubresource, _In_opt_ const D3D11_BOX* pSrcBox)
{
    pSrcResource->GetUnderlyingResource()->ReadFromSubresource(pDstData, DstRowPitch, DstDepthPitch, SrcSubresource, reinterpret_cast<const D3D12_BOX*>(pSrcBox));
}

//----------------------------------------------------------------------------------------------------------------------------------
bool  ImmediateContext::MapUnderlyingSynchronize(Resource* pResource, UINT Subresource, MAP_TYPE MapType, bool DoNotWait, _In_opt_ const D3D12_BOX *pReadWriteRange, MappedSubresource* pMap )
{
    bool bSynchronizeSucceeded = SynchronizeForMap(pResource, Subresource, MapType, DoNotWait);
    if (bSynchronizeSucceeded)
    {
        MapUnderlying(pResource, Subresource, MapType, pReadWriteRange, pMap);
    }
    return bSynchronizeSucceeded;
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::UnmapDefault(Resource* pResource, UINT Subresource, _In_opt_ const D3D12_BOX *pReadWriteRange)
{
    auto pResource12 = pResource->GetUnderlyingResource();

    if (pResource->m_Identity->m_bPlacedTexture)
    {
        Subresource = 0;
    }

    // No way to tell whether the map that is being undone was a READ, WRITE, or both, so key off CPU access flags
    // to determine if data could've been written by the CPU
    bool bCouldBeWritten = (pResource->AppDesc()->CPUAccessFlags() & RESOURCE_CPU_ACCESS_WRITE) != 0;
    bool bRowMajorPattern = pResource->m_Identity->m_bPlacedTexture ||
                            pResource->Parent()->ResourceDimension12() == D3D12_RESOURCE_DIMENSION_BUFFER;
    // If we couldn't have written anything, pass an empty range
    D3D12_RANGE WrittenRange = bCouldBeWritten ?
        pResource->GetSubresourceRange(Subresource, pReadWriteRange) : CD3DX12_RANGE(0, 0);
    // If we know how much we could've written, pass the range. If we know we didn't, pass an empty range. Otherwise, pass null.
    D3D12_RANGE* pWrittenRange = bRowMajorPattern || !bCouldBeWritten ?
        &WrittenRange : nullptr;
    pResource12->Unmap(Subresource, pWrittenRange);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::UnmapUnderlyingSimple(Resource* pResource, UINT Subresource, _In_opt_ const D3D12_BOX *pReadWriteRange)
{
    assert(pResource->AppDesc()->Usage() == RESOURCE_USAGE_DYNAMIC || pResource->AppDesc()->Usage() == RESOURCE_USAGE_STAGING);
    assert(pResource->OwnsReadbackHeap() || pResource->UnderlyingResourceIsSuballocated());
    
    auto pResource12 = pResource->GetUnderlyingResource();
    D3D12_RANGE WrittenRange = pResource->GetSubresourceRange(Subresource, pReadWriteRange);
    pResource12->Unmap(0, &WrittenRange);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::UnmapUnderlyingStaging(Resource* pResource, UINT Subresource, _In_opt_ const D3D12_BOX *pReadWriteRange)
{
    assert(pResource->AppDesc()->Usage() == RESOURCE_USAGE_DYNAMIC || pResource->AppDesc()->Usage() == RESOURCE_USAGE_STAGING);
    assert(pResource->OwnsReadbackHeap() ||  !pResource->m_Identity->m_bOwnsUnderlyingResource);

    auto pResource12 = pResource->GetUnderlyingResource();        

    // No way to tell whether the map that is being undone was a READ, WRITE, or both, so key off CPU access flags
    // to determine if data could've been written by the CPU. If we couldn't have written anything, pass an empty range
    bool bCouldBeWritten = (pResource->AppDesc()->CPUAccessFlags() & RESOURCE_CPU_ACCESS_WRITE) != 0;
    D3D12_RANGE WrittenRange = bCouldBeWritten ?
        pResource->GetSubresourceRange(Subresource, pReadWriteRange) : CD3DX12_RANGE(0, 0);

    pResource12->Unmap(0, &WrittenRange);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::UnmapDynamicTexture(Resource* pResource, UINT Subresource, _In_opt_ const D3D12_BOX *pReadWriteRange, bool bUploadMappedContents)
{
    UINT MipIndex, PlaneIndex, ArrayIndex;
    pResource->DecomposeSubresource(Subresource, MipIndex, ArrayIndex, PlaneIndex);

    assert(pResource->AppDesc()->Usage() == RESOURCE_USAGE_DYNAMIC);
    assert(pResource->GetCurrentCpuHeap(Subresource) != nullptr);

    Resource* pRenameResource = pResource->GetCurrentCpuHeap(Subresource);

    // If multiple planes of the dynamic texture were mapped simultaneously, only copy
    // data from the upload buffer once all planes have been unmapped.
    assert(pResource->GetDynamicTextureData(Subresource).m_MappedPlaneRefCount[PlaneIndex] > 0);
    pResource->GetDynamicTextureData(Subresource).m_MappedPlaneRefCount[PlaneIndex]--;
    if (bUploadMappedContents)
    {
        UnmapUnderlyingStaging(pRenameResource, PlaneIndex, pReadWriteRange);
    }
    else
    {
        UnmapUnderlyingSimple(pRenameResource, PlaneIndex, pReadWriteRange);
    }

    if (pResource->GetDynamicTextureData(Subresource).AnyPlaneMapped())
    {
        return;
    }

    if(bUploadMappedContents)
    {
        UINT DstX = pReadWriteRange ? pReadWriteRange->left : 0u;
        UINT DstY = pReadWriteRange ? pReadWriteRange->top : 0u;
        UINT DstZ = pReadWriteRange ? pReadWriteRange->front : 0u;

        // Copy each plane.
        for (UINT iPlane = 0; iPlane < pResource->AppDesc()->NonOpaquePlaneCount(); ++iPlane)
        {
            if (   pResource->Parent()->ResourceDimension12() == D3D12_RESOURCE_DIMENSION_BUFFER
                || pResource->GetDynamicTextureData(Subresource).m_DirtyPlaneMask & (1 << iPlane))
            {
                UINT planeSubresource = pResource->GetSubresourceIndex(iPlane, MipIndex, ArrayIndex);
                ResourceCopyRegion(pResource, planeSubresource, DstX, DstY, DstZ, pRenameResource, iPlane, pReadWriteRange);
            }
        }

        pResource->GetDynamicTextureData(Subresource).m_DirtyPlaneMask = 0;
    }

    pResource->SetCurrentCpuHeap(Subresource, nullptr);
}

//----------------------------------------------------------------------------------------------------------------------------------
HRESULT  ImmediateContext::CheckFormatSupport(_Out_ D3D12_FEATURE_DATA_FORMAT_SUPPORT& formatData)
{
    return m_pDevice12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatData, sizeof(formatData));
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CheckMultisampleQualityLevels(DXGI_FORMAT format, UINT SampleCount, D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS Flags, _Out_ UINT* pNumQualityLevels )
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS SupportStruct;
    SupportStruct.Format = format;
    SupportStruct.SampleCount = SampleCount;
    SupportStruct.Flags = Flags;
    HRESULT hr = m_pDevice12->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &SupportStruct, sizeof(SupportStruct));

    *pNumQualityLevels = SUCCEEDED(hr) ? SupportStruct.NumQualityLevels : 0;
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CheckFeatureSupport(D3D12_FEATURE Feature, _Inout_updates_bytes_(FeatureSupportDataSize)void* pFeatureSupportData, UINT FeatureSupportDataSize)
{
    ThrowFailure(m_pDevice12->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize));
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CopyDataToBuffer(
    ID3D12Resource* pDstResource,
    UINT DstOffset,
    const void* pData,
    UINT Size
    ) noexcept(false)
{
    const UINT AlignedSize = 1024; // To ensure good pool re-use
    assert(Size <= AlignedSize);

    auto UploadHeap = AcquireSuballocatedHeap(AllocatorHeapType::Upload, AlignedSize, ResourceAllocationContext::ImmediateContextThreadTemporary); // throw( _com_error )

    void* pMapped;
    CD3DX12_RANGE ReadRange(0, 0);
    HRESULT hr = UploadHeap.Map(0, &ReadRange, &pMapped);
    ThrowFailure(hr); // throw( _com_error )

    memcpy(pMapped, pData, Size);

    CD3DX12_RANGE WrittenRange(0, Size);
    UploadHeap.Unmap(0, &WrittenRange);

    GetGraphicsCommandList()->CopyBufferRegion(
        pDstResource,
        DstOffset,
        UploadHeap.GetResource(),
        UploadHeap.GetOffset(),
        Size
        );

    AdditionalCommandsAdded();
    ReleaseSuballocatedHeap(
        AllocatorHeapType::Upload, 
        UploadHeap,
        GetCommandListID());
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::TransitionResourceForView(ViewBase* pView, D3D12_RESOURCE_STATES desiredState) noexcept
{
    m_ResourceStateManager.TransitionSubresources(pView->m_pResource, pView->m_subresources, desiredState);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::CreateSharedNTHandle(_In_ Resource *pResource, _Out_ HANDLE *pHandle, _In_opt_ SECURITY_ATTRIBUTES *pSA)
{
    assert(pResource->Parent()->IsNTHandleShared()); // Note: Not validated by this layer, but only called when this is true.

    ThrowFailure(m_pDevice12->CreateSharedHandle(pResource->GetUnderlyingResource(), pSA, GENERIC_ALL, nullptr, pHandle));
}

//----------------------------------------------------------------------------------------------------------------------------------
bool RetiredObject::ReadyToDestroy(ImmediateContext* pContext, UINT64 lastCommandListID)
{
    return lastCommandListID <= pContext->GetCompletedFenceValue();
}

//----------------------------------------------------------------------------------------------------------------------------------
PipelineState* ImmediateContext::GetPipelineState()
{
    return m_CurrentState.m_pPSO;
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::SetPipelineState(PipelineState* pPipeline)
{
    if (!m_CurrentState.m_pPSO || !pPipeline ||
         m_CurrentState.m_pPSO->GetRootSignature() != pPipeline->GetRootSignature())
    {
        m_DirtyStates |= e_ComputeRootSignatureDirty;
    }

    m_CurrentState.m_pPSO = pPipeline;
    m_DirtyStates |= e_PipelineStateDirty;
}

//----------------------------------------------------------------------------------------------------------------------------------
DXGI_FORMAT ImmediateContext::GetParentForFormat(DXGI_FORMAT format)
{
    return CD3D11FormatHelper::GetParentFormat(format);
};

//----------------------------------------------------------------------------------------------------------------------------------
HRESULT  ImmediateContext::GetDeviceState()
{
    return m_pDevice12->GetDeviceRemovedReason();
}

//----------------------------------------------------------------------------------------------------------------------------------
 void ImmediateContext::Signal(
    _In_ Fence* pFence,
    UINT64 Value
    )
{
    Flush();
    ThrowFailure(GetCommandQueue()->Signal(pFence->Get(), Value));
    pFence->UsedInCommandList(GetCommandListID() - 1);
}

//----------------------------------------------------------------------------------------------------------------------------------
 void ImmediateContext::Wait(
    std::shared_ptr<Fence> const& pFence,
    UINT64 Value
    )
{
    Flush();
    auto pQueue = GetCommandQueue();
    if (pQueue)
    {
        ThrowFailure(pQueue->Wait(pFence->Get(), Value));
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
unique_comptr<ID3D12Resource> ImmediateContext::AllocateHeap(UINT64 HeapSize, UINT64 alignment, AllocatorHeapType heapType) noexcept(false)
{
    D3D12_HEAP_PROPERTIES Props = GetHeapProperties(GetD3D12HeapType(heapType));
    D3D12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(HeapSize, D3D12_RESOURCE_FLAG_NONE, alignment);
    unique_comptr<ID3D12Resource> spResource;
    HRESULT hr = m_pDevice12->CreateCommittedResource(
        &Props,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        GetDefaultPoolState(heapType),
        nullptr,
        IID_PPV_ARGS(&spResource));
    ThrowFailure(hr); // throw( _com_error )

    // Cache the Map within the D3D12 resource.
    CD3DX12_RANGE NullRange(0, 0);
    void* pData = nullptr;
    ThrowFailure(spResource->Map(0, &NullRange, &pData));

    return std::move(spResource);
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::ClearState() noexcept
{
    m_CurrentState.ClearState();

    m_DirtyStates |= e_DirtyOnFirstCommandList;
    m_StatesToReassert |= e_ReassertOnNewCommandList;
}

//----------------------------------------------------------------------------------------------------------------------------------
void ImmediateContext::SharingContractPresent(_In_ Resource* pResource)
{
    Flush();

    auto pSharingContract = GetCommandListManager()->GetSharingContract();
    if (pSharingContract)
    {
        ID3D12Resource* pUnderlying = pResource->GetUnderlyingResource();
        pSharingContract->Present(pUnderlying, 0, nullptr);
    }

    pResource->UsedInCommandList(GetCommandListID());
    GetCommandListManager()->SetNeedSubmitFence();
}

}
