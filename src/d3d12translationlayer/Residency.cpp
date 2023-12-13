// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "ImmediateContext.hpp"
#include "ImmediateContext.inl"
#include "Residency.h"

namespace D3D12TranslationLayer
{

void Internal::LRUCache::TrimToSyncPointInclusive(INT64 CurrentUsage, INT64 CurrentBudget, std::vector<ID3D12Pageable*> &EvictionList, UINT64 FenceValue)
{
    EvictionList.clear();
    LIST_ENTRY* pResourceEntry = ResidentObjectListHead.Flink;
    while (pResourceEntry != &ResidentObjectListHead)
    {
        ManagedObject* pObject = CONTAINING_RECORD(pResourceEntry, ManagedObject, ListEntry);

        if (CurrentUsage < CurrentBudget)
        {
            return;
        }
        if (pObject->LastUsedFenceValue > FenceValue)
        {
            return;
        }

        assert(pObject->ResidencyStatus == ManagedObject::RESIDENCY_STATUS::RESIDENT);

        if (pObject->IsPinned())
        {
            pResourceEntry = pResourceEntry->Flink;
        }
        else
        {
            EvictionList.push_back(pObject->pUnderlying);
            Evict(pObject);

            CurrentUsage -= pObject->Size;

            pResourceEntry = ResidentObjectListHead.Flink;
        }
    }
}

void Internal::LRUCache::TrimAgedAllocations(UINT64 FenceValue, std::vector<ID3D12Pageable*> &EvictionList, UINT64 CurrentTimeStamp, UINT64 MinDelta)
{
    LIST_ENTRY* pResourceEntry = ResidentObjectListHead.Flink;
    while (pResourceEntry != &ResidentObjectListHead)
    {
        ManagedObject* pObject = CONTAINING_RECORD(pResourceEntry, ManagedObject, ListEntry);

        if (CurrentTimeStamp - pObject->LastUsedTimestamp <= MinDelta) // Don't evict things which have been used recently
        {
            return;
        }
        if (pObject->LastUsedFenceValue > FenceValue)
        {
            return;
        }

        assert(pObject->ResidencyStatus == ManagedObject::RESIDENCY_STATUS::RESIDENT);

        if (pObject->IsPinned())
        {
            pResourceEntry = pResourceEntry->Flink;
        }
        else
        {
            EvictionList.push_back(pObject->pUnderlying);
            Evict(pObject);

            pResourceEntry = ResidentObjectListHead.Flink;
        }
    }
}

HRESULT ResidencyManager::Initialize(IDXCoreAdapter *ParentAdapterDXCore)
{
    AdapterDXCore = ParentAdapterDXCore;

    if (FAILED(ImmCtx.m_pDevice12->QueryInterface(&Device)))
    {
        return E_NOINTERFACE;
    }

    LARGE_INTEGER Frequency;
    QueryPerformanceFrequency(&Frequency);

    // Calculate how many QPC ticks are equivalent to the given time in seconds
    MinEvictionGracePeriodTicks = UINT64(Frequency.QuadPart * cMinEvictionGracePeriod);
    MaxEvictionGracePeriodTicks = UINT64(Frequency.QuadPart * cMaxEvictionGracePeriod);
    BudgetQueryPeriodTicks = UINT64(Frequency.QuadPart * cBudgetQueryPeriod);

    HRESULT hr = S_OK;
    hr = AsyncThreadFence.Initialize(Device);

    return hr;
}

HRESULT ResidencyManager::ProcessPagingWork(UINT CommandListIndex, ResidencySet *pMasterSet)
{
    // the size of all the objects which will need to be made resident in order to execute this set.
    UINT64 SizeToMakeResident = 0;

    LARGE_INTEGER CurrentTime;
    QueryPerformanceCounter(&CurrentTime);

    HRESULT hr = S_OK;
    {
        // A lock must be taken here as the state of the objects will be altered
        std::lock_guard Lock(Mutex);

        MakeResidentList.reserve(pMasterSet->Set.size());
        EvictionList.reserve(LRU.NumResidentObjects);

        // Mark the objects used by this command list to be made resident
        for (auto pObject : pMasterSet->Set)
        {
            // If it's evicted we need to make it resident again
            if (pObject->ResidencyStatus == ManagedObject::RESIDENCY_STATUS::EVICTED)
            {
                MakeResidentList.push_back({ pObject });
                LRU.MakeResident(pObject);

                SizeToMakeResident += pObject->Size;
            }

            // Update the last sync point that this was used on
            // Note: This can be used for app command queues as well, but in that case, they'll
            // be pinned rather than relying on this implicit sync point tracking.
            pObject->LastUsedFenceValue = ImmCtx.GetCommandListID();

            pObject->LastUsedTimestamp = CurrentTime.QuadPart;
            LRU.ObjectReferenced(pObject);
        }

        DXCoreAdapterMemoryBudget LocalMemory;
        ZeroMemory(&LocalMemory, sizeof(LocalMemory));
        GetCurrentBudget(CurrentTime.QuadPart, &LocalMemory);

        UINT64 EvictionGracePeriod = GetCurrentEvictionGracePeriod(&LocalMemory);
        UINT64 LastSubmittedFenceValue = ImmCtx.GetCommandListID() - 1;
        UINT64 WaitedFenceValue = ImmCtx.GetCompletedFenceValue();
        LRU.TrimAgedAllocations(WaitedFenceValue, EvictionList, CurrentTime.QuadPart, EvictionGracePeriod);

        if (!EvictionList.empty())
        {
            [[maybe_unused]] HRESULT hrEvict = Device->Evict((UINT)EvictionList.size(), EvictionList.data());
            assert(SUCCEEDED(hrEvict));
            EvictionList.clear();
        }

        if (!MakeResidentList.empty())
        {
            UINT32 ObjectsMadeResident = 0;
            UINT32 MakeResidentIndex = 0;
            while (true)
            {
                INT64 TotalUsage = LocalMemory.currentUsage;
                INT64 TotalBudget = LocalMemory.budget;

                INT64 AvailableSpace = TotalBudget - TotalUsage;

                UINT64 BatchSize = 0;
                UINT32 NumObjectsInBatch = 0;
                UINT32 BatchStart = MakeResidentIndex;

                if (AvailableSpace > 0)
                {
                    for (UINT32 i = MakeResidentIndex; i < MakeResidentList.size(); i++)
                    {
                        // If we try to make this object resident, will we go over budget?
                        if (BatchSize + MakeResidentList[i].pManagedObject->Size > UINT64(AvailableSpace))
                        {
                            // Next time we will start here
                            MakeResidentIndex = i;
                            break;
                        }
                        else
                        {
                            BatchSize += MakeResidentList[i].pManagedObject->Size;
                            NumObjectsInBatch++;
                            ObjectsMadeResident++;

                            MakeResidentList[i].pUnderlying = MakeResidentList[i].pManagedObject->pUnderlying;
                        }
                    }

                    hr = Device->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE,
                                                     NumObjectsInBatch,
                                                     &MakeResidentList[BatchStart].pUnderlying,
                                                     AsyncThreadFence.pFence,
                                                     AsyncThreadFence.FenceValue + 1);
                    if (SUCCEEDED(hr))
                    {
                        AsyncThreadFence.Increment();
                        SizeToMakeResident -= BatchSize;
                    }
                }

                if (FAILED(hr) || ObjectsMadeResident != MakeResidentList.size())
                {
                    ManagedObject *pResidentHead = LRU.GetResidentListHead();
                    while (pResidentHead && pResidentHead->IsPinned())
                    {
                        pResidentHead = CONTAINING_RECORD(pResidentHead->ListEntry.Flink, ManagedObject, ListEntry);
                    }

                    // If there is nothing to trim OR the only objects 'Resident' are the ones about to be used by this execute.
                    bool ForceResidency = pResidentHead == nullptr || pResidentHead->LastUsedFenceValue > LastSubmittedFenceValue;
                    if (ForceResidency)
                    {
                        // Make resident the rest of the objects as there is nothing left to trim
                        UINT32 NumObjects = (UINT32)MakeResidentList.size() - ObjectsMadeResident;

                        // Gather up the remaining underlying objects
                        for (UINT32 i = MakeResidentIndex; i < MakeResidentList.size(); i++)
                        {
                            MakeResidentList[i].pUnderlying = MakeResidentList[i].pManagedObject->pUnderlying;
                        }

                        hr = Device->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE,
                                                         NumObjects,
                                                         &MakeResidentList[MakeResidentIndex].pUnderlying,
                                                         AsyncThreadFence.pFence,
                                                         AsyncThreadFence.FenceValue + 1);
                        if (SUCCEEDED(hr))
                        {
                            AsyncThreadFence.Increment();
                        }
                        if (FAILED(hr))
                        {
                            // TODO: What should we do if this fails? This is a catastrophic failure in which the app is trying to use more memory
                            //       in 1 command list than can possibly be made resident by the system.
                            assert(SUCCEEDED(hr));
                        }
                        break;
                    }

                    // Wait until the GPU is done
                    UINT64 FenceValueToWaitFor = pResidentHead ? pResidentHead->LastUsedFenceValue : LastSubmittedFenceValue;
                    ImmCtx.WaitForFenceValue(FenceValueToWaitFor);
                    WaitedFenceValue = FenceValueToWaitFor;

                    LRU.TrimToSyncPointInclusive(TotalUsage + INT64(SizeToMakeResident), TotalBudget, EvictionList, WaitedFenceValue);

                    [[maybe_unused]] HRESULT hrEvict = Device->Evict((UINT)EvictionList.size(), EvictionList.data());
                    assert(SUCCEEDED(hrEvict));
                }
                else
                {
                    // We made everything resident, mission accomplished
                    break;
                }
            }
        }

