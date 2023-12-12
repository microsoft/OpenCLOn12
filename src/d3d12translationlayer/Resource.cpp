// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

namespace D3D12TranslationLayer
{
    //==================================================================================================================================
    // AppResourceDesc
    //==================================================================================================================================
    //----------------------------------------------------------------------------------------------------------------------------------
    AppResourceDesc::AppResourceDesc(const D3D12_RESOURCE_DESC &desc12, 
        D3D12TranslationLayer::RESOURCE_USAGE Usage, 
        DWORD Access, 
        DWORD BindFlags)
    {
        UINT16 depth = desc12.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc12.DepthOrArraySize;
        UINT16 arraySize = desc12.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc12.DepthOrArraySize;
        UINT8 nonOpaquePlaneCount = (UINT8)CD3D11FormatHelper::NonOpaquePlaneCount(desc12.Format);
        UINT numSubresources = desc12.MipLevels * arraySize * nonOpaquePlaneCount;
        *this = AppResourceDesc(
            desc12.MipLevels * arraySize,
            nonOpaquePlaneCount,
            numSubresources,
            (UINT8)desc12.MipLevels,
            arraySize,
            depth,
            (UINT)desc12.Width,
            (UINT)desc12.Height,
            desc12.Format,
            desc12.SampleDesc.Count,
            desc12.SampleDesc.Quality,
            Usage,
            (D3D12TranslationLayer::RESOURCE_CPU_ACCESS)Access,
            (D3D12TranslationLayer::RESOURCE_BIND_FLAGS)BindFlags,
            desc12.Dimension);
    }

    //==================================================================================================================================
    // Resource
    //==================================================================================================================================
    //----------------------------------------------------------------------------------------------------------------------------------

