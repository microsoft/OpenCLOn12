// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "D3D12TranslationLayerDependencyIncludes.h"
#include "Fence.hpp"
#include "Util.hpp"

namespace D3D12TranslationLayer
{
    LONGLONG InterlockedRead64(volatile LONGLONG* p);

    class ImmediateContext;
    class ResidencySet;

    class CommandListManager
    {
    public:
        CommandListManager(ImmediateContext *pParent, ID3D12CommandQueue *pQueue);

        ~CommandListManager();

        void AdditionalCommandsAdded() noexcept;
        void DispatchCommandAdded() noexcept;
        void UploadHeapSpaceAllocated(UINT64 heapSize) noexcept;
        void ReadbackInitiated() noexcept;
        void SubmitCommandListIfNeeded();

        bool HasCommands() const noexcept { return m_NumCommands > 0; }

        void SubmitCommandList();
        void InitCommandList();
        void ResetCommandList();
        void CloseCommandList() { CloseCommandList(nullptr); }
        void DiscardCommandList();
        void ResetResidencySet();

        void PrepForCommandQueueSync();

        // Returns true if synchronization was successful, false likely means device is removed
        bool WaitForCompletion();
        bool WaitForFenceValue(UINT64 FenceValue);
        UINT64 GetCompletedFenceValue() noexcept { return m_Fence.GetCompletedValue(); }
        HRESULT EnqueueSetEvent(HANDLE hEvent) noexcept;
        UINT64 EnsureFlushedAndFenced();
        HANDLE GetEvent() noexcept { return m_hWaitEvent; }
        void AddResourceToResidencySet(Resource *pResource);

        UINT64 GetCommandListID() { return m_commandListID; }
        UINT64 GetCommandListIDInterlockedRead() { return InterlockedRead64((volatile LONGLONG*)&m_commandListID); }
        ID3D12CommandQueue* GetCommandQueue() { return m_pCommandQueue.get(); }
        ID3D12CommandList* GetCommandList() { return m_pCommandList.get(); }
        ID3D12SharingContract* GetSharingContract() { return m_pSharingContract.get(); }
        Fence* GetFence() { return &m_Fence; }

        ID3D12GraphicsCommandList* GetGraphicsCommandList(ID3D12CommandList *pCommandList = nullptr) { return static_cast<ID3D12GraphicsCommandList * const>(pCommandList ? pCommandList : m_pCommandList.get()); }

        bool WaitForFenceValueInternal(bool IsImmediateContextThread, UINT64 FenceValue);

    private:
        void ResetCommandListTrackingData()
        {
            m_NumCommands = 0;
            m_NumDispatches = 0;
            m_UploadHeapSpaceAllocated = 0;
        }

        void SubmitCommandListImpl();

        ImmediateContext* const                             m_pParent; // weak-ref
        unique_comptr<ID3D12CommandList>                    m_pCommandList;
        unique_comptr<ID3D12CommandAllocator>               m_pCommandAllocator;
        unique_comptr<ID3D12CommandQueue>                   m_pCommandQueue;
        unique_comptr<ID3D12SharingContract>                m_pSharingContract;
        Fence                                               m_Fence{m_pParent, FENCE_FLAG_NONE, 0};
#if TRANSLATION_LAYER_DBG
        Fence                                               m_StallFence{m_pParent, FENCE_FLAG_NONE, 0};
#endif
        std::unique_ptr<ResidencySet>      m_pResidencySet;
        UINT                                                m_NumFlushesWithNoReadback = 0;
        UINT                                                m_NumCommands = 0;
        UINT                                                m_NumDispatches = 0;
        UINT64                                              m_UploadHeapSpaceAllocated = 0;
        ThrowingSafeHandle                                  m_hWaitEvent;

        // The more upload heap space allocated in a command list, the more memory we are 
        // potentially holding up that could have been recycled into the pool. If too
        // much is held up, flush the command list
        static constexpr UINT cMaxAllocatedUploadHeapSpacePerCommandList = 256 * 1024 * 1024;

        DWORD                                               m_MaxAllocatedUploadHeapSpacePerCommandList;

        // Command allocator pools
        CBoundedFencePool< unique_comptr<ID3D12CommandAllocator> > m_AllocatorPool;

        // Some notes on threading related to this command list ID / fence value.
        // The fence value is and should only ever be written by the immediate context thread.
        // The immediate context thread may read the fence value through GetCommandListID().
        // Other threads may read this value, but should only do so via CommandListIDInterlockedRead().
        UINT64 m_commandListID = 1;

        // Number of maximum in-flight command lists at a given time
        static constexpr UINT GetMaxInFlightDepth()
        {
            return 1024;
        };

        void SubmitFence() noexcept;
        void CloseCommandList(ID3D12CommandList *pCommandList);
        void PrepareNewCommandList();
        void IncrementFence();
        void UpdateLastUsedCommandListIDs();
    };

}  // namespace D3D12TranslationLayer
