// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
namespace D3D12TranslationLayer
{

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
inline bool CViewBoundState<TBindable, NumBindSlots>::UpdateBinding(_In_range_(0, NumBindings-1) UINT slot, _In_opt_ TBindable* pBindable) noexcept
{
    auto& Current = this->m_Bound[slot];
    if (pBindable)
    {
        this->m_NumBound = max(this->m_NumBound, slot + 1);
    }
    if (Current != pBindable)
    {
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
inline bool CConstantBufferBoundState::UpdateBinding(_In_range_(0, NumBindings-1) UINT slot, _In_opt_ Resource* pBindable) noexcept
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
            if (pUAVs[i]->m_pResource->m_Identity->m_LastUAVAccess == GetCommandListID())
            {
                m_vUAVBarriers.push_back({ D3D12_RESOURCE_BARRIER_TYPE_UAV });
                m_vUAVBarriers.back().UAV.pResource = pUAVs[i]->m_pResource->GetUnderlyingResource();
            }
            pUAVs[i]->m_pResource->m_Identity->m_LastUAVAccess = GetCommandListID();
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
    pfnAccumulate(e_CSShaderResourcesDirty, RootSigDesc.GetShaderStage().GetSRVBindingCount());
    pfnAccumulate(e_CSConstantBuffersDirty, RootSigDesc.GetShaderStage().GetCBBindingCount());
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
    pfnAccumulate(e_CSSamplersDirty, RootSigDesc.GetShaderStage().GetSamplerBindingCount());
    return NumRequiredSlots;
}


//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::DirtyShaderResourcesHelper(UINT& HeapSlot) noexcept
{
    if ((m_DirtyStates & e_CSShaderResourcesDirty) == 0)
    {
        return;
    }

    SStageState& CurrentState = m_CurrentState.m_CS;
    auto& SRVBindings = CurrentState.m_SRVs;
    UINT RootSigHWM = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc.GetShaderStage().GetSRVBindingCount();
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
    if ((m_DirtyStates & e_CSConstantBuffersDirty) == 0)
    {
        return;
    }

    SStageState& CurrentState = m_CurrentState.m_CS;
    auto& CBBindings = CurrentState.m_CBs;
    UINT numCBs = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc.GetShaderStage().GetCBBindingCount();
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
    if ((m_DirtyStates & e_CSSamplersDirty) == 0)
    {
        return;
    }

    SStageState& CurrentState = m_CurrentState.m_CS;
    UINT RootSigHWM = m_CurrentState.m_pPSO->GetRootSignature()->m_Desc.GetShaderStage().GetSamplerBindingCount();
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

inline void ImmediateContext::ApplyShaderResourcesHelper() noexcept
{
    SStageState& CurrentState = m_CurrentState.m_CS;
    if ((m_StatesToReassert & e_CSShaderResourcesDirty) == 0)
    {
        return;
    }

    GetGraphicsCommandList()->SetComputeRootDescriptorTable(
        1,
        CurrentState.m_SRVTableBase);
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::ApplyConstantBuffersHelper() noexcept
{
    SStageState &CurrentState = m_CurrentState.m_CS;
    if ((m_StatesToReassert & e_CSConstantBuffersDirty) == 0)
    {
        return;
    }

    GetGraphicsCommandList()->SetComputeRootDescriptorTable(
        0,
        CurrentState.m_CBTableBase);
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::ApplySamplersHelper() noexcept
{
    SStageState &CurrentState = m_CurrentState.m_CS;
    if ((m_StatesToReassert & e_CSSamplersDirty) == 0)
    {
        return;
    }

    GetGraphicsCommandList()->SetComputeRootDescriptorTable(
        2,
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
    auto& shaderStage = RootSigDesc.GetShaderStage();

    const TDeclVector EmptyDecls;
    auto pComputeShader = m_CurrentState.m_pPSO->GetShader();
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

    m_ResourceStateManager.ApplyAllResourceTransitions();

    if (m_StatesToReassert & e_ComputeStateDirty)
    {
        if (m_StatesToReassert & e_ComputeRootSignatureDirty)
        {
            GetGraphicsCommandList()->SetComputeRootSignature(m_CurrentState.m_pPSO->GetRootSignature()->GetForImmediateUse());
            m_StatesToReassert |= e_ComputeBindingsDirty;
        }

        if (m_StatesToReassert & e_PipelineStateDirty)
        {
            auto pPSO = m_CurrentState.m_pPSO->GetForUse();
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
                GetGraphicsCommandList()->SetComputeRootDescriptorTable(2, m_CurrentState.m_CS.m_SRVTableBase);
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
inline ID3D12CommandQueue *ImmediateContext::GetCommandQueue() noexcept
{
    if (m_CommandList)
    {
        return m_CommandList->GetCommandQueue();
    }
    else
    {
        return nullptr;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline ID3D12GraphicsCommandList *ImmediateContext::GetGraphicsCommandList() noexcept
{
    return m_CommandList->GetGraphicsCommandList();
}

// There is an MSVC bug causing a bogus warning to be emitted here for x64 only, while compiling ApplyAllResourceTransitions
#pragma warning(push)
#pragma warning(disable: 4789)
//----------------------------------------------------------------------------------------------------------------------------------
inline CommandListManager *ImmediateContext::GetCommandListManager() noexcept
{
    return m_CommandList ? m_CommandList.get() : nullptr;
}
#pragma warning(pop)

//----------------------------------------------------------------------------------------------------------------------------------
inline ID3D12CommandList *ImmediateContext::GetCommandList() noexcept
{
    if (m_CommandList)
    {
        return m_CommandList->GetCommandList();
    }
    else
    {
        return nullptr;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCommandListID() noexcept
{
    if (m_CommandList)
    {
        return m_CommandList->GetCommandListID();
    }
    else
    {
        return 0;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCommandListIDInterlockedRead() noexcept
{
    if (m_CommandList)
    {
        return m_CommandList->GetCommandListIDInterlockedRead();
    }
    else
    {
        return 0;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCommandListIDWithCommands() noexcept
{
    // This method gets the ID of the last command list that actually has commands, which is either
    // the current command list, if it has commands, or the previously submitted command list if the
    // current is empty.
    //
    // The result of this method is the fence id that will be signaled after a flush, and is used so that
    // Async::End can track query completion correctly.
    UINT64 Id = 0;
    if (m_CommandList)
    {
        Id = m_CommandList->GetCommandListID();
        assert(Id);
        if (m_CommandList->HasCommands() && !m_CommandList->NeedSubmitFence())
        {
            Id -= 1; // Go back one command list
        }
    }
    return Id;
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCompletedFenceValue() noexcept
{
    if (m_CommandList)
    {
        return m_CommandList->GetCompletedFenceValue();
    }
    else
    {
        return 0;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline Fence *ImmediateContext::GetFence() noexcept
{
    if (m_CommandList)
    {
        return m_CommandList->GetFence();
    }
    else
    {
        return nullptr;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::CloseCommandList() noexcept
{
    if (m_CommandList)
    {
        m_CommandList->CloseCommandList();
    }
}


//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::ResetCommandList() noexcept
{
    if (m_CommandList)
    {
        m_CommandList->ResetCommandList();
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline HRESULT ImmediateContext::EnqueueSetEvent(HANDLE hEvent) noexcept
{
    if (m_CommandList)
    {
        return m_CommandList->EnqueueSetEvent(hEvent);
    }
    else
    {
        return E_UNEXPECTED;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::WaitForCompletion()
{
    if (m_CommandList)
    {
        return m_CommandList->WaitForCompletion(); // throws
    }
    else
    {
        return false;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::WaitForFenceValue(UINT64 FenceValue)
{
    if (m_CommandList)
    {
        return m_CommandList->WaitForFenceValue(FenceValue); // throws
    }
    else
    {
        return false;
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::SubmitCommandList()
{
    if (m_CommandList)
    {
        m_CommandList->SubmitCommandList(); // throws
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::AdditionalCommandsAdded() noexcept
{
    if (m_CommandList)
    {
        m_CommandList->AdditionalCommandsAdded();
    }
}

inline void ImmediateContext::UploadHeapSpaceAllocated(UINT64 HeapSize) noexcept
{
    if (m_CommandList)
    {
        m_CommandList->UploadHeapSpaceAllocated(HeapSize);
    }
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::HasCommands() noexcept
{
    if (m_CommandList)
    {
        return m_CommandList->HasCommands();
    }
    else
    {
        return false;
    }
}

};
