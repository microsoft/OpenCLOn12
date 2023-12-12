// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "Allocator.h"
#include "CommandListManager.hpp"
#include "CommandListManager.inl"
#include "ImmediateContext.hpp"
#include "Residency.h"
#include "Resource.hpp"

namespace D3D12TranslationLayer
{
    //==================================================================================================================================
    // 
    //==================================================================================================================================

    //----------------------------------------------------------------------------------------------------------------------------------
    CommandListManager::CommandListManager(ImmediateContext *pParent, ID3D12CommandQueue *pQueue)
        : m_pParent(pParent)
        , m_pCommandQueue(pQueue)
        , m_pCommandList(nullptr)
        , m_pCommandAllocator(nullptr)
        , m_AllocatorPool(false /*bLock*/, GetMaxInFlightDepth())
        , m_hWaitEvent(CreateEvent(nullptr, FALSE, FALSE, nullptr)) // throw( _com_error )
        , m_MaxAllocatedUploadHeapSpacePerCommandList(cMaxAllocatedUploadHeapSpacePerCommandList)
    {
        ResetCommandListTrackingData();
        
        if (!m_pCommandQueue)
        {
            D3D12_COMMAND_QUEUE_DESC queue = {};
            queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            queue.NodeMask = 1;
            queue.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            CComPtr<ID3D12Device9> spDevice9;
            if (SUCCEEDED(m_pParent->m_pDevice12->QueryInterface(&spDevice9)))
            {
                ThrowFailure(spDevice9->CreateCommandQueue1(&queue, m_pParent->m_CreationArgs.CreatorID, IID_PPV_ARGS(&m_pCommandQueue)));
            }
            else
            {
                ThrowFailure(m_pParent->m_pDevice12->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_pCommandQueue)));
            }
        }

        PrepareNewCommandList();

        m_pCommandQueue->QueryInterface(&m_pSharingContract); // Ignore failure, interface not always present.
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CommandListManager::~CommandListManager()
    {
    }

    void CommandListManager::ReadbackInitiated() noexcept
    { 
        m_NumFlushesWithNoReadback = 0;
    }

    void CommandListManager::AdditionalCommandsAdded() noexcept
    { 
        m_NumCommands++;
    }

    void CommandListManager::DispatchCommandAdded() noexcept
    {
        m_NumDispatches++;
        m_NumCommands++;
    }

    void CommandListManager::UploadHeapSpaceAllocated(UINT64 heapSize) noexcept
    {
        m_UploadHeapSpaceAllocated += heapSize;
    }

    void CommandListManager::SubmitCommandListIfNeeded()
    {
        // TODO: Heuristics below haven't been heavily profiled, we'll likely want to re-visit and tune
        // this based on multiple factors when profiling (i.e. number of draws, amount of memory 
        // referenced, etc.), possibly changing on an app-by-app basis

        // These parameters attempt to avoid regressing already CPU bound applications. 
        // In these cases, submitting too frequently will make the app slower due
        // to frequently re-emitting state and the overhead of submitting/creating command lists
        static const UINT cMinDrawsOrDispatchesForSubmit = 512;
        static const UINT cMinRenderOpsForSubmit = 1000;

        // To further avoid regressing CPU bound applications, we'll stop opportunistic
        // flushing if it appears that the app doesn't need to kick off work early.
        static const UINT cMinFlushesWithNoCPUReadback = 50;

        const bool bHaveEnoughCommandsForSubmit = 
            m_NumCommands > cMinRenderOpsForSubmit ||
            m_NumDispatches > cMinDrawsOrDispatchesForSubmit;
        const bool bShouldOpportunisticFlush =
            m_NumFlushesWithNoReadback < cMinFlushesWithNoCPUReadback;
        const bool bShouldFreeUpMemory =
            m_UploadHeapSpaceAllocated > m_MaxAllocatedUploadHeapSpacePerCommandList;
        if ((bHaveEnoughCommandsForSubmit && bShouldOpportunisticFlush) ||
            bShouldFreeUpMemory)
        {
            // If the GPU is idle, submit work to keep it busy
            if (m_Fence.GetCompletedValue() == m_commandListID - 1)
            {
                SubmitCommandListImpl();
            }
        }
    }


    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::PrepareNewCommandList()
    {
        HRESULT hr = S_OK;
        // Acquire a command allocator from the pool (or create a new one)
        auto pfnCreateNew = [](ID3D12Device* pDevice12, D3D12_COMMAND_LIST_TYPE type) -> unique_comptr<ID3D12CommandAllocator> // noexcept(false)
        {
            unique_comptr<ID3D12CommandAllocator> spAllocator;
            HRESULT hr = pDevice12->CreateCommandAllocator(
                type,
                IID_PPV_ARGS(&spAllocator)
                );
            ThrowFailure(hr); // throw( _com_error )

            return std::move(spAllocator);
        };

        auto pfnWaitForFence = [&](UINT64 fenceVal) -> bool // noexcept(false)
        {
            return WaitForFenceValue(fenceVal);
        };

        UINT64 CurrentFence = m_Fence.GetCompletedValue();

        m_pCommandAllocator = m_AllocatorPool.RetrieveFromPool(
            CurrentFence,
            pfnWaitForFence,
            pfnCreateNew,
            m_pParent->m_pDevice12.get(),
            D3D12_COMMAND_LIST_TYPE_COMPUTE
            );

        // Create or recycle a command list
        if (m_pCommandList)
        {
            // Recycle the previously created command list
            ResetCommandList();
        }
        else
        {
            // Create a new command list
            hr = m_pParent->m_pDevice12->CreateCommandList(1,
                D3D12_COMMAND_LIST_TYPE_COMPUTE,
                m_pCommandAllocator.get(),
                nullptr,
                IID_PPV_ARGS(&m_pCommandList));
        }
        ThrowFailure(hr); // throw( _com_error )

        ResetResidencySet();
        ResetCommandListTrackingData();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::ResetResidencySet()
    {
        m_pResidencySet = std::make_unique<ResidencySet>();
        m_pResidencySet->Open(0);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::SubmitCommandList()
    {
        ++m_NumFlushesWithNoReadback;
        SubmitCommandListImpl();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::SubmitCommandListImpl() // throws
    {
        CloseCommandList(m_pCommandList.get()); // throws

        m_pResidencySet->Close();

        m_pParent->GetResidencyManager().ExecuteCommandList(m_pCommandQueue.get(), 0, m_pCommandList.get(), m_pResidencySet.get());

        // Return the command allocator to the pool for recycling
        m_AllocatorPool.ReturnToPool(std::move(m_pCommandAllocator), m_commandListID);

        SubmitFence();

        PrepareNewCommandList();
        m_pParent->PostSubmitNotification();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::ResetCommandList()
    {
        // Reset the command allocator (indicating that the driver can recycle memory associated with it)
        ThrowFailure(m_pCommandAllocator->Reset());

        ID3D12GraphicsCommandList *pGraphicsCommandList = GetGraphicsCommandList(m_pCommandList.get());
        ThrowFailure(pGraphicsCommandList->Reset(m_pCommandAllocator.get(), nullptr));
        InitCommandList();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::InitCommandList()
    {
        ID3D12GraphicsCommandList *pGraphicsCommandList = GetGraphicsCommandList(m_pCommandList.get());
        ID3D12DescriptorHeap* pHeaps[2] = { m_pParent->m_ViewHeap.m_pDescriptorHeap.get(), m_pParent->m_SamplerHeap.m_pDescriptorHeap.get() };
        // Sampler heap is null for compute-only devices; don't include it in the count.
        pGraphicsCommandList->SetDescriptorHeaps(m_pParent->ComputeOnly() ? 1 : 2, pHeaps);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::CloseCommandList(ID3D12CommandList *pCommandList)
    {
        ThrowFailure(GetGraphicsCommandList(pCommandList)->Close());
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::AddResourceToResidencySet(Resource *pResource)
    {
        ManagedObject *pResidencyObject = pResource->GetResidencyHandle();
        if (pResidencyObject)
        {
            assert(pResidencyObject->IsInitialized());
            m_pResidencySet->Insert(pResidencyObject);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::SubmitFence() noexcept
    {
        m_pCommandQueue->Signal(m_Fence.Get(), m_commandListID);
        IncrementFence();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::IncrementFence()
    {
        InterlockedIncrement64((volatile LONGLONG*)&m_commandListID);
        UpdateLastUsedCommandListIDs();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::UpdateLastUsedCommandListIDs()
    {
        // This is required for edge cases where you have a command list that has somethings bound but with no meaningful calls (draw/dispatch),
        // and Flush is called, incrementing the command list ID. If that command list only does resource barriers, some bound objects may not
        // have their CommandListID's updated
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT64 CommandListManager::EnsureFlushedAndFenced()
    {
        m_NumFlushesWithNoReadback = 0;
        PrepForCommandQueueSync(); // throws
        UINT64 FenceValue = GetCommandListID() - 1;

        return FenceValue;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::PrepForCommandQueueSync()
    {
        if (HasCommands())
        {
            SubmitCommandListImpl(); // throws
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    HRESULT CommandListManager::EnqueueSetEvent(HANDLE hEvent) noexcept
    {
        UINT64 FenceValue = 0;
        try {
            FenceValue = EnsureFlushedAndFenced(); // throws
        }
        catch (_com_error& e)
        {
            return e.Error();
        }
        catch (std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }

        return m_Fence.SetEventOnCompletion(FenceValue, hEvent);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool CommandListManager::WaitForCompletion()
    {
        ThrowFailure(EnqueueSetEvent(m_hWaitEvent)); // throws

#ifdef USE_PIX
        PIXNotifyWakeFromFenceSignal(m_hWaitEvent);
#endif
        DWORD waitRet = WaitForSingleObject(m_hWaitEvent, INFINITE);
        UNREFERENCED_PARAMETER(waitRet);
        assert(waitRet == WAIT_OBJECT_0);
        return true;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool CommandListManager::WaitForFenceValue(UINT64 FenceValue)
    {
        m_NumFlushesWithNoReadback = 0;

        return WaitForFenceValueInternal(true, FenceValue); // throws
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool CommandListManager::WaitForFenceValueInternal(bool IsImmediateContextThread, UINT64 FenceValue)
    {
        // Command list ID is the value of the fence that will be signaled on submission
        UINT64 CurCmdListID = IsImmediateContextThread ? m_commandListID : GetCommandListIDInterlockedRead();
        if (CurCmdListID <= FenceValue) // Using <= because value read by this thread might be stale
        {
            if (IsImmediateContextThread)
            {
                assert(HasCommands());
                assert(CurCmdListID == FenceValue);
                SubmitCommandListImpl(); // throws
                CurCmdListID = m_commandListID;
                assert(CurCmdListID > FenceValue);
            }
            else
            {
                return false;
            }
        }

        if (m_Fence.GetCompletedValue() >= FenceValue)
        {
            return true;
        }
        ThrowFailure(m_Fence.SetEventOnCompletion(FenceValue, m_hWaitEvent));

        DWORD waitRet = WaitForSingleObject(m_hWaitEvent, INFINITE);
        UNREFERENCED_PARAMETER(waitRet);
        assert(waitRet == WAIT_OBJECT_0);
        return true;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CommandListManager::DiscardCommandList()
    {
        ResetCommandListTrackingData();
        m_pCommandList = nullptr;

        m_pResidencySet->Close();
    }
}
