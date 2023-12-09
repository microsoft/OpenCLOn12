// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

namespace D3D12TranslationLayer
{
    //==================================================================================================================================
    // Async/query/predicate/counter
    //==================================================================================================================================

    Async::Async(ImmediateContext* pDevice, EQueryType Type, UINT CommandListTypeMask) noexcept
        : DeviceChild(pDevice)
        , m_Type(Type)
        , m_CommandListTypeMask(COMMAND_LIST_TYPE_GRAPHICS_MASK)
    {
        ZeroMemory(m_EndedCommandListID, sizeof(m_EndedCommandListID));
    }

    Async::~Async()
    {
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void Async::End() noexcept
    {
        EndInternal();

        ZeroMemory(m_EndedCommandListID, sizeof(m_EndedCommandListID));
        for (UINT listType = 0; listType < (UINT)COMMAND_LIST_TYPE::MAX_VALID; listType++)
        {
            if (m_CommandListTypeMask & (1 << listType))
            {
                m_EndedCommandListID[listType] = m_pParent->GetCommandListIDWithCommands((COMMAND_LIST_TYPE)listType);
            }
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool Async::GetData(void* pData, UINT DataSize, bool DoNotFlush, bool AsyncGetData) noexcept
    {
        if (!AsyncGetData && !FlushAndPrep(DoNotFlush))
        {
            return false;
        }

        if (pData != nullptr && DataSize != 0)
        {
            GetDataInternal(pData, DataSize);
        }

        return true;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool Async::FlushAndPrep(bool DoNotFlush) noexcept
    {
        bool ret = true;
        for (UINT listType = 0; listType < (UINT)COMMAND_LIST_TYPE::MAX_VALID; listType++)
        {
            if (m_CommandListTypeMask & (1 << listType))
            {
                COMMAND_LIST_TYPE commandListType = (COMMAND_LIST_TYPE)listType;
                if (m_EndedCommandListID[listType] == m_pParent->GetCommandListID(commandListType))
                {
                    if (DoNotFlush)
                    {
                        return false;
                    }
                    
                    // convert exceptions to bool result as method is noexcept and SubmitCommandList throws
                    try
                    {
                        m_pParent->SubmitCommandList(commandListType); // throws
                    }
                    catch (_com_error&)
                    {
                        ret = false;
                    }
                    catch (std::bad_alloc&)
                    {
                        ret = false;
                    }
                }

                UINT64 LastCompletedFence = m_pParent->GetCompletedFenceValue(commandListType);
                if (LastCompletedFence < m_EndedCommandListID[listType])
                {
                    ret = false;
                    continue;
                }
            }
        }
        return ret;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    D3D12_QUERY_TYPE Query::GetType12() const
    {
        switch (m_Type)
        {
        case e_QUERY_TIMESTAMP:
            return D3D12_QUERY_TYPE_TIMESTAMP;
        default:
            assert(false);
            return static_cast<D3D12_QUERY_TYPE>(-1);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    D3D12_QUERY_HEAP_TYPE Query::GetHeapType12() const
    {
        switch (m_Type)
        {
        case e_QUERY_TIMESTAMP:
            return D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        default:
            assert(false);
            return static_cast<D3D12_QUERY_HEAP_TYPE>(-1);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    Query::~Query()
    {
        for (auto& obj : m_spQueryHeap) { AddToDeferredDeletionQueue(obj); }
        for (auto& obj : m_spResultBuffer)
        {
            if (obj.IsInitialized())
            {
                m_pParent->ReleaseSuballocatedHeap(AllocatorHeapType::Readback, obj, m_LastUsedCommandListID);
            }
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void Query::Initialize() noexcept(false)
    {
        // GetNumSubQueries() is > 1 for stream-output queries where 11on12 must accumulate the results from all 4 streams
        // m_InstancesPerQuery is a constant multiplier for all queries.  A new instance is used each time that Suspend/Resume are called
        D3D12_QUERY_HEAP_DESC QueryHeapDesc = { GetHeapType12(), 1 * m_InstancesPerQuery, m_pParent->GetNodeMask() };
        UINT BufferSize = GetDataSize12() * QueryHeapDesc.Count;

        // The only query types that allows non-graphics command list type are TIMESTAMP and VIDEO_STATS for now.
        assert(m_Type == e_QUERY_TIMESTAMP || m_CommandListTypeMask == COMMAND_LIST_TYPE_GRAPHICS_MASK);

        for (UINT listType = 0; listType < (UINT)COMMAND_LIST_TYPE::MAX_VALID; listType++)
        {
            if (!(m_CommandListTypeMask & (1 << listType)))
            {
                continue;
            }
            HRESULT hr = m_pParent->m_pDevice12->CreateQueryHeap(
                &QueryHeapDesc,
                IID_PPV_ARGS(&m_spQueryHeap[listType])
                );
            ThrowFailure(hr); // throw( _com_error )

            // Query data goes into a readback heap for CPU readback in GetData
            {
                m_spResultBuffer[listType] = m_pParent->AcquireSuballocatedHeap(
                    AllocatorHeapType::Readback, BufferSize, ResourceAllocationContext::FreeThread); // throw( _com_error )
            }
        }
        m_CurrentInstance = 0;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void Query::Suspend() noexcept
    {
        assert(m_CurrentInstance < m_InstancesPerQuery);

        // Store data in the query object, then resolve into the result buffer
        UINT DataSize12 = GetDataSize12();
        UINT NumSubQueries = 1;
        D3D12_QUERY_TYPE QueryType12 = GetType12();

        auto DoEndQuery = [&](auto pIface, COMMAND_LIST_TYPE commandListType, UINT subQuery)
        {
            UINT Index = QueryIndex(m_CurrentInstance);

            pIface->EndQuery(
                m_spQueryHeap[(UINT)commandListType].get(),
                static_cast<D3D12_QUERY_TYPE>(QueryType12 + subQuery),
                Index
                );

            pIface->ResolveQueryData(
                m_spQueryHeap[(UINT)commandListType].get(),
                static_cast<D3D12_QUERY_TYPE>(QueryType12 + subQuery),
                Index,
                1,
                m_spResultBuffer[(UINT)commandListType].GetResource(),
                Index * DataSize12 + m_spResultBuffer[(UINT)commandListType].GetOffset()
                );
        };

        for (UINT listType = 0; listType < (UINT)COMMAND_LIST_TYPE::MAX_VALID; listType++)
        {
            if (m_CommandListTypeMask & (1 << listType))
            {
                COMMAND_LIST_TYPE commandListType = (COMMAND_LIST_TYPE)listType;
                for (UINT subquery = 0; subquery < NumSubQueries; subquery++)
                {
                    static_assert(static_cast<UINT>(COMMAND_LIST_TYPE::MAX_VALID) == 3u, "ImmediateContext::DiscardView must support all command list types.");

                    switch (commandListType)
                    {
                    case COMMAND_LIST_TYPE::GRAPHICS:
                        DoEndQuery(m_pParent->GetGraphicsCommandList(), commandListType, subquery);
                        break;
                    }
                }
                m_pParent->AdditionalCommandsAdded(commandListType);
                m_LastUsedCommandListID[listType] = m_pParent->GetCommandListID(commandListType);
            }
        }
    }


    //----------------------------------------------------------------------------------------------------------------------------------
    void Query::EndInternal() noexcept
    {
        m_CurrentInstance = 0;

        assert(m_CurrentInstance < m_InstancesPerQuery);

        // Write data for current instance into the result buffer
        Suspend();

        m_CurrentInstance++;

        assert(m_CurrentInstance <= m_InstancesPerQuery);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void Query::GetDataInternal(_Out_writes_bytes_(DataSize) void* pData, UINT DataSize) noexcept
    {
        assert(m_CurrentInstance <= m_InstancesPerQuery);

        // initialize queries that can be used in multiple command lists
        if (m_Type == e_QUERY_TIMESTAMP)
        {
            if (DataSize < sizeof(UINT64))
            {
                ThrowFailure(E_INVALIDARG);
            }

            UINT64 *pDest = reinterpret_cast<UINT64 *>(pData);
            *pDest = 0;
        }

        for (UINT listType = 0; listType < (UINT)COMMAND_LIST_TYPE::MAX_VALID; listType++)
        {
            if (!(m_CommandListTypeMask & (1 << listType)))
            {
                continue;
            }

            if (m_Type != e_QUERY_TIMESTAMP)
            {
                m_pParent->GetCommandListManager((COMMAND_LIST_TYPE)listType)->ReadbackInitiated();
            }

            void* pMappedData = nullptr;

            CD3DX12_RANGE ReadRange(0, DataSize);
            HRESULT hr = m_spResultBuffer[listType].Map(
                0,
                &ReadRange,
                &pMappedData
                );
            ThrowFailure(hr);

            UINT DataSize12 = GetDataSize12();
            UINT NumSubQueries = 1;

            // All structures are arrays of 64-bit values
            assert(0 == (DataSize12 % sizeof(UINT64)));

            UINT NumCounters = DataSize12 / sizeof(UINT64);

            UINT64 TempBuffer[12];

            static_assert(sizeof(TempBuffer) >= sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS), "Temporary query buffer no large enough.");
            static_assert(sizeof(TempBuffer) >= sizeof(D3D12_QUERY_DATA_SO_STATISTICS), "Temporary query buffer no large enough.");
            assert(sizeof(TempBuffer) >= DataSize12);
            assert(_countof(TempBuffer) >= NumCounters);

            // Accumulate all instances & subqueries into a single value
            // If the query was never issued, then 0 will be returned
            ZeroMemory(TempBuffer, sizeof(TempBuffer));

            const UINT64* pSrc = reinterpret_cast<const UINT64*>(pMappedData);

            for (UINT Instance = 0; Instance < m_CurrentInstance; Instance++)
            {
                for (UINT SubQuery = 0; SubQuery < NumSubQueries; SubQuery++)
                {
                    for (UINT Counter = 0; Counter < NumCounters; Counter++)
                    {
                        TempBuffer[Counter] += pSrc[0];
                        pSrc++;
                    }
                }
            }

            switch (m_Type)
            {
                // 11 and 12 match, need to interpret & merge values from possibly multiple queues
            case e_QUERY_TIMESTAMP:
            {
                UINT64 Timestamp = (UINT64)TempBuffer[0];
                if (Timestamp > *(UINT64 *)pData)
                {
                    *(UINT64 *)pData = Timestamp;
                }
                break;
            }

            default:
            {
                assert(false);
            }
            break;
            }
            CD3DX12_RANGE WrittenRange(0, 0);
            m_spResultBuffer[listType].Unmap(0, &WrittenRange);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT Query::GetDataSize12() const
    {
        // This returns the size of the data written by ResolveQueryData
        UINT Result = 0;

        switch (m_Type)
        {
        case e_QUERY_TIMESTAMP:
            Result = sizeof(UINT64);
            break;

        default:
            assert(false);
        }

        return Result;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void Query::AdvanceInstance()
    {
        // Used during Resume or AutoAdvance to move to the next instance
        assert(m_CurrentInstance < m_InstancesPerQuery);

        if ((m_CurrentInstance + 1) < m_InstancesPerQuery)
        {
            m_CurrentInstance++;
        }
        else
        {
            // Out of instances
            // Wait for the GPU to finish all outstanding work
            m_pParent->WaitForCompletion(m_CommandListTypeMask);

            // Accumulate all results into Instance0
            void* pMappedData = nullptr;

            UINT DataSize12 = GetDataSize12();
            UINT NumSubQueries = 1;

            CD3DX12_RANGE ReadRange(0, DataSize12 * NumSubQueries * m_InstancesPerQuery);
            for (UINT listType = 0; listType < (UINT)COMMAND_LIST_TYPE::MAX_VALID; listType++)
            {
                if (!(m_CommandListTypeMask & (1 << listType)))
                {
                    continue;
                }
                ThrowFailure(m_spResultBuffer[listType].Map(0, &ReadRange, &pMappedData));
                // All structures are arrays of 64-bit values
                assert(0 == (DataSize12 % sizeof(UINT64)));

                UINT NumCountersPerSubQuery = DataSize12 / sizeof(UINT64);
                UINT NumCountersPerInstance = NumCountersPerSubQuery * NumSubQueries;

                UINT64* pInstance0 = reinterpret_cast<UINT64*>(pMappedData);

                for (UINT Instance = 1; Instance <= m_CurrentInstance; Instance++)
                {
                    const UINT64* pInstance = reinterpret_cast<const UINT64*>(pMappedData) + (NumCountersPerInstance * Instance);

                    for (UINT i = 0; i < NumCountersPerInstance; i++)
                    {
                        pInstance0[i] += pInstance[i];
                    }
                }

                CD3DX12_RANGE WrittenRange(0, DataSize12 * NumSubQueries);
                m_spResultBuffer[listType].Unmap(0, &WrittenRange);
            }

            // Instance0 has valid data.  11on12 can re-use the data for instance1 and beyond
            m_CurrentInstance = 1;
        }
        assert(m_CurrentInstance < m_InstancesPerQuery);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT Query::QueryIndex(UINT Instance)
    {
        return Instance;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ImmediateContext::QueryEnd(Async* pAsync)
    {
        pAsync->End();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool ImmediateContext::QueryGetData(Async* pAsync, void* pData, UINT DataSize, bool DoNotFlush, bool AsyncGetData)
    {
        return pAsync->GetData(pData, DataSize, DoNotFlush, AsyncGetData);
    }
};