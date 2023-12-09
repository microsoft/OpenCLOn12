// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

namespace D3D12TranslationLayer
{
    //==================================================================================================================================
    // Async
    // Stores data responsible for remapping D3D11 async (e.g. queries) to underlying D3D12 async
    //==================================================================================================================================

    enum EQueryType
    {
        e_QUERY_TIMESTAMP,
    };

    class Async : public DeviceChild
    {
    public:
    public:
        Async(ImmediateContext* pDevice, EQueryType Type, UINT CommandListTypeMask) noexcept;
        virtual ~Async() noexcept;

        virtual void Initialize() noexcept(false) = 0;

        void End() noexcept;
        bool GetData(void* pData, UINT DataSize, bool DoNotFlush, bool AsyncGetData) noexcept;

        bool FlushAndPrep(bool DoNotFlush) noexcept;

    protected:
        virtual void EndInternal() noexcept = 0;
        virtual void GetDataInternal(_Out_writes_bytes_(DataSize) void* pData, UINT DataSize) noexcept = 0;

    public:
        EQueryType m_Type;
        UINT64 m_EndedCommandListID[(UINT)COMMAND_LIST_TYPE::MAX_VALID];
        UINT m_CommandListTypeMask;
    };

    class Query : public Async
    {
    public:
        Query(ImmediateContext* pDevice, EQueryType Type, UINT CommandListTypeMask, UINT nInstances = c_DefaultInstancesPerQuery) noexcept
            : Async(pDevice, Type, CommandListTypeMask)
            , m_CurrentInstance(0)
            , m_InstancesPerQuery(nInstances)
        { }
        virtual ~Query();

        virtual void Initialize() noexcept(false);

        UINT GetCurrentInstance() { return m_CurrentInstance;  }

    protected:
        void Suspend() noexcept;
        virtual void EndInternal() noexcept;
        virtual void GetDataInternal(_Out_writes_bytes_(DataSize) void* pData, UINT DataSize) noexcept;

        D3D12_QUERY_TYPE GetType12() const;
        D3D12_QUERY_HEAP_TYPE GetHeapType12() const;
        UINT GetDataSize12() const;
        void AdvanceInstance();
        UINT QueryIndex(UINT Instance);

        static const UINT c_DefaultInstancesPerQuery = 4;

    protected:
        unique_comptr<ID3D12QueryHeap> m_spQueryHeap[(UINT)COMMAND_LIST_TYPE::MAX_VALID];
        D3D12ResourceSuballocation m_spResultBuffer[(UINT)COMMAND_LIST_TYPE::MAX_VALID];
        UINT m_CurrentInstance;
        const UINT m_InstancesPerQuery;
    };
};