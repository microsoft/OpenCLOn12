// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
namespace D3D12TranslationLayer
{

inline ID3D12Resource* GetUnderlyingResource(Resource* pResource)
{
    if (!pResource)
        return nullptr;
    return pResource->GetUnderlyingResource();
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
inline void  ImmediateContext::Dispatch(UINT x, UINT y, UINT z)
{
    try
    {
        GetGraphicsCommandList()->Dispatch(x, y, z);
        PostDispatch();
    }
    catch (_com_error) {} // already handled, but can't touch the command list
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