        MakeResidentList.clear();
        EvictionList.clear();
        return hr;
    }
}

static void GetDXCoreBudget(IDXCoreAdapter *AdapterDXCore, DXCoreAdapterMemoryBudget *InfoOut, DXCoreSegmentGroup Segment)
{
    DXCoreAdapterMemoryBudgetNodeSegmentGroup InputParams = {};
    InputParams.segmentGroup = Segment;

    [[maybe_unused]] HRESULT hr = AdapterDXCore->QueryState(DXCoreAdapterState::AdapterMemoryBudget, &InputParams, InfoOut);
    assert(SUCCEEDED(hr));
}

void ResidencyManager::GetCurrentBudget(UINT64 Timestamp, DXCoreAdapterMemoryBudget* InfoOut)
{
    if (Timestamp - LastBudgetTimestamp >= BudgetQueryPeriodTicks)
    {
        LastBudgetTimestamp = Timestamp;
        DXCoreAdapterMemoryBudget Local, Nonlocal;
        GetDXCoreBudget(AdapterDXCore, &Local, DXCoreSegmentGroup::Local);
        GetDXCoreBudget(AdapterDXCore, &Nonlocal, DXCoreSegmentGroup::NonLocal);
        CachedBudget.currentUsage = Local.currentUsage + Nonlocal.currentUsage;
        CachedBudget.budget = Local.budget + Nonlocal.budget;
    }
    *InfoOut = CachedBudget;
}
}
