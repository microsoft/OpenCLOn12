// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "View.inl"

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
    return m_CommandList.GetCommandQueue();
}

//----------------------------------------------------------------------------------------------------------------------------------
inline ID3D12GraphicsCommandList *ImmediateContext::GetGraphicsCommandList() noexcept
{
    return m_CommandList.GetGraphicsCommandList();
}

// There is an MSVC bug causing a bogus warning to be emitted here for x64 only, while compiling ApplyAllResourceTransitions
#pragma warning(push)
#pragma warning(disable: 4789)
//----------------------------------------------------------------------------------------------------------------------------------
inline CommandListManager *ImmediateContext::GetCommandListManager() noexcept
{
    return &m_CommandList;
}
#pragma warning(pop)

//----------------------------------------------------------------------------------------------------------------------------------
inline ID3D12CommandList *ImmediateContext::GetCommandList() noexcept
{
    return m_CommandList.GetCommandList();
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCommandListID() noexcept
{
    return m_CommandList.GetCommandListID();
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCommandListIDInterlockedRead() noexcept
{
    return m_CommandList.GetCommandListIDInterlockedRead();
}

//----------------------------------------------------------------------------------------------------------------------------------
inline UINT64 ImmediateContext::GetCompletedFenceValue() noexcept
{
    return m_CommandList.GetCompletedFenceValue();
}

//----------------------------------------------------------------------------------------------------------------------------------
inline Fence *ImmediateContext::GetFence() noexcept
{
    return m_CommandList.GetFence();
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::CloseCommandList() noexcept
{
    m_CommandList.CloseCommandList();
}


//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::ResetCommandList() noexcept
{
    m_CommandList.ResetCommandList();
}

//----------------------------------------------------------------------------------------------------------------------------------
inline HRESULT ImmediateContext::EnqueueSetEvent(HANDLE hEvent) noexcept
{
    return m_CommandList.EnqueueSetEvent(hEvent);
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::WaitForCompletion()
{
    return m_CommandList.WaitForCompletion(); // throws
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::WaitForFenceValue(UINT64 FenceValue)
{
    return m_CommandList.WaitForFenceValue(FenceValue); // throws
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::SubmitCommandList()
{
    m_CommandList.SubmitCommandList(); // throws
}

//----------------------------------------------------------------------------------------------------------------------------------
inline void ImmediateContext::AdditionalCommandsAdded() noexcept
{
    m_CommandList.AdditionalCommandsAdded();
}

inline void ImmediateContext::UploadHeapSpaceAllocated(UINT64 HeapSize) noexcept
{
    m_CommandList.UploadHeapSpaceAllocated(HeapSize);
}

//----------------------------------------------------------------------------------------------------------------------------------
inline bool ImmediateContext::HasCommands() noexcept
{
    return m_CommandList.HasCommands();
}

};