    void Resource::Create(ResourceAllocationContext threadingContext)  noexcept(false)
    {
        bool& ownsResource = m_Identity->m_bOwnsUnderlyingResource;
        ownsResource = (m_creationArgs.m_heapDesc.Properties.Type == D3D12_HEAP_TYPE_DEFAULT ||
            m_creationArgs.m_heapDesc.Properties.Type == D3D12_HEAP_TYPE_CUSTOM ||
            OwnsReadbackHeap() ||
            (AppDesc()->Usage() == RESOURCE_USAGE_DYNAMIC && (Parent()->ResourceDimension12() != D3D12_RESOURCE_DIMENSION_BUFFER)));

        // Create/retrieve the resource
        if (ownsResource)
        {
            CreateUnderlying(threadingContext); // throw( _com_error, bad_alloc )
        }
        else
        {
            m_Identity->m_suballocation = m_pParent->AcquireSuballocatedHeapForResource(this, threadingContext);
            for (auto& placement : m_SubresourcePlacement)
            {
                placement.Offset += m_Identity->GetSuballocatedOffset();
            }

            // Note: This clear routine simply releases the container's references on the preallocated memory,
            // and destructs any objects contained within. It leaves the memory allocated.
            //
            // In the future, we may want to revisit the allocation routines below with understanding of the
            // "ownsResource" computation above, either recalculating it to compute/initialize the preallocated arrays,
            // or precalculate it prior to allocating and initializing the Resource class.
            m_DynamicTexturePlaneData.clear();
            m_spCurrentCpuHeaps.clear();
        }

        // Update the state
        UnderlyingResourceChanged(); // throw( _com_error )
        m_isValid = true;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    inline UINT8 GetSubresourceMultiplier(ResourceCreationArgs const& createArgs) noexcept
    {
        return CD3D11FormatHelper::FamilySupportsStencil(createArgs.m_appDesc.Format()) ?
            2 : 1;
    }

    inline UINT GetTotalSubresources(ResourceCreationArgs const& createArgs) noexcept
    {
        return createArgs.m_appDesc.Subresources() * GetSubresourceMultiplier(createArgs);
    }

    inline UINT GetSubresourcesForTransitioning(ResourceCreationArgs const& createArgs) noexcept
    {
        return (createArgs.m_appDesc.Usage() == RESOURCE_USAGE_STAGING || createArgs.ApiTextureLayout12() == D3D12_TEXTURE_LAYOUT_ROW_MAJOR) ?
            1u : GetTotalSubresources(createArgs);
    }

    inline UINT GetSubresourcesForDynamicTexturePlaneData(ResourceCreationArgs const& createArgs) noexcept
    {
        return (createArgs.m_appDesc.Usage() == D3D11_USAGE_DYNAMIC) ?
            createArgs.m_appDesc.Subresources() / createArgs.m_appDesc.NonOpaquePlaneCount() : 0u;
    }

    inline UINT GetSubresourcesForCpuHeaps(ResourceCreationArgs const& createArgs) noexcept
    {
        return createArgs.m_appDesc.CPUAccessFlags() ?
            GetSubresourcesForDynamicTexturePlaneData(createArgs) : 0u;
    }

    inline bool IsSimultaneousAccess(ResourceCreationArgs const& createArgs) noexcept
    {
        // TODO: Some resource types add in simultaneous access during CreateUnderlying
        // We should refactor so that the desc doesn't change after construction...
        return ((createArgs.m_desc12.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != D3D12_RESOURCE_FLAG_NONE ||
                createArgs.m_desc12.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) &&
                (createArgs.m_appDesc.Usage() == D3D11_USAGE_DEFAULT ||
                 createArgs.m_appDesc.Usage() == D3D11_USAGE_IMMUTABLE);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    size_t Resource::CalcPreallocationSize(ResourceCreationArgs const& createArgs)
    {
        auto SubresourceCount = GetTotalSubresources(createArgs);
        return sizeof(Resource) +
            TransitionableResourceBase::CalcPreallocationSize(GetSubresourcesForTransitioning(createArgs)) +
            sizeof(unique_comptr<Resource>) * GetSubresourcesForCpuHeaps(createArgs) +
            sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) * SubresourceCount +
            sizeof(UINT64) * GetSubresourcesForCpuHeaps(createArgs) +
            sizeof(DynamicTexturePlaneData) * GetSubresourcesForDynamicTexturePlaneData(createArgs);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    Resource::Resource(ImmediateContext* pDevice, ResourceCreationArgs& createArgs, void*& pPreallocatedMemory) noexcept(false)
        : DeviceChild(pDevice)
        , TransitionableResourceBase(GetSubresourcesForTransitioning(createArgs), pPreallocatedMemory)
        , m_SubresourceMultiplier(GetSubresourceMultiplier(createArgs))
        , m_creationArgs(createArgs)
        , m_Identity(AllocateResourceIdentity(NumSubresources(), IsSimultaneousAccess(createArgs))) // throw( bad_alloc )
        , m_spCurrentCpuHeaps(GetSubresourcesForCpuHeaps(createArgs), pPreallocatedMemory)
        , m_SubresourcePlacement(NumSubresources(), pPreallocatedMemory)
        , m_LastCommandListID(GetSubresourcesForCpuHeaps(createArgs), pPreallocatedMemory)
        , m_DynamicTexturePlaneData(GetSubresourcesForDynamicTexturePlaneData(createArgs), pPreallocatedMemory)
    {
        m_effectiveUsage = AppDesc()->Usage();

        // Default row-major textures are unsupported by D3D12 (except for cross-adapter), therefore
        // they are emulated using buffers and should typically be handled similarly to staging textures.
        if (m_effectiveUsage == D3D11_USAGE_DEFAULT &&
            Parent()->ResourceDimension12() != D3D12_RESOURCE_DIMENSION_BUFFER &&
            Parent()->ApiTextureLayout12() == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
        {
            m_effectiveUsage = RESOURCE_USAGE_STAGING;
        }

        InitializeSubresourceDescs(); // throw( _com_error )
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    unique_comptr<Resource> Resource::AllocateResource(ImmediateContext* pDevice, ResourceCreationArgs& createArgs)
    {
        struct VoidDeleter { void operator()(void* p) { operator delete(p); } };

        size_t ObjectSize = CalcPreallocationSize(createArgs);
        std::unique_ptr<void, VoidDeleter> spMemory(operator new(ObjectSize));

        void* pPreallocatedMemory = reinterpret_cast<Resource*>(spMemory.get()) + 1;
        unique_comptr<Resource> spResource(new (spMemory.get()) Resource(pDevice, createArgs, pPreallocatedMemory));
        spMemory.release();

        // This can fire if CalcPreallocationSize doesn't account for all preallocated arrays, or if the void*& decays to void*
        assert(reinterpret_cast<size_t>(pPreallocatedMemory) == reinterpret_cast<size_t>(spResource.get()) + ObjectSize);

        return std::move(spResource);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    unique_comptr<Resource> Resource::CreateResource(ImmediateContext* pDevice, ResourceCreationArgs& createArgs, ResourceAllocationContext threadingContext)
    {
        auto spResource = AllocateResource(pDevice, createArgs);
        spResource->Create(threadingContext);
        return std::move(spResource);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    Resource::~Resource() noexcept
    {
        if (!m_isValid)
        {
            m_Identity.reset(nullptr);
        }
        m_pParent->ReturnAllBuffersToPool(*this);

        // If this resource was ever valid, ensure that it either still has its owned heap/resource, or that it returned its borrowed resource
        assert(m_Identity.get() == nullptr || // The identity is deleted early in the case of failed initialization
            m_Identity->m_bOwnsUnderlyingResource == (m_Identity->m_spUnderlyingResource.get() != nullptr));

        if (m_Identity)
        {
            if (m_Identity->m_bOwnsUnderlyingResource)
            {
                if (m_Identity->GetOwnedResource())
                {
                    m_pParent->AddResourceToDeferredDeletionQueue(GetIdentity()->GetOwnedResource(),
                        std::move(m_Identity->m_pResidencyHandle),
                        m_LastUsedCommandListID);
                }
            }
        }

        m_isValid = false; // Tag deleted resources for easy inspection in a debugger
    }

    void Resource::AddToResidencyManager(bool bIsResident)
    {
        if (m_Identity 
            && !m_Identity->m_bSharedResource
            && m_Identity->m_bOwnsUnderlyingResource
            && m_Identity->GetOwnedResource() != nullptr)
        {
            auto &residencyManager = m_pParent->GetResidencyManager();

            m_Identity->m_pResidencyHandle = std::unique_ptr<ResidencyManagedObjectWrapper>(new ResidencyManagedObjectWrapper(residencyManager));
            D3D12_RESOURCE_DESC resourceDesc12 = m_creationArgs.m_desc12;
            D3D12_RESOURCE_ALLOCATION_INFO allocInfo = m_pParent->m_pDevice12->GetResourceAllocationInfo(1, 1, &resourceDesc12);

            m_Identity->m_pResidencyHandle->Initialize(m_Identity->GetResource(), allocInfo.SizeInBytes, bIsResident);
        }
    }

    class SwapChainAssistant : public ID3D12SwapChainAssistant
    {
    private: ~SwapChainAssistant() = default;
    public:
        SwapChainAssistant()
        {
            if (!AllocateLocallyUniqueId(&m_LUID))
                throw std::bad_alloc();
        }
        STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&m_RefCount); }
        STDMETHOD_(ULONG, Release)() { ULONG ret = InterlockedDecrement(&m_RefCount); if (ret == 0) delete this; return ret; }
        STDMETHOD(QueryInterface)(REFIID riid, void** ppv)
        {
            if (InlineIsEqualGUID(riid, __uuidof(ID3D12SwapChainAssistant)) ||
                InlineIsEqualUnknown(riid))
            {
                AddRef();
                *ppv = this;
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        STDMETHOD_(LUID, GetLUID)() { return m_LUID; }
        STDMETHOD(GetSwapChainObject)(REFIID, void**) { return E_NOTIMPL; }
        STDMETHOD(GetCurrentResourceAndCommandQueue)(REFIID, void**, REFIID, void**) { return E_NOTIMPL; }
        STDMETHOD(InsertImplicitSync)() { return E_NOTIMPL; }
        volatile ULONG m_RefCount = 0;
        LUID m_LUID;
    };

    //----------------------------------------------------------------------------------------------------------------------------------
    void Resource::CreateUnderlying(ResourceAllocationContext threadingContext) noexcept(false)
    {
        assert(m_Identity->m_bOwnsUnderlyingResource);

        D3D12_RESOURCE_DESC& Desc12 = m_creationArgs.m_desc12;
        D3D12_HEAP_DESC& HeapDesc = m_creationArgs.m_heapDesc;
        D3D11_RESOURCE_FLAGS& Flags11 = m_creationArgs.m_flags11;

        if (AppDesc()->Usage() == RESOURCE_USAGE_DYNAMIC && (Desc12.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || IsLockableSharedBuffer()))
        {
            HeapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        }

        if (HeapDesc.Properties.Type != D3D12_HEAP_TYPE_CUSTOM)
        {
            HeapDesc.Properties = m_pParent->GetHeapProperties(HeapDesc.Properties.Type);
        }

        // D3D11 allowed SRV-only MSAA resources, but D3D12 requires RTV/DSV
        if (Desc12.SampleDesc.Count > 1 && (Desc12.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) == 0)
        {
            Desc12.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }

        if (Desc12.Format == DXGI_FORMAT_420_OPAQUE) // Feature_D3D1XDisplayable
        {
            // 420_OPAQUE doesn't exist in D3D12.
            Desc12.Format = DXGI_FORMAT_NV12; 
        }

        HRESULT hr = S_OK;

        // All sharing must use this API in order to enable in-API sharing
        bool bCompatibilityCreateRequired = m_creationArgs.IsShared();

        // True if simultaneous access will pass D3D12 validation
        bool bSupportsSimultaneousAccess =
            (Flags11.BindFlags & D3D11_BIND_DEPTH_STENCIL) == 0 &&
            Desc12.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            Desc12.SampleDesc.Count == 1 && Desc12.SampleDesc.Quality == 0;

        if (bCompatibilityCreateRequired || !m_creationArgs.m_bManageResidency)
        {
            HeapDesc.Flags &= ~D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT;
        }

        m_pParent->TryAllocateResourceWithFallback([&]()
        {
            if (m_creationArgs.m_PrivateCreateFn)
            {
                m_creationArgs.m_PrivateCreateFn(m_creationArgs, &m_Identity->m_spUnderlyingResource);
            }
            else if (bCompatibilityCreateRequired)
            {
                // Shared resources should use simultaneous access whenever possible
                // For future designs: Shared resources that cannot use simultaneous access should, in theory, transition to COMMON at every flush
                Desc12.Flags
                    |= (bSupportsSimultaneousAccess
                        && !(Flags11.MiscFlags & 0x200000) // Feature_D3D1XDisplayable : RESOURCE_MISC_SHARED_EXCLUSIVE_WRITER
                        )
                    ? D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
                    : D3D12_RESOURCE_FLAG_NONE
                    ;
                assert((HeapDesc.Flags & D3D12_HEAP_FLAG_SHARED) != 0);

                D3D12_COMPATIBILITY_SHARED_FLAGS CompatFlags =
                    ((m_creationArgs.IsNTHandleShared()) ? D3D12_COMPATIBILITY_SHARED_FLAG_NONE : D3D12_COMPATIBILITY_SHARED_FLAG_NON_NT_HANDLE) |
                    ((m_creationArgs.m_flags11.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) ? D3D12_COMPATIBILITY_SHARED_FLAG_KEYED_MUTEX : D3D12_COMPATIBILITY_SHARED_FLAG_NONE);

                hr = m_pParent->m_pCompatDevice->CreateSharedResource(
                    &HeapDesc.Properties,
                    HeapDesc.Flags,
                    &Desc12,
                    D3D12_RESOURCE_STATE_COMMON,
                    nullptr, // Clear values
                    &Flags11,
                    CompatFlags,
                    nullptr,
                    nullptr,
                    IID_PPV_ARGS(&m_Identity->m_spUnderlyingResource));
            }
            else
            {
                hr = m_pParent->m_pDevice12->CreateCommittedResource(
                    &HeapDesc.Properties,
                    HeapDesc.Flags,
                    &Desc12,
                    D3D12_RESOURCE_STATE_COMMON,
                    nullptr, // Clear values
                    IID_PPV_ARGS(&m_Identity->m_spUnderlyingResource));
            }
            ThrowFailure(hr); // throw( _com_error )
        }, threadingContext);

        if (!bCompatibilityCreateRequired &&
            m_creationArgs.m_bManageResidency)
        {
            AddToResidencyManager((HeapDesc.Flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT) == D3D12_HEAP_FLAG_NONE);
        }
    }

    ManagedObject *Resource::GetResidencyHandle()
    {
        ManagedObject *pObject = nullptr;
        if (m_Identity && m_Identity->m_pResidencyHandle)
        {
            pObject = &m_Identity->m_pResidencyHandle->GetManagedObject();
        }
        return pObject;
    }

    void Resource::UsedInCommandList(UINT64 id)
    {
        m_pParent->AddObjectToResidencySet(this);
        DeviceChild::UsedInCommandList(id);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void Resource::UnderlyingResourceChanged() noexcept(false)
    {
        // Tile pools don't have a resource
        if (!m_Identity->GetResource())
        {
            return;
        }

        m_Identity->m_currentState.Reset();

        // For resources which own their underlying subresource, they're created in COMMON.
        if (!m_Identity->m_bOwnsUnderlyingResource)
        {
            CCurrentResourceState::SubresourceState State = { 0ull, 0ull, GetDefaultPoolState(GetAllocatorHeapType()) };
            m_Identity->m_currentState.SetResourceState(State);
        }

        ResetLastUsedInCommandList();

        m_DesiredState.Reset();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void Resource::ZeroConstantBufferPadding() noexcept
    {
        // Determine if we need to do any work
        if ((AppDesc()->BindFlags() & RESOURCE_BIND_CONSTANT_BUFFER) == 0 || // The only buffers that are bloated
            AppDesc()->Width() % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0 || // Only if it's actually bloated
            m_Identity->m_bOwnsUnderlyingResource) // Default constant buffers are handled separately
        {
            return;
        }

        UINT64 AlignedSize = m_SubresourcePlacement[0].Footprint.RowPitch;
        SIZE_T ZeroSize = static_cast<SIZE_T>(AlignedSize - AppDesc()->Width());
        void *pData;
        CD3DX12_RANGE ReadRange(0, 0);
        HRESULT hr = GetUnderlyingResource()->Map(0, &ReadRange, &pData);
        if (SUCCEEDED(hr))
        {
            BYTE* pZeroAddr = reinterpret_cast<BYTE*>(pData)+m_SubresourcePlacement[0].Offset + AppDesc()->Width();
            ZeroMemory(pZeroAddr, ZeroSize);

            CD3DX12_RANGE WrittenRange(SIZE_T(m_SubresourcePlacement[0].Offset) + AppDesc()->Width(), 0);
            WrittenRange.End = WrittenRange.Begin + ZeroSize;
            GetUnderlyingResource()->Unmap(0, &WrittenRange);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Resource::GetSubresourcePlacement(UINT subresource) noexcept
    {
        return m_SubresourcePlacement[subresource];
    }

    void Resource::UpdateAppDesc(const AppResourceDesc &AppDesc)
    {
        m_creationArgs.m_appDesc = AppDesc;
        InitializeSubresourceDescs();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void Resource::InitializeSubresourceDescs() noexcept(false)
    {
        m_Identity->m_bPlacedTexture = m_creationArgs.m_isPlacedTexture;
        CD3DX12_RESOURCE_DESC ResDesc(
            AppDesc()->ResourceDimension(),
            0,
            AppDesc()->Width(),
            AppDesc()->Height(),
            static_cast<UINT16>(AppDesc()->ResourceDimension() == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? AppDesc()->Depth() : AppDesc()->ArraySize()),
            AppDesc()->MipLevels(),
            AppDesc()->Format(),
            1,
            0,
            AppDesc()->ResourceDimension() == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN,
            D3D12_RESOURCE_FLAG_NONE);
        ID3D12Device* pDevice = m_pParent->m_pDevice12.get();

        pDevice->GetCopyableFootprints(&ResDesc, 0, NumSubresources(), 0, &m_SubresourcePlacement[0], nullptr, nullptr, nullptr);
        
        if (m_SubresourcePlacement[0].Footprint.RowPitch == -1)
        {
            ThrowFailure(E_INVALIDARG);
        }
        
        if (NumSubresources() > 1)
        {
            // Offset produced by GetCopyableFootprints is ignored, because 11on12 chooses a different layout in the buffer than D3D12
            // Subresource order in D3D12 is Plane -> Array -> Mip
            // However, for CPU-accessible data, the desired memory layout is Array -> Plane -> Mip
            UINT LastSubresource = 0;
            const UINT ArraySize = AppDesc()->ArraySize(),
                PlaneCount = AppDesc()->NonOpaquePlaneCount() * m_SubresourceMultiplier,
                MipLevels = AppDesc()->MipLevels();
            for (UINT ArraySlice = 0; ArraySlice < ArraySize; ++ArraySlice)
            {
                for (UINT PlaneSlice = 0; PlaneSlice < PlaneCount; ++PlaneSlice)
                {
                    for (UINT MipLevel = 0; MipLevel < MipLevels; ++MipLevel)
                    {
                        auto& LastPlacement = GetSubresourcePlacement(LastSubresource);

                        UINT CurrentSubresource = ComposeSubresourceIdxExtended(MipLevel, ArraySlice, PlaneSlice, MipLevels, ArraySize);

                        if (CurrentSubresource != 0)
                        {
                            GetSubresourcePlacement(CurrentSubresource).Offset =
                                Align<UINT64>(LastPlacement.Offset + DepthPitch(LastSubresource) * LastPlacement.Footprint.Depth, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                        }

                        LastSubresource = CurrentSubresource;
                    }
                }
            }
        }
        else
        {
            // D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT is used to ensure that constant buffers are multiples of 256 bytes in size
            m_SubresourcePlacement[0].Footprint.RowPitch =
                Align<UINT>(m_SubresourcePlacement[0].Footprint.RowPitch, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        }
    }

    void Resource::FillSubresourceDesc(ID3D12Device* pDevice, DXGI_FORMAT Format, UINT PlaneWidth, UINT PlaneHeight, UINT Depth, _Out_ D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Placement) noexcept
    {
        CD3DX12_RESOURCE_DESC ResourceDesc(
            D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            0, // Alignment
            PlaneWidth, PlaneHeight, static_cast<UINT16>(Depth),
            1, // Array size
            Format,
            1, // Sample count
            0, // Sample quality
            D3D12_TEXTURE_LAYOUT_UNKNOWN,
            D3D12_RESOURCE_FLAG_NONE
            );
        if (Format == DXGI_FORMAT_UNKNOWN)
        {
            ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        }
        else if (Depth > 1)
        {
            ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        }
        pDevice->GetCopyableFootprints(&ResourceDesc, 0, 1, 0, &Placement, nullptr, nullptr, nullptr);

        // D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT is used to ensure that constant buffers are multiples of 256 bytes in size
        Placement.Footprint.RowPitch = Align<UINT>(Placement.Footprint.RowPitch, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT Resource::DepthPitch(UINT Subresource) noexcept
    {
        auto& Placement = GetSubresourcePlacement(Subresource).Footprint;
        assert(Placement.Width > 0 && Placement.Height > 0 && Placement.Depth > 0 && Placement.RowPitch > 0);

        // The placement format should be in the same cast set as the resource format unless the format is planar.
        // Also, DepthStencil formats are planar in 12 only and need to be checked seperately.
        assert(m_pParent->GetParentForFormat(AppDesc()->Format()) == CD3D11FormatHelper::GetParentFormat(Placement.Format)
            || CD3D11FormatHelper::Planar(m_pParent->GetParentForFormat(AppDesc()->Format()))
            || CD3D11FormatHelper::FamilySupportsStencil(m_pParent->GetParentForFormat(AppDesc()->Format())));

        UINT SlicePitch;
        CD3D11FormatHelper::CalculateMinimumRowMajorSlicePitch(Placement.Format, Placement.RowPitch, Placement.Height, SlicePitch);

        return SlicePitch;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    D3D12_RANGE Resource::GetSubresourceRange(UINT Subresource, _In_opt_ const D3D12_BOX *pSelectedBox) noexcept
    {
        auto& SubresourceInfo = GetSubresourcePlacement(Subresource);
        UINT TightRowPitch;
        CD3D11FormatHelper::CalculateMinimumRowMajorRowPitch(SubresourceInfo.Footprint.Format, SubresourceInfo.Footprint.Width, TightRowPitch);

        SIZE_T StartOffset = SIZE_T(SubresourceInfo.Offset);

        SIZE_T Size = DepthPitch(Subresource) * (SubresourceInfo.Footprint.Depth - 1) + TightRowPitch;
        if (pSelectedBox)
        {
            if (Parent()->m_desc12.Format == DXGI_FORMAT_UNKNOWN)
            {
                assert(Parent()->m_desc12.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
                assert(Subresource == 0);

                StartOffset += pSelectedBox->left;
                Size = pSelectedBox->right - pSelectedBox->left;
            }
            else
            {
                const UINT BITS_PER_BYTE = 8;
                const UINT BytesPerPixel = CD3D11FormatHelper::GetBitsPerUnit(Parent()->m_desc12.Format) / BITS_PER_BYTE;
                const UINT SlicePitch = SubresourceInfo.Footprint.Height * SubresourceInfo.Footprint.RowPitch;
                StartOffset += pSelectedBox->left * BytesPerPixel + pSelectedBox->top * SubresourceInfo.Footprint.RowPitch + pSelectedBox->front * SlicePitch;


                UINT DepthSize = (pSelectedBox->back - pSelectedBox->front - 1) * DepthPitch(Subresource);
                UINT HeightSize = (pSelectedBox->bottom - pSelectedBox->top - 1) * SubresourceInfo.Footprint.RowPitch;
                UINT RowSize = (pSelectedBox->right - pSelectedBox->left) * BytesPerPixel;
                Size = DepthSize + HeightSize + RowSize;
            }
        }

        return CD3DX12_RANGE(StartOffset, StartOffset + Size);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT64 Resource::GetResourceSize() noexcept
    {
        UINT Subresource = NumSubresources() - 1;
        auto& LastSubresourcePlacement = GetSubresourcePlacement(Subresource);
        UINT64 TotalSize = DepthPitch(Subresource) * LastSubresourcePlacement.Footprint.Depth + (LastSubresourcePlacement.Offset - m_SubresourcePlacement[0].Offset);
        return TotalSize;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    D3D12_HEAP_TYPE Resource::GetD3D12HeapType(RESOURCE_USAGE usage, UINT cpuAccessFlags) noexcept
    {
        if (usage == RESOURCE_USAGE_DEFAULT || usage == RESOURCE_USAGE_IMMUTABLE)
        {
            return D3D12_HEAP_TYPE_DEFAULT;
        }
        else
        {
            assert(usage == RESOURCE_USAGE_DYNAMIC || usage == RESOURCE_USAGE_STAGING);

            // Using GetCustomHeapProperties with Readback heaps allows for both reading
            // and writing to the the heap so choose this heap if the cpu access is read,
            // regardless of whether there's also CPU write access
            if (cpuAccessFlags & RESOURCE_CPU_ACCESS_READ)
                return D3D12_HEAP_TYPE_READBACK;
            if (cpuAccessFlags & RESOURCE_CPU_ACCESS_WRITE)
                return D3D12_HEAP_TYPE_UPLOAD;

            return D3D12_HEAP_TYPE_DEFAULT;
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    Resource* Resource::GetCurrentCpuHeap(UINT Subresource) 
    {
        if (m_spCurrentCpuHeaps.size() == 0) 
        {
            return nullptr;
        }
        
        UINT DynamicTextureIndex = GetDynamicTextureIndex(Subresource);
        return m_spCurrentCpuHeaps[DynamicTextureIndex].get();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void Resource::SetCurrentCpuHeap(UINT subresource, Resource* UploadHeap)
    { 
        UINT DynamicTextureIndex = GetDynamicTextureIndex(subresource);
        m_spCurrentCpuHeaps[DynamicTextureIndex] = UploadHeap;
    }
};
