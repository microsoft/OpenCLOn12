// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
namespace D3D12TranslationLayer
{

//----------------------------------------------------------------------------------------------------------------------------------
template<> inline LIST_ENTRY& CResourceBindings::GetViewList<ShaderResourceViewType>() { return m_ShaderResourceViewList; }
template<> inline LIST_ENTRY& CResourceBindings::GetViewList<RenderTargetViewType>() { return m_RenderTargetViewList; }
template<> inline LIST_ENTRY& CResourceBindings::GetViewList<UnorderedAccessViewType>() { return m_UnorderedAccessViewList; }

template<> inline CSubresourceBindings::BindFunc CResourceBindings::GetBindFunc<ShaderResourceViewType>(EShaderStage stage) { return &CSubresourceBindings::NonPixelShaderResourceViewBound; }
template<> inline CSubresourceBindings::BindFunc CResourceBindings::GetBindFunc<RenderTargetViewType>(EShaderStage) { return &CSubresourceBindings::RenderTargetViewBound; }
template<> inline CSubresourceBindings::BindFunc CResourceBindings::GetBindFunc<UnorderedAccessViewType>(EShaderStage) { return &CSubresourceBindings::UnorderedAccessViewBound; }

template<> inline CSubresourceBindings::BindFunc CResourceBindings::GetUnbindFunc<ShaderResourceViewType>(EShaderStage stage) { return &CSubresourceBindings::NonPixelShaderResourceViewUnbound; }
template<> inline CSubresourceBindings::BindFunc CResourceBindings::GetUnbindFunc<RenderTargetViewType>(EShaderStage) { return &CSubresourceBindings::RenderTargetViewUnbound; }
template<> inline CSubresourceBindings::BindFunc CResourceBindings::GetUnbindFunc<DepthStencilViewType>(EShaderStage) { return &CSubresourceBindings::DepthStencilViewUnbound; }
template<> inline CSubresourceBindings::BindFunc CResourceBindings::GetUnbindFunc<UnorderedAccessViewType>(EShaderStage) { return &CSubresourceBindings::UnorderedAccessViewUnbound; }

//----------------------------------------------------------------------------------------------------------------------------------
template<typename TIface>
void CResourceBindings::ViewBound(View<TIface>* pView, EShaderStage stage, UINT /*slot*/)
{
    auto& viewBindings = pView->m_currentBindings;
    pView->IncrementBindRefs();
    if (!viewBindings.IsViewBound())
    {
        D3D12TranslationLayer::InsertHeadList(&GetViewList<TIface>(), &viewBindings.m_ViewBindingList);
    }
    CViewSubresourceSubset &viewSubresources = pView->m_subresources;
    ViewBoundCommon(viewSubresources, GetBindFunc<TIface>(stage));
}

//----------------------------------------------------------------------------------------------------------------------------------
template<typename TIface>
void CResourceBindings::ViewUnbound(View<TIface>* pView, EShaderStage stage, UINT /*slot*/)
{
    auto& viewBindings = pView->m_currentBindings;
    pView->DecrementBindRefs();
    if (pView->GetBindRefs() == 0 && viewBindings.IsViewBound())
    {
        D3D12TranslationLayer::RemoveEntryList(&viewBindings.m_ViewBindingList);
        D3D12TranslationLayer::InitializeListHead(&viewBindings.m_ViewBindingList);
    }
    CViewSubresourceSubset &viewSubresources = pView->m_subresources;
    ViewUnboundCommon(viewSubresources, GetUnbindFunc<TIface>(stage));
}

//----------------------------------------------------------------------------------------------------------------------------------
template<> inline void CResourceBindings::ViewBound<DepthStencilViewType>(TDSV* pView, EShaderStage, UINT /*slot*/)
{
    assert(!m_bIsDepthStencilViewBound);
    m_bIsDepthStencilViewBound = true;
    pView->IncrementBindRefs();

    CViewSubresourceSubset &viewSubresources = pView->m_subresources;
    
    bool bHasStencil = pView->m_pResource->SubresourceMultiplier() != 1;
    bool bReadOnlyDepth = !!(pView->GetDesc12().Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH);
    bool bReadOnlyStencil = !!(pView->GetDesc12().Flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL);
    auto pfnDepthBound = bReadOnlyDepth ? &CSubresourceBindings::ReadOnlyDepthStencilViewBound : &CSubresourceBindings::WritableDepthStencilViewBound;
    if (!bHasStencil || bReadOnlyDepth == bReadOnlyStencil)
    {
        ViewBoundCommon(viewSubresources, pfnDepthBound);
        return;
    }

    CViewSubresourceSubset readSubresources(pView->GetDesc12(),
                                             pView->m_pResource->AppDesc()->MipLevels(),
                                             pView->m_pResource->AppDesc()->ArraySize(),
                                             pView->m_pResource->SubresourceMultiplier(),
                                             CViewSubresourceSubset::ReadOnly);
    CViewSubresourceSubset writeSubresources(pView->GetDesc12(),
                                             pView->m_pResource->AppDesc()->MipLevels(),
                                             pView->m_pResource->AppDesc()->ArraySize(),
                                             pView->m_pResource->SubresourceMultiplier(),
                                             CViewSubresourceSubset::WriteOnly);

    // If either of these were empty, then there would be only one type of bind required, and the (readOnlyDepth == readOnlyStencil) check would've covered it
    assert(!readSubresources.IsEmpty() && !writeSubresources.IsEmpty());

    UINT NumViewsReferencingSubresources = m_NumViewsReferencingSubresources;
    ViewBoundCommon(readSubresources, &CSubresourceBindings::ReadOnlyDepthStencilViewBound);
    ViewBoundCommon(writeSubresources, &CSubresourceBindings::WritableDepthStencilViewBound);
    m_NumViewsReferencingSubresources = NumViewsReferencingSubresources + 1;
}

//----------------------------------------------------------------------------------------------------------------------------------
template<> inline void CResourceBindings::ViewUnbound<DepthStencilViewType>(TDSV* pView, EShaderStage stage, UINT /*slot*/)
{
#if TRANSLATION_LAYER_DBG
    // View bindings aren't used for DSVs
    auto& viewBindings = pView->m_currentBindings;
    assert(!viewBindings.IsViewBound());
#endif

    assert(m_bIsDepthStencilViewBound);
    m_bIsDepthStencilViewBound = false;
    pView->DecrementBindRefs();

    CViewSubresourceSubset &viewSubresources = pView->m_subresources;
    ViewUnboundCommon(viewSubresources, GetUnbindFunc<DepthStencilViewType>(stage));
}

//----------------------------------------------------------------------------------------------------------------------------------
// Binding helpers
//----------------------------------------------------------------------------------------------------------------------------------
inline void VBBinder::Bound(Resource* pBuffer, UINT slot, EShaderStage)  { return ImmediateContext::VertexBufferBound(pBuffer, slot); }
inline void VBBinder::Unbound(Resource* pBuffer, UINT slot, EShaderStage) { return ImmediateContext::VertexBufferUnbound(pBuffer, slot); }
inline void IBBinder::Bound(Resource* pBuffer, UINT, EShaderStage)  { return ImmediateContext::IndexBufferBound(pBuffer); }
inline void IBBinder::Unbound(Resource* pBuffer, UINT, EShaderStage) { return ImmediateContext::IndexBufferUnbound(pBuffer); }
inline void SOBinder::Bound(Resource* pBuffer, UINT slot, EShaderStage)  { return ImmediateContext::StreamOutputBufferBound(pBuffer, slot); }
inline void SOBinder::Unbound(Resource* pBuffer, UINT slot, EShaderStage) { return ImmediateContext::StreamOutputBufferUnbound(pBuffer, slot); }

//----------------------------------------------------------------------------------------------------------------------------------
inline bool BitSetLessThan(unsigned long bits, UINT slot)
{
    unsigned long index = 0;
    return BitScanForward(&index, bits) && index < slot;
}
inline bool BitSetLessThan(unsigned long long bits, UINT slot)
{
#ifdef BitScanForward64
    unsigned long index = 0;
    return BitScanForward64(&index, bits) && index < slot;
#else
    if (slot < 32)
    {
        return BitSetLessThan(static_cast<unsigned long>(bits), slot);
    }
    if (static_cast<unsigned long>(bits))
    {
        return true;
    }
    return BitSetLessThan(static_cast<unsigned long>(bits >> 32), slot - 32);
#endif
}

//----------------------------------------------------------------------------------------------------------------------------------
template <typename TBindable, UINT NumBindSlots>
inline bool CBoundState<TBindable, NumBindSlots>::DirtyBitsUpTo(_In_range_(0, NumBindings) UINT NumBitsToCheck) const noexcept
{
    if (NumBitsToCheck == 0)
    {
        return false;
    }

    if (NumBindSlots <= 32)
    {
        return BitSetLessThan(m_DirtyBits.to_ulong(), NumBitsToCheck);
    }
    else if (NumBindSlots <= 64)
    {
        return BitSetLessThan(m_DirtyBits.to_ullong(), NumBitsToCheck);
    }
    else
    {
        constexpr UINT NumBitsPerWord = sizeof(::std::conditional_t<NumBindings <= sizeof(unsigned long) * CHAR_BIT, unsigned long, unsigned long long>) * 8;
        // First, check whole "words" for any bit being set.
        UINT NumWordsToCheck = NumBitsToCheck / NumBitsPerWord;
        for (UINT word = 0; word < NumWordsToCheck; ++word)
        {
            if (m_DirtyBits._Getword(word))
            {
                return true;
            }
            NumBitsToCheck -= NumBitsPerWord;
        }
        // The slot we were asking about was the last bit of the last word we checked.
        if (NumBitsToCheck == 0)
        {
            return false;
        }
        // Check for bits inside a word.
        return BitSetLessThan(m_DirtyBits._Getword(NumWordsToCheck), NumBitsToCheck);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
template <typename TBindable, UINT NumBindSlots>
void CBoundState<TBindable, NumBindSlots>::ReassertResourceState() const noexcept
{
    for (UINT i = 0; i < m_NumBound; ++i)
    {
        if (m_Bound[i])
        {
            ImmediateContext* pDevice = m_Bound[i]->m_pParent;
            pDevice->TransitionResourceForBindings(m_Bound[i]);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
template <typename TBindable, UINT NumBindSlots, typename TBinder>
inline bool CSimpleBoundState<TBindable, NumBindSlots, TBinder>::UpdateBinding(_In_range_(0, NumBindings-1) UINT slot, _In_opt_ TBindable* pBindable, EShaderStage stage) noexcept
{
    auto pCurrent = this->m_Bound[slot];
    if (__super::UpdateBinding(slot, pBindable))
    {
        TBinder::Unbound(pCurrent, slot, stage);
        TBinder::Bound(pBindable, slot, stage);
        return true;
    }
    return false;
}

//----------------------------------------------------------------------------------------------------------------------------------
template <typename TBindable, UINT NumBindSlots>
inline bool CViewBoundState<TBindable, NumBindSlots>::UpdateBinding(_In_range_(0, NumBindings-1) UINT slot, _In_opt_ TBindable* pBindable, EShaderStage stage) noexcept
{
    auto& Current = this->m_Bound[slot];
    if (pBindable)
    {
        this->m_NumBound = max(this->m_NumBound, slot + 1);
    }
    if (Current != pBindable)
    {
        if (Current) Current->ViewUnbound(slot, stage);
        if (pBindable) pBindable->ViewBound(slot, stage);
        Current = pBindable;
        
        // We skip calling TrimNumBound because we just use shader data to determine the actual amount to bind
        this->m_DirtyBits.set(slot);
        return true;
    }
    return false;
}

//----------------------------------------------------------------------------------------------------------------------------------
template <typename TBindable, UINT NumBindSlots>
inline bool CViewBoundState<TBindable, NumBindSlots>::IsDirty(TDeclVector const& New, UINT rootSignatureBucketSize, bool bKnownDirty) noexcept
{
    // Note: Even though there are vector resize ops here, they cannot throw,
    // since the backing memory for the vector was already allocated using reserve(NumBindSlots)
    bool bDirty = bKnownDirty;
    for (UINT i = 0; i < New.size(); ++i)
    {
        if (i >= m_ShaderData.size())
        {
            // We've never bound this many before
            m_ShaderData.insert(m_ShaderData.end(), New.begin() + i, New.end());
            bDirty = true;
            break;
        }
        // Don't overwrite typed NULLs with untyped NULLs,
        // any type will work to fill a slot that won't be used
        if (m_ShaderData[i] != New[i] &&
            New[i] != c_AnyNull)
        {
            m_ShaderData[i] = New[i];
            bDirty |= this->m_Bound[i] == nullptr;
        }
    }

    if (m_ShaderData.size() < rootSignatureBucketSize)
    {
        // Did we move to a larger bucket size? If so, fill the extra shader data to null (unknown) resource dimension
        m_ShaderData.resize(rootSignatureBucketSize, c_AnyNull);
        bDirty = true;
    }
    else if (m_ShaderData.size() > rootSignatureBucketSize)
    {
        // Did we move to a smaller bucket size? If so, shrink the shader data to fit
        // Don't need to mark as dirty since the root signature won't be able to address the stale descriptors
        m_ShaderData.resize(rootSignatureBucketSize);
    }

    if (!bDirty)
    {
        bDirty = DirtyBitsUpTo(static_cast<UINT>(rootSignatureBucketSize));
    }

    return bDirty;
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool CConstantBufferBoundState::UpdateBinding(_In_range_(0, NumBindings-1) UINT slot, _In_opt_ Resource* pBindable, EShaderStage stage) noexcept
{
    auto& Current = m_Bound[slot];
    if (pBindable)
    {
        m_NumBound = max(m_NumBound, slot + 1);
    }
    if (Current != pBindable)
    {
        ImmediateContext::ConstantBufferUnbound(Current, slot, stage);
        ImmediateContext::ConstantBufferBound(pBindable, slot, stage);
        Current = pBindable;

        // We skip calling TrimNumBound because we just use shader data to determine the actual amount to bind
        m_DirtyBits.set(slot);
        return true;
    }
    return false;
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool CSamplerBoundState::UpdateBinding(_In_range_(0, NumBindings-1) UINT slot, _In_ Sampler* pBindable) noexcept
{
    auto& Current = m_Bound[slot];
    if (pBindable)
    {
        m_NumBound = max(m_NumBound, slot + 1);
    }
    if (Current != pBindable)
    {
        Current = pBindable;

        // We skip calling TrimNumBound because we just use shader data to determine the actual amount to bind
        m_DirtyBits.set(slot);
        return true;
    }
    return false;
}

inline ID3D12Resource* GetUnderlyingResource(Resource* pResource)
{
    if (!pResource)
        return nullptr;
    return pResource->GetUnderlyingResource();
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::InsertUAVBarriersIfNeeded(CViewBoundState<UAV, D3D11_1_UAV_SLOT_COUNT>& UAVBindings, UINT NumUAVs) noexcept
{
    // Insert UAV barriers if necessary, and indicate UAV barriers will be necessary next time
    // TODO: Optimizations here could avoid inserting barriers on read-after-read
    auto pUAVs = UAVBindings.GetBound();
    m_vUAVBarriers.clear();
    for (UINT i = 0; i < NumUAVs; ++i)
    {
        if (pUAVs[i])
        {
            if (pUAVs[i]->m_pResource->m_Identity->m_LastUAVAccess == GetCommandListID(COMMAND_LIST_TYPE::GRAPHICS))
            {
                m_vUAVBarriers.push_back({ D3D12_RESOURCE_BARRIER_TYPE_UAV });
                m_vUAVBarriers.back().UAV.pResource = pUAVs[i]->m_pResource->GetUnderlyingResource();
            }
            pUAVs[i]->m_pResource->m_Identity->m_LastUAVAccess = GetCommandListID(COMMAND_LIST_TYPE::GRAPHICS);
        }
    }
    if (m_vUAVBarriers.size())
    {
        GetGraphicsCommandList()->ResourceBarrier((UINT)m_vUAVBarriers.size(), m_vUAVBarriers.data());
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT ImmediateContext::CalculateViewSlotsForBindings() noexcept
{
    UINT NumRequiredSlots = 0;
    auto pfnAccumulate = [this, &NumRequiredSlots](UINT dirtyBit, UINT count)
    {
        if (m_DirtyStates & dirtyBit) { NumRequiredSlots += count; }
    };
    auto& RootSigDesc = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc;
    pfnAccumulate(e_CSShaderResourcesDirty, RootSigDesc.GetShaderStage<e_CS>().GetSRVBindingCount());
    pfnAccumulate(e_CSConstantBuffersDirty, RootSigDesc.GetShaderStage<e_CS>().GetCBBindingCount());
    pfnAccumulate(e_CSUnorderedAccessViewsDirty, RootSigDesc.GetUAVBindingCount());
    return NumRequiredSlots;
}

inline UINT ImmediateContext::CalculateSamplerSlotsForBindings() noexcept
{
    UINT NumRequiredSlots = 0;
    auto pfnAccumulate = [this, &NumRequiredSlots](UINT dirtyBit, UINT count)
    {
        if (m_DirtyStates & dirtyBit) { NumRequiredSlots += count; }
    };
    auto& RootSigDesc = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc;
    pfnAccumulate(e_CSSamplersDirty, RootSigDesc.GetShaderStage<e_CS>().GetSamplerBindingCount());
    return NumRequiredSlots;
}


//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::DirtyShaderResourcesHelper(UINT& HeapSlot) noexcept
{
    typedef SShaderTraits<e_CS> TShaderTraits;
    if ((m_DirtyStates & TShaderTraits::c_ShaderResourcesDirty) == 0)
    {
        return;
    }

    SStageState& CurrentState = TShaderTraits::CurrentStageState(m_CurrentState);
    auto& SRVBindings = CurrentState.m_SRVs;
    UINT RootSigHWM = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc.GetShaderStage<e_CS>().GetSRVBindingCount();
    UINT numSRVs = RootSigHWM;
    static const UINT MaxSRVs = SRVBindings.NumBindings;
    assert(HeapSlot + numSRVs <= m_ViewHeap.m_Desc.NumDescriptors);

    D3D12_CPU_DESCRIPTOR_HANDLE Descriptors[MaxSRVs];
    SRVBindings.FillDescriptors(Descriptors, m_NullSRVs, RootSigHWM);

    CurrentState.m_SRVTableBase = m_ViewHeap.GPUHandle(HeapSlot);
    D3D12_CPU_DESCRIPTOR_HANDLE SRVTableBaseCPU = m_ViewHeap.CPUHandle(HeapSlot);

    m_pDevice12->CopyDescriptors(1, &SRVTableBaseCPU, &numSRVs,
        numSRVs, Descriptors, nullptr /*sizes*/,
        m_ViewHeap.m_Desc.Type);

    HeapSlot += numSRVs;
}

//----------------------------------------------------------------------------------------------------------------------------------
template <typename TDesc>
inline void GetBufferViewDesc(Resource *pBuffer, TDesc &Desc, UINT APIOffset, UINT APISize = -1)
{
    if (pBuffer)
    {
        Desc.SizeInBytes =
            min(GetDynamicBufferSize<TDesc>(pBuffer, APIOffset), APISize);
        Desc.BufferLocation = Desc.SizeInBytes == 0 ? 0 :
            // TODO: Cache the GPU VA, frequent calls to this cause a CPU hotspot
            (pBuffer->GetUnderlyingResource()->GetGPUVirtualAddress() // Base of the DX12 resource
             + pBuffer->GetSubresourcePlacement(0).Offset // Base of the DX11 resource after renaming
             + APIOffset); // Offset from the base of the DX11 resource
    }
    else
    {
        Desc.BufferLocation = 0;
        Desc.SizeInBytes = 0;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::DirtyConstantBuffersHelper(UINT& HeapSlot) noexcept
{
    typedef SShaderTraits<e_CS> TShaderTraits;
    if ((m_DirtyStates & TShaderTraits::c_ConstantBuffersDirty) == 0)
    {
        return;
    }

    SStageState& CurrentState = TShaderTraits::CurrentStageState(m_CurrentState);
    auto& CBBindings = CurrentState.m_CBs;
    UINT numCBs = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc.GetShaderStage<e_CS>().GetCBBindingCount();
    static const UINT MaxCBs = CBBindings.NumBindings;

    assert(HeapSlot + numCBs <= m_ViewHeap.m_Desc.NumDescriptors);

    for (UINT i = 0; i < numCBs; ++i)
    {
        CBBindings.ResetDirty(i);
        auto pBuffer = CBBindings.GetBound()[i];
        D3D12_CONSTANT_BUFFER_VIEW_DESC CBDesc;
        UINT APIOffset = CurrentState.m_uConstantBufferOffsets[i] * 16;
        UINT APISize = CurrentState.m_uConstantBufferCounts[i] * 16;
        GetBufferViewDesc(pBuffer, CBDesc, APIOffset, APISize);

        D3D12_CPU_DESCRIPTOR_HANDLE Descriptor = m_ViewHeap.CPUHandle(HeapSlot + i);
        m_pDevice12->CreateConstantBufferView(&CBDesc, Descriptor);
    }

    CurrentState.m_CBTableBase = m_ViewHeap.GPUHandle(HeapSlot);

    HeapSlot += numCBs;
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::DirtySamplersHelper(UINT& HeapSlot) noexcept
{
    typedef SShaderTraits<e_CS> TShaderTraits;
    if ((m_DirtyStates & TShaderTraits::c_SamplersDirty) == 0)
    {
        return;
    }

    SStageState& CurrentState = TShaderTraits::CurrentStageState(m_CurrentState);
    UINT RootSigHWM = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc.GetShaderStage<e_CS>().GetSamplerBindingCount();
    UINT numSamplers = RootSigHWM;
    auto& SamplerBindings = CurrentState.m_Samplers;
    static const UINT MaxSamplers = SamplerBindings.NumBindings;

    assert(HeapSlot + numSamplers <= m_SamplerHeap.m_Desc.NumDescriptors);

    D3D12_CPU_DESCRIPTOR_HANDLE Descriptors[MaxSamplers];
    SamplerBindings.FillDescriptors(Descriptors, &m_NullSampler, RootSigHWM);

    CurrentState.m_SamplerTableBase = m_SamplerHeap.GPUHandle(HeapSlot);
    D3D12_CPU_DESCRIPTOR_HANDLE SamplerTableBaseCPU = m_SamplerHeap.CPUHandle(HeapSlot);

    m_pDevice12->CopyDescriptors(1, &SamplerTableBaseCPU, &numSamplers,
        numSamplers, Descriptors, nullptr /*sizes*/,
        m_SamplerHeap.m_Desc.Type);

    HeapSlot += numSamplers;
}

//----------------------------------------------------------------------------------------------------------------------------------
template<EShaderStage eShader> struct DescriptorBindFuncs
{
    static decltype(&ID3D12GraphicsCommandList::SetComputeRootDescriptorTable) GetBindFunc()
    {
        return &ID3D12GraphicsCommandList::SetComputeRootDescriptorTable;
    }
};

template<EShaderStage eShader> struct SRVBindIndices;
template<> struct SRVBindIndices<e_CS> { static const UINT c_TableIndex = 1; };

inline void ImmediateContext::ApplyShaderResourcesHelper() noexcept
{
    typedef SShaderTraits<e_CS> TShaderTraits;
    SStageState& CurrentState = TShaderTraits::CurrentStageState(m_CurrentState);
    if ((m_StatesToReassert & TShaderTraits::c_ShaderResourcesDirty) == 0)
    {
        return;
    }

    (GetGraphicsCommandList()->*DescriptorBindFuncs<e_CS>::GetBindFunc())(
        SRVBindIndices<e_CS>::c_TableIndex,
        CurrentState.m_SRVTableBase);
}

//----------------------------------------------------------------------------------------------------------------------------------
template<EShaderStage eShader> struct CBBindIndices;
template<> struct CBBindIndices<e_CS> { static const UINT c_TableIndex = 0; };

inline void ImmediateContext::ApplyConstantBuffersHelper() noexcept
{
    typedef SShaderTraits<e_CS> TShaderTraits;
    SStageState& CurrentState = TShaderTraits::CurrentStageState(m_CurrentState);
    if ((m_StatesToReassert & TShaderTraits::c_ConstantBuffersDirty) == 0)
    {
        return;
    }

    (GetGraphicsCommandList()->*DescriptorBindFuncs<e_CS>::GetBindFunc())(
        CBBindIndices<e_CS>::c_TableIndex,
        CurrentState.m_CBTableBase);
}

//----------------------------------------------------------------------------------------------------------------------------------
template<EShaderStage eShader> struct SamplerBindIndices;
template<> struct SamplerBindIndices<e_CS> { static const UINT c_TableIndex = 2; };

inline void ImmediateContext::ApplySamplersHelper() noexcept
{
    typedef SShaderTraits<e_CS> TShaderTraits;
    SStageState& CurrentState = TShaderTraits::CurrentStageState(m_CurrentState);
    if ((m_StatesToReassert & TShaderTraits::c_SamplersDirty) == 0)
    {
        return;
    }

    (GetGraphicsCommandList()->*DescriptorBindFuncs<e_CS>::GetBindFunc())(
        SamplerBindIndices<e_CS>::c_TableIndex,
        CurrentState.m_SamplerTableBase);
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void  ImmediateContext::Dispatch(UINT x, UINT y, UINT z)
{
    // Early out if no compute shader has been set
    if (!m_CurrentState.m_pPSO->GetComputeDesc().CS.pShaderBytecode)
    {
        return;
    }

    try
    {
        PreDispatch(); // throw ( _com_error )
        GetGraphicsCommandList()->Dispatch(x, y, z);
        PostDispatch();
    }
    catch (_com_error) {} // already handled, but can't touch the command list
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::PreDispatch() noexcept(false)
{
#ifdef USE_PIX
    PIXScopedEvent(GetGraphicsCommandList(), 0ull, L"PreDispatch");
#endif

    if (m_bUseRingBufferDescriptorHeaps)
    {
        // Always dirty the state when using ring buffer heaps because we can't safely reuse tables in that case.
        m_DirtyStates |= EDirtyBits::e_HeapBindingsDirty;
    }

    if (m_DirtyStates & e_ComputeRootSignatureDirty)
    {
        m_StatesToReassert |= e_ComputeBindingsDirty; // All bindings need to be reapplied
        if (m_CurrentState.m_pLastComputeRootSig == nullptr)
        {
            // We don't know, so we have to be conservative.
            m_DirtyStates |= e_ComputeBindingsDirty;
        }
        else if (m_CurrentState.m_pLastComputeRootSig != m_CurrentState.m_pPSO->GetRootSignature())
        {
            RootSignatureDesc const& OldDesc = m_CurrentState.m_pLastComputeRootSig->m_Desc;
            RootSignatureDesc const& NewDesc = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc;
            if (NewDesc.m_ShaderStages[0].GetCBBindingCount() > OldDesc.m_ShaderStages[0].GetCBBindingCount())
            {
                m_DirtyStates |= e_CSConstantBuffersDirty;
            }
            if (NewDesc.m_ShaderStages[0].GetSRVBindingCount() > OldDesc.m_ShaderStages[0].GetSRVBindingCount())
            {
                m_DirtyStates |= e_CSShaderResourcesDirty;
            }
            if (NewDesc.m_ShaderStages[0].GetSamplerBindingCount() > OldDesc.m_ShaderStages[0].GetSamplerBindingCount())
            {
                m_DirtyStates |= e_CSSamplersDirty;
            }
            if (NewDesc.GetUAVBindingCount() > OldDesc.GetUAVBindingCount())
            {
                m_DirtyStates |= e_CSUnorderedAccessViewsDirty;
            }
        }
        m_CurrentState.m_pLastComputeRootSig = m_CurrentState.m_pPSO->GetRootSignature();
    }

    // See PreDraw for comments regarding how dirty bits for bindings are managed
    auto& RootSigDesc = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc;
    auto& shaderStage = RootSigDesc.GetShaderStage<e_CS>();

    const TDeclVector EmptyDecls;
    auto pComputeShader = m_CurrentState.m_pPSO->GetShader<e_CS>();
    m_DirtyStates |= m_CurrentState.m_CS.m_SRVs.IsDirty(pComputeShader ? pComputeShader->m_ResourceDecls : EmptyDecls, shaderStage.GetSRVBindingCount(), !!(m_DirtyStates & e_CSShaderResourcesDirty)) ? e_CSShaderResourcesDirty : 0;
    m_DirtyStates |= m_CurrentState.m_CS.m_CBs.IsDirty(shaderStage.GetCBBindingCount()) ? e_CSConstantBuffersDirty : 0;
    m_DirtyStates |= m_CurrentState.m_CS.m_Samplers.IsDirty(shaderStage.GetSamplerBindingCount()) ? e_CSSamplersDirty : 0;
    m_DirtyStates |= m_CurrentState.m_CSUAVs.IsDirty(pComputeShader ? pComputeShader->m_UAVDecls : EmptyDecls, RootSigDesc.GetUAVBindingCount(), !!(m_DirtyStates & e_CSUnorderedAccessViewsDirty)) ? e_CSUnorderedAccessViewsDirty : 0;

    // Now that pipeline dirty bits are set appropriately, check if we need to update the descriptor heap
    UINT ViewHeapSlot = ReserveSlotsForBindings(m_ViewHeap, &ImmediateContext::CalculateViewSlotsForBindings); // throw( _com_error )
    UINT SamplerHeapSlot = 0;
    if (!ComputeOnly())
    {
        SamplerHeapSlot = ReserveSlotsForBindings(m_SamplerHeap, &ImmediateContext::CalculateSamplerSlotsForBindings); // throw( _com_error )
    }

    auto& UAVBindings = m_CurrentState.m_CSUAVs;
    UINT numUAVs = RootSigDesc.GetUAVBindingCount();
    InsertUAVBarriersIfNeeded(UAVBindings, numUAVs);

    // Second pass copies data into the the descriptor heap
    if (m_DirtyStates & e_ComputeBindingsDirty)
    {
        DirtyShaderResourcesHelper(ViewHeapSlot);
        DirtyConstantBuffersHelper(ViewHeapSlot);
        if (!ComputeOnly())
        {
            DirtySamplersHelper(SamplerHeapSlot);
        }
        if (m_DirtyStates & e_CSUnorderedAccessViewsDirty)
        {
            auto& UAVTableBase = m_CurrentState.m_CSUAVTableBase;

            static const UINT MaxUAVs = UAVBindings.NumBindings;
            assert(ViewHeapSlot + numUAVs <= m_ViewHeap.m_Desc.NumDescriptors);

            D3D12_CPU_DESCRIPTOR_HANDLE UAVDescriptors[MaxUAVs];
            UAVBindings.FillDescriptors(UAVDescriptors, m_NullUAVs, RootSigDesc.GetUAVBindingCount());

            UAVTableBase = m_ViewHeap.GPUHandle(ViewHeapSlot);
            D3D12_CPU_DESCRIPTOR_HANDLE UAVTableBaseCPU = m_ViewHeap.CPUHandle(ViewHeapSlot);

            m_pDevice12->CopyDescriptors(1, &UAVTableBaseCPU, &numUAVs,
                numUAVs, UAVDescriptors, nullptr /*sizes*/,
                m_ViewHeap.m_Desc.Type);

            ViewHeapSlot += numUAVs;
        }
    }

    m_StatesToReassert |= (m_DirtyStates & e_ComputeStateDirty);

    if (m_StatesToReassert & e_FirstDispatch)
    {
        m_CurrentState.m_CS.m_SRVs.ReassertResourceState();
        m_CurrentState.m_CS.m_CBs.ReassertResourceState();
        m_CurrentState.m_CSUAVs.ReassertResourceState();
    }

    m_ResourceStateManager.ApplyAllResourceTransitions(true);

    if (m_StatesToReassert & e_ComputeStateDirty)
    {
        if (m_StatesToReassert & e_ComputeRootSignatureDirty)
        {
            GetGraphicsCommandList()->SetComputeRootSignature(m_CurrentState.m_pPSO->GetRootSignature()->GetForImmediateUse());
            m_StatesToReassert |= e_ComputeBindingsDirty;
        }

        if (m_StatesToReassert & e_PipelineStateDirty)
        {
            auto pPSO = m_CurrentState.m_pPSO->GetForUse(COMMAND_LIST_TYPE::GRAPHICS);
            if (!pPSO)
            {
                throw _com_error(S_OK);
            }
            GetGraphicsCommandList()->SetPipelineState(pPSO);
        }

        ApplyShaderResourcesHelper();
        ApplyConstantBuffersHelper();
        if (ComputeOnly())
        {
            if (m_StatesToReassert & e_CSSamplersDirty)
            {
                // For compute-only, we turn our sampler tables into SRVs.
                // Make sure we bind something that's valid to the sampler slot, and just make it mirror the SRVs.
                GetGraphicsCommandList()->SetComputeRootDescriptorTable(SamplerBindIndices<e_CS>::c_TableIndex, m_CurrentState.m_CS.m_SRVTableBase);
            }
        }
        else
        {
            ApplySamplersHelper();
        }
        if (m_StatesToReassert & e_CSUnorderedAccessViewsDirty)
        {
            static const UINT UAVTableIndex = 3;
            auto const& UAVTableBase = m_CurrentState.m_CSUAVTableBase;

            GetGraphicsCommandList()->SetComputeRootDescriptorTable(UAVTableIndex, UAVTableBase);
        }
    }

    m_StatesToReassert &= ~e_ComputeStateDirty;
    m_DirtyStates &= ~e_ComputeStateDirty;
}

//----------------------------------------------------------------------------------------------------------------------------------
inline ID3D12CommandQueue *ImmediateContext::GetCommandQueue(COMMAND_LIST_TYPE type) noexcept
{
    if (type != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)type])
    {
        return m_CommandLists[(UINT)type]->GetCommandQueue();
    }
    else
    {
        return nullptr;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline ID3D12GraphicsCommandList *ImmediateContext::GetGraphicsCommandList() noexcept
{
    return m_CommandLists[(UINT)COMMAND_LIST_TYPE::GRAPHICS]->GetGraphicsCommandList();
}

// There is an MSVC bug causing a bogus warning to be emitted here for x64 only, while compiling ApplyAllResourceTransitions
#pragma warning(push)
#pragma warning(disable: 4789)
//----------------------------------------------------------------------------------------------------------------------------------
inline CommandListManager *ImmediateContext::GetCommandListManager(COMMAND_LIST_TYPE type) noexcept
{
    return type != COMMAND_LIST_TYPE::UNKNOWN ? m_CommandLists[(UINT)type].get() : nullptr;
}
#pragma warning(pop)

//----------------------------------------------------------------------------------------------------------------------------------
inline ID3D12CommandList *ImmediateContext::GetCommandList(COMMAND_LIST_TYPE commandListType) noexcept
{
    if (commandListType != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)commandListType])
    {
        return m_CommandLists[(UINT)commandListType]->GetCommandList();
    }
    else
    {
        return nullptr;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCommandListID(COMMAND_LIST_TYPE type) noexcept
{
    if (type != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)type])
    {
        return m_CommandLists[(UINT)type]->GetCommandListID();
    }
    else
    {
        return 0;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCommandListIDInterlockedRead(COMMAND_LIST_TYPE type) noexcept
{
    if (type != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)type])
    {
        return m_CommandLists[(UINT)type]->GetCommandListIDInterlockedRead();
    }
    else
    {
        return 0;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCommandListIDWithCommands(COMMAND_LIST_TYPE type) noexcept
{
    // This method gets the ID of the last command list that actually has commands, which is either
    // the current command list, if it has commands, or the previously submitted command list if the
    // current is empty.
    //
    // The result of this method is the fence id that will be signaled after a flush, and is used so that
    // Async::End can track query completion correctly.
    UINT64 Id = 0;
    if (type != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)type])
    {
        Id = m_CommandLists[(UINT)type]->GetCommandListID();
        assert(Id);
        if (!m_CommandLists[(UINT)type]->HasCommands() && !m_CommandLists[(UINT)type]->NeedSubmitFence())
        {
            Id -= 1; // Go back one command list
        }
    }
    return Id;
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCompletedFenceValue(COMMAND_LIST_TYPE type) noexcept
{
    if (type != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)type])
    {
        return m_CommandLists[(UINT)type]->GetCompletedFenceValue();
    }
    else
    {
        return 0;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline Fence *ImmediateContext::GetFence(COMMAND_LIST_TYPE type) noexcept
{
    if (type != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)type])
    {
        return m_CommandLists[(UINT)type]->GetFence();
    }
    else
    {
        return nullptr;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::CloseCommandList(UINT commandListTypeMask) noexcept
{
    for (UINT i = 0; i < (UINT)COMMAND_LIST_TYPE::MAX_VALID; i++)
    {
        if ((commandListTypeMask & (1 << i)) && m_CommandLists[i])
        {
            m_CommandLists[i]->CloseCommandList();
        }
    }
}


//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::ResetCommandList(UINT commandListTypeMask) noexcept
{
    for (UINT i = 0; i < (UINT)COMMAND_LIST_TYPE::MAX_VALID; i++)
    {
        if ((commandListTypeMask & (1 << i)) && m_CommandLists[i])
        {
            m_CommandLists[i]->ResetCommandList();
        }
    }
}


//----------------------------------------------------------------------------------------------------------------------------------
inline HRESULT ImmediateContext::EnqueueSetEvent(UINT commandListTypeMask, HANDLE hEvent) noexcept
{
#ifdef USE_PIX
    PIXSetMarker(0ull, L"EnqueueSetEvent");
#endif
    HRESULT hr = S_OK;
    ID3D12Fence *pFences[(UINT)COMMAND_LIST_TYPE::MAX_VALID] = {};
    UINT64 FenceValues[(UINT)COMMAND_LIST_TYPE::MAX_VALID] = {};
    UINT nLists = 0;
    for (UINT i = 0; i < (UINT)COMMAND_LIST_TYPE::MAX_VALID; i++)
    {
        if ((commandListTypeMask & (1 << i)) && m_CommandLists[i])
        {
            pFences[nLists] = m_CommandLists[i]->GetFence()->Get();
            try {
                FenceValues[nLists] = m_CommandLists[i]->EnsureFlushedAndFenced(); // throws
            }
            catch (_com_error& e)
            {
                return e.Error();
            }
            catch (std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }

            ++nLists;
        }
    }
    hr = m_pDevice12_1->SetEventOnMultipleFenceCompletion(
             pFences,
             FenceValues,
             nLists,
             D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL,
             hEvent);
    return hr;
}

//----------------------------------------------------------------------------------------------------------------------------------
inline HRESULT ImmediateContext::EnqueueSetEvent(COMMAND_LIST_TYPE commandListType, HANDLE hEvent) noexcept
{
    if (commandListType != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)commandListType])
    {
        return m_CommandLists[(UINT)commandListType]->EnqueueSetEvent(hEvent);
    }
    else
    {
        return E_UNEXPECTED;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::WaitForCompletion(UINT commandListTypeMask) noexcept
{
    UINT nLists = 0;
    HANDLE hEvents[(UINT)COMMAND_LIST_TYPE::MAX_VALID] = {};
    for (UINT i = 0; i < (UINT)COMMAND_LIST_TYPE::MAX_VALID; i++)
    {
        if ((commandListTypeMask & (1 << i)) && m_CommandLists[i])
        {
            hEvents[nLists] = m_CommandLists[i]->GetEvent();
            if (FAILED(m_CommandLists[i]->EnqueueSetEvent(hEvents[nLists])))
            {
                return false;
            }
            ++nLists;
        }
    }
    DWORD waitRet = WaitForMultipleObjects(nLists, hEvents, TRUE, INFINITE);
    UNREFERENCED_PARAMETER(waitRet);
    assert(waitRet == WAIT_OBJECT_0);
    return true;
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::WaitForCompletion(COMMAND_LIST_TYPE commandListType)
{
    if (commandListType != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)commandListType])
    {
        return m_CommandLists[(UINT)commandListType]->WaitForCompletion(); // throws
    }
    else
    {
        return false;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::WaitForFenceValue(COMMAND_LIST_TYPE commandListType, UINT64 FenceValue)
{
    if (commandListType != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)commandListType])
    {
        return m_CommandLists[(UINT)commandListType]->WaitForFenceValue(FenceValue); // throws
    }
    else
    {
        return false;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::SubmitCommandList(UINT commandListTypeMask)
{
    for (UINT i = 0; i < (UINT)COMMAND_LIST_TYPE::MAX_VALID; i++)
    {
        if ((commandListTypeMask & (1 << i))  &&  m_CommandLists[i])
        {
            m_CommandLists[i]->SubmitCommandList(); // throws
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::SubmitCommandList(COMMAND_LIST_TYPE commandListType)
{
    if (commandListType != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)commandListType])
    {
        m_CommandLists[(UINT)commandListType]->SubmitCommandList(); // throws
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::AdditionalCommandsAdded(COMMAND_LIST_TYPE type) noexcept
{
    if (type != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)type])
    {
        m_CommandLists[(UINT)type]->AdditionalCommandsAdded();
    }
}

inline void ImmediateContext::UploadHeapSpaceAllocated(COMMAND_LIST_TYPE type, UINT64 HeapSize) noexcept
{
    if (type != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)type])
    {
        m_CommandLists[(UINT)type]->UploadHeapSpaceAllocated(HeapSize);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::HasCommands(COMMAND_LIST_TYPE type) noexcept
{
    if (type != COMMAND_LIST_TYPE::UNKNOWN  &&  m_CommandLists[(UINT)type])
    {
        return m_CommandLists[(UINT)type]->HasCommands();
    }
    else
    {
        return false;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT ImmediateContext::GetCurrentCommandListTypeMask() noexcept
{
    UINT Mask = 0;
    for (UINT i = 0; i < (UINT)COMMAND_LIST_TYPE::MAX_VALID; i++)
    {
        if (m_CommandLists[i])
        {
            Mask |= (1 << i);
        }
    }
    return Mask;
}


};
