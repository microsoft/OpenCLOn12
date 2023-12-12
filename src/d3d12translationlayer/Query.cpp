// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "ImmediateContext.hpp"
#include "Query.hpp"

namespace D3D12TranslationLayer
{
    TimestampQuery::TimestampQuery(ImmediateContext* pDevice) noexcept(false)
        : DeviceChild(pDevice)
    {
        D3D12_QUERY_HEAP_DESC QueryHeapDesc = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1, 1 };

        HRESULT hr = m_pParent->m_pDevice12->CreateQueryHeap(
            &QueryHeapDesc,
            IID_PPV_ARGS(&m_spQueryHeap)
        );
        ThrowFailure(hr); // throw( _com_error )

        // Query data goes into a readback heap for CPU readback in GetData
        m_spResultBuffer = m_pParent->AcquireSuballocatedHeap(
            AllocatorHeapType::Readback, sizeof(UINT64), ResourceAllocationContext::FreeThread); // throw( _com_error )
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void TimestampQuery::End() noexcept
    {
        // Store data in the query object, then resolve into the result buffer
        auto pIface = m_pParent->GetGraphicsCommandList();

        pIface->EndQuery(m_spQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        pIface->ResolveQueryData(
            m_spQueryHeap.get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            0,
            1,
            m_spResultBuffer.GetResource(),
            m_spResultBuffer.GetOffset()
        );
        m_pParent->AdditionalCommandsAdded();
        m_LastUsedCommandListID = m_pParent->GetCommandListID();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT64 TimestampQuery::GetData() noexcept
    {
        void* pMappedData = nullptr;

        CD3DX12_RANGE ReadRange(0, sizeof(UINT64));
        HRESULT hr = m_spResultBuffer.Map(
            0,
            &ReadRange,
            &pMappedData
            );
        ThrowFailure(hr);

        const UINT64* pSrc = reinterpret_cast<const UINT64*>(pMappedData);
        UINT64 result = *pSrc;
        CD3DX12_RANGE WrittenRange(0, 0);
        m_spResultBuffer.Unmap(0, &WrittenRange);
        return result;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    TimestampQuery::~TimestampQuery()
    {
        AddToDeferredDeletionQueue(m_spQueryHeap);
        if (m_spResultBuffer.IsInitialized())
        {
            m_pParent->ReleaseSuballocatedHeap(AllocatorHeapType::Readback, m_spResultBuffer, m_LastUsedCommandListID);
        }
    }
};