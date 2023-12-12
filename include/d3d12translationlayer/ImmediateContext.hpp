// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include <deque>
#include <functional>
#include <queue>

namespace D3D12TranslationLayer
{
class Resource;
class CommandListManager;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// A pool of objects that are recycled on specific fence values
// This class assumes single threaded caller
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template<typename TResourceType>
class CFencePool
{
public:
    void ReturnToPool(TResourceType&& Resource, UINT64 FenceValue) noexcept
    {
        try
        {
            auto lock = m_pLock ? std::unique_lock(*m_pLock) : std::unique_lock<std::mutex>();
            m_Pool.emplace_back(FenceValue, std::move(Resource)); // throw( bad_alloc )
        }
        catch (std::bad_alloc&)
        {
            // Just drop the error
            // All uses of this pool use unique_comptr, which will release the resource
        }
    }

    template <typename PFNCreateNew, typename... CreationArgType>
    TResourceType RetrieveFromPool(UINT64 CurrentFenceValue, PFNCreateNew pfnCreateNew, const CreationArgType&... CreationArgs) noexcept(false)
    {
        auto lock = m_pLock ? std::unique_lock(*m_pLock) : std::unique_lock<std::mutex>();
        TPool::iterator Head = m_Pool.begin();
        if (Head == m_Pool.end() || (CurrentFenceValue < Head->first))
        {
            return std::move(pfnCreateNew(CreationArgs...)); // throw( _com_error )
        }

        assert(Head->second);
        TResourceType ret = std::move(Head->second);
        m_Pool.erase(Head);
        return std::move(ret);
    }

    void Trim(UINT64 TrimThreshold, UINT64 CurrentFenceValue)
    {
        auto lock = m_pLock ? std::unique_lock(*m_pLock) : std::unique_lock<std::mutex>();

        TPool::iterator Head = m_Pool.begin();

        if (Head == m_Pool.end() || (CurrentFenceValue < Head->first))
        {
            return;
        }

        UINT64 difference = CurrentFenceValue - Head->first;

        if (difference >= TrimThreshold)
        {
            // only erase one item per 'pump'
            assert(Head->second);
            m_Pool.erase(Head);
        }
    }

    CFencePool(bool bLock = false) noexcept
        : m_pLock(bLock ? new std::mutex : nullptr)
    {
    }
    CFencePool(CFencePool &&other) noexcept
    {
        m_Pool = std::move(other.m_Pool);
        m_pLock = std::move(other.m_pLock);
    }
    CFencePool& operator=(CFencePool &&other) noexcept
    {
        m_Pool = std::move(other.m_Pool);
        m_pLock = std::move(other.m_pLock);
        return *this;
    }

protected:
    typedef std::pair<UINT64, TResourceType> TPoolEntry;
    typedef std::list<TPoolEntry> TPool;

    CFencePool(CFencePool const& other) = delete;
    CFencePool& operator=(CFencePool const& other) = delete;

protected:
    TPool m_Pool;
    std::unique_ptr<std::mutex> m_pLock;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// A pool of objects that are recycled on specific fence values
// with a maximum depth before blocking on RetrieveFromPool
// This class assumes single threaded caller
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template<typename TResourceType>
class CBoundedFencePool : public CFencePool<TResourceType>
{
public:

    template <typename PFNWaitForFenceValue, typename PFNCreateNew, typename... CreationArgType>
    TResourceType RetrieveFromPool(UINT64 CurrentFenceValue, PFNWaitForFenceValue pfnWaitForFenceValue, PFNCreateNew pfnCreateNew, const CreationArgType&... CreationArgs) noexcept(false)
    {
        auto lock = m_pLock ? std::unique_lock(*m_pLock) : std::unique_lock<std::mutex>();
        TPool::iterator Head = m_Pool.begin();

        if (Head == m_Pool.end())
        {
            return std::move(pfnCreateNew(CreationArgs...)); // throw( _com_error )
        }
        else if (CurrentFenceValue < Head->first)
        {
            if (m_Pool.size() < m_MaxInFlightDepth)
            {
                return std::move(pfnCreateNew(CreationArgs...)); // throw( _com_error )
            }
            else
            {
                pfnWaitForFenceValue(Head->first); // throw( _com_error )
            }
        }

        assert(Head->second);
        TResourceType ret = std::move(Head->second);
        m_Pool.erase(Head);
        return std::move(ret);
    }

    CBoundedFencePool(bool bLock = false, UINT MaxInFlightDepth = UINT_MAX) noexcept
        : CFencePool(bLock),
        m_MaxInFlightDepth(MaxInFlightDepth)
    {
    }
    CBoundedFencePool(CBoundedFencePool&& other) noexcept
        : CFencePool(other),
        m_MaxInFlightDepth(other.m_MaxInFlightDepth)
    {
    }
    CBoundedFencePool& operator=(CBoundedFencePool&& other) noexcept
    {
        m_Pool = std::move(other.m_Pool);
        m_pLock = std::move(other.m_pLock);
        m_MaxInFlightDepth = other.m_MaxInFlightDepth;
        return *this;
    }

protected:
    UINT m_MaxInFlightDepth;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Multi-level pool (for dynamic resource data upload)
// This class is free-threaded (to enable D3D11 free-threaded resource destruction)
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename TResourceType, UINT64 ResourceSizeMultiple>
class CMultiLevelPool
{
public:
    CMultiLevelPool(UINT64 TrimThreshold, bool bLock)
        : m_TrimThreshold(TrimThreshold)
    {
    }

    void ReturnToPool(UINT64 Size, TResourceType&& Resource, UINT64 FenceValue) noexcept
    {
        UINT PoolIndex = IndexFromSize(Size);
        auto Lock = std::lock_guard(m_Lock);

        if (PoolIndex >= m_MultiPool.size())
        {
            m_MultiPool.resize(PoolIndex + 1);
        }

        m_MultiPool[PoolIndex].ReturnToPool(std::move(Resource), FenceValue);
    }

    template <typename PFNCreateNew>
    TResourceType RetrieveFromPool(UINT64 Size, UINT64 CurrentFenceValue, PFNCreateNew pfnCreateNew) noexcept(false)
    {
        UINT PoolIndex = IndexFromSize(Size);
        UINT AlignedSize = (PoolIndex + 1) * ResourceSizeMultiple;

        auto Lock = std::unique_lock(m_Lock);

        if (PoolIndex >= m_MultiPool.size())
        {
            // pfnCreateNew might be expensive, and won't touch the data structure
            if (Lock.owns_lock())
            {
                Lock.unlock();
            }
            return std::move(pfnCreateNew(AlignedSize)); // throw( _com_error )
        }
        ASSUME(PoolIndex < m_MultiPool.size());

        // Note that RetrieveFromPool can call pfnCreateNew
        // m_Lock will be held during this potentially slow operation
        // This is not optimized because it is expected that once an app reaches steady-state
        // behavior, the pool will not need to grow.
        return std::move(m_MultiPool[PoolIndex].RetrieveFromPool(CurrentFenceValue, pfnCreateNew, AlignedSize)); // throw( _com_error )
    }

    void Trim(UINT64 CurrentFenceValue)
    {
        auto Lock = std::lock_guard(m_Lock);

        for (TPool& pool : m_MultiPool)
        {
            pool.Trim(m_TrimThreshold, CurrentFenceValue);
        }
    }

protected:
    UINT IndexFromSize(UINT64 Size) noexcept { return (Size == 0) ? 0 : (UINT)((Size - 1) / ResourceSizeMultiple); }

protected:
    typedef CFencePool<TResourceType> TPool;
    typedef std::vector<TPool> TMultiPool;

protected:
    TMultiPool m_MultiPool;
    std::mutex m_Lock;
    UINT64 m_TrimThreshold;
};

typedef CMultiLevelPool<unique_comptr<ID3D12Resource>, 64*1024> TDynamicBufferPool;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Fenced Ring Buffer
// A simple ring buffer which keeps track of allocations on the GPU time line
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class CFencedRingBuffer
{
public:

    CFencedRingBuffer(UINT32 Size = 0)
        : m_Size(Size)
        , m_Head(Size)
        , m_Ledger{}
    {}

    HRESULT Allocate(UINT32 NumItems, UINT64 CurrentFenceValue, _Out_ UINT32& OffsetOut)
    {
        assert(m_Size > 0);
        assert(NumItems < m_Size / 2);

        if (NumItems == 0)
        {
            OffsetOut = DereferenceTail();
            return S_OK;
        }

        if (CurrentFenceValue > GetCurrentLedgeEntry().m_FenceValue)
        {
            if (FAILED(MoveToNextLedgerEntry(CurrentFenceValue)))
            {
                return E_FAIL;
            }
        }

        UINT64 tailLocation = DereferenceTail();

        // Allocations need to be contiguous
        if (tailLocation + NumItems > m_Size)
        {
            UINT64 remainder = m_Size - tailLocation;
            UINT32 dummy = 0;
            // Throw away the difference so we can allocate a contiguous block
            if (FAILED(Allocate(UINT32(remainder), CurrentFenceValue, dummy)))
            {
                return E_FAIL;
            }
        }

        if (m_Tail + NumItems <= m_Head)
        {
            // The tail could have moved due to alignment so deref again
            OffsetOut = DereferenceTail();
            GetCurrentLedgeEntry().m_NumAllocations += NumItems;
            m_Tail += NumItems;
            return S_OK;
        }
        else
        {
            OffsetOut = UINT32(-1);
            return E_FAIL;
        }
    }

    void Deallocate(UINT64 CompletedFenceValue)
    {
        for (size_t i = 0; i < _countof(m_Ledger); i++)
        {
            LedgerEntry& entry = m_Ledger[i];

            const UINT32 bit = (1 << i);

            if ((m_LedgerMask & bit) && entry.m_FenceValue <= CompletedFenceValue)
            {
                // Dealloc
                m_Head += entry.m_NumAllocations;
                entry = {};

                // Unset the bit
                m_LedgerMask &= ~(bit);
            }

            if (m_LedgerMask == 0)
            {
                break;
            }
        }
    }

private:

    inline UINT32 DereferenceTail() const { return m_Tail % m_Size; }

    UINT64 m_Head = 0;
    UINT64 m_Tail = 0;
    UINT32 m_Size;

    struct LedgerEntry
    {
        UINT64 m_FenceValue;
        UINT32 m_NumAllocations;
    };

    // TODO: If we define a max lag between CPU and GPU this should be set to slightly more than that
    static const UINT32 cLedgerSize = 16;

    LedgerEntry m_Ledger[cLedgerSize];
    UINT32 m_LedgerMask = 0x1;
    static_assert(cLedgerSize <= std::numeric_limits<decltype(m_LedgerMask)>::digits);

    UINT32 m_LedgerIndex = 0;

    LedgerEntry& GetCurrentLedgeEntry() { return  m_Ledger[m_LedgerIndex]; }

    bool IsLedgerEntryAvailable(UINT32 Index) const { return (m_LedgerMask & (1 << Index)) == 0; }

    HRESULT MoveToNextLedgerEntry(UINT64 CurrentFenceValue)
    {
        m_LedgerIndex++;
        m_LedgerIndex %= cLedgerSize;

        if (IsLedgerEntryAvailable(m_LedgerIndex))
        {
            m_LedgerMask |= (1 << m_LedgerIndex);

            GetCurrentLedgeEntry().m_NumAllocations = 0;
            GetCurrentLedgeEntry().m_FenceValue = CurrentFenceValue;

            return S_OK;
        }
        else
        {
            return E_FAIL;
        }
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Descriptor heap manager
// Used to allocate descriptors from CPU-only heaps corresponding to view/sampler objects
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
class CDescriptorHeapManager
{
public: // Types
    typedef D3D12_CPU_DESCRIPTOR_HANDLE HeapOffset;
    typedef decltype(HeapOffset::ptr) HeapOffsetRaw;
    typedef UINT HeapIndex;

private: // Types
    struct SFreeRange { HeapOffsetRaw Start; HeapOffsetRaw End; };
    struct SHeapEntry
    {
        unique_comptr<ID3D12DescriptorHeap> m_Heap;
        std::list<SFreeRange> m_FreeList;

        SHeapEntry() { }
        SHeapEntry(SHeapEntry &&o) : m_Heap(std::move(o.m_Heap)), m_FreeList(std::move(o.m_FreeList)) { }
    };

    // Note: This data structure relies on the pointer validity guarantee of std::deque,
    // that as long as inserts/deletes are only on either end of the container, pointers
    // to elements continue to be valid. If trimming becomes an option, the free heap
    // list must be re-generated at that time.
    typedef std::deque<SHeapEntry> THeapMap;

public: // Methods
    CDescriptorHeapManager(ID3D12Device* pDevice,
                           D3D12_DESCRIPTOR_HEAP_TYPE Type,
                           UINT NumDescriptorsPerHeap) noexcept
        : m_Desc( { Type,
                    NumDescriptorsPerHeap,
                    D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                    1} )
        , m_DescriptorSize(pDevice->GetDescriptorHandleIncrementSize(Type))
        , m_pDevice(pDevice)
    {
    }

    HeapOffset AllocateHeapSlot(_Out_opt_ HeapIndex *outIndex = nullptr) noexcept(false)
    {
        auto Lock = std::lock_guard(m_CritSect);
        if (m_FreeHeaps.empty())
        {
            AllocateHeap(); // throw( _com_error )
        }
        assert(!m_FreeHeaps.empty());
        HeapIndex index = m_FreeHeaps.front();
        SHeapEntry &HeapEntry = m_Heaps[index];
        assert(!HeapEntry.m_FreeList.empty());
        SFreeRange &Range = *HeapEntry.m_FreeList.begin();
        HeapOffset Ret = { Range.Start };
        Range.Start += m_DescriptorSize;

        if (Range.Start == Range.End)
        {
            HeapEntry.m_FreeList.pop_front();
            if (HeapEntry.m_FreeList.empty())
            {
                m_FreeHeaps.pop_front();
            }
        }
        if (outIndex)
        {
            *outIndex = index;
        }
        return Ret;
    }

    void FreeHeapSlot(HeapOffset Offset, HeapIndex index) noexcept
    {
        auto Lock = std::lock_guard(m_CritSect);
        try
        {
            assert(index < m_Heaps.size());
            SHeapEntry &HeapEntry = m_Heaps[index];

            SFreeRange NewRange = 
            {
                Offset.ptr,
                Offset.ptr + m_DescriptorSize
            };

            bool bFound = false;
            for (auto it = HeapEntry.m_FreeList.begin(), end = HeapEntry.m_FreeList.end();
                 it != end && !bFound;
                 ++it)
            {
                SFreeRange &Range = *it;
                assert(Range.Start <= Range.End);
                if (Range.Start == Offset.ptr + m_DescriptorSize)
                {
                    Range.Start = Offset.ptr;
                    bFound = true;
                }
                else if (Range.End == Offset.ptr)
                {
                    Range.End += m_DescriptorSize;
                    bFound = true;
                }
                else
                {
                    assert(Range.End < Offset.ptr || Range.Start > Offset.ptr);
                    if (Range.Start > Offset.ptr)
                    {
                        HeapEntry.m_FreeList.insert(it, NewRange); // throw( bad_alloc )
                        bFound = true;
                    }
                }
            }

            if (!bFound)
            {
                if (HeapEntry.m_FreeList.empty())
                {
                    m_FreeHeaps.push_back(index); // throw( bad_alloc )
                }
                HeapEntry.m_FreeList.push_back(NewRange); // throw( bad_alloc )
            }
        }
        catch( std::bad_alloc& )
        {
            // Do nothing - there will be slots that can no longer be reclaimed.
        }
    }

private: // Methods
    void AllocateHeap() noexcept(false)
    {
        SHeapEntry NewEntry;
        ThrowFailure( m_pDevice->CreateDescriptorHeap(&m_Desc, IID_PPV_ARGS(&NewEntry.m_Heap)) ); // throw( _com_error )
        HeapOffset HeapBase = NewEntry.m_Heap->GetCPUDescriptorHandleForHeapStart();
        NewEntry.m_FreeList.push_back({HeapBase.ptr,
                                        HeapBase.ptr + m_Desc.NumDescriptors * m_DescriptorSize}); // throw( bad_alloc )

        m_Heaps.emplace_back(std::move(NewEntry)); // throw( bad_alloc )
        m_FreeHeaps.push_back(static_cast<HeapIndex>(m_Heaps.size() - 1)); // throw( bad_alloc )
    }

private: // Members
    const D3D12_DESCRIPTOR_HEAP_DESC m_Desc;
    const UINT m_DescriptorSize;
    ID3D12Device* const m_pDevice; // weak-ref
    std::mutex m_CritSect;

    THeapMap m_Heaps;
    std::list<HeapIndex> m_FreeHeaps;
};

// Extra data appended to the end of stream-output buffers
struct SStreamOutputSuffix
{
    UINT BufferFilledSize;
    UINT VertexCountPerInstance;
    UINT InstanceCount;
    UINT StartVertexLocation;
    UINT StartInstanceLocation;
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Core implementation
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum EDirtyBits : UINT64
{
    // Pipeline states:
    // Dirty bits are set when a shader or constant in the PSO desc changes, and causes a PSO lookup/compile
    // Reassert bits are set on command list boundaries, on graphics/compute boundaries, and after dirty processing
    e_PipelineStateDirty  = 0x1,

    e_CSShaderResourcesDirty      = 0x20000,
    e_CSConstantBuffersDirty      = 0x40000,
    e_CSSamplersDirty             = 0x80000,

    e_CSUnorderedAccessViewsDirty = 0x200000,

    e_FirstDispatch               = 0x200000000,

    e_ComputeRootSignatureDirty   = 0x800000000,

    // Combinations of Heap-based bindings, by pipeline type
    e_ComputeBindingsDirty        = e_CSShaderResourcesDirty | e_CSConstantBuffersDirty | e_CSSamplersDirty | e_CSUnorderedAccessViewsDirty,

    // Combinations of heap-based bindings, by heap type
    e_ViewsDirty                  = 
                                    e_CSShaderResourcesDirty | e_CSConstantBuffersDirty | 
                                    e_CSUnorderedAccessViewsDirty,
    e_SamplersDirty               = e_CSSamplersDirty,

    // All heap-based bindings
    e_HeapBindingsDirty           = e_ComputeBindingsDirty,

    // All state bits by pipeline type
    e_ComputeStateDirty           = e_PipelineStateDirty | e_ComputeBindingsDirty | e_FirstDispatch | e_ComputeRootSignatureDirty,

    // Accumulations of state bits set on command list boundaries and initialization
    // New command lists require all state to be reasserted, but nothing new needs to be dirtied.  
    // The first command list associated with a device must treat all heaps as dirty
    // to setup initial descriptor tables
    e_DirtyOnNewCommandList       = 0,
    e_DirtyOnFirstCommandList     = e_HeapBindingsDirty,
    e_ReassertOnNewCommandList    = e_ComputeStateDirty,
};

class ImmediateContext;

struct RetiredObject
{
    RetiredObject() {}
    RetiredObject(UINT64 lastCommandListID) :
        m_lastCommandListID(lastCommandListID)
    {
    }

    static bool ReadyToDestroy(ImmediateContext* pContext, UINT64 lastCommandListID);
    bool ReadyToDestroy(ImmediateContext* pContext) { return ReadyToDestroy(pContext, m_lastCommandListID); }

    UINT64 m_lastCommandListID = 0;
};

struct RetiredD3D12Object : public RetiredObject
{
    RetiredD3D12Object() {}
    RetiredD3D12Object(ID3D12Object* pUnderlying, _In_opt_ std::unique_ptr<ResidencyManagedObjectWrapper> &&pResidencyHandle, UINT64 lastCommandListID) :
        RetiredObject(lastCommandListID)
        , m_pUnderlying(pUnderlying)
        , m_pResidencyHandle(std::move(pResidencyHandle)) {}

    RetiredD3D12Object(RetiredD3D12Object &&retiredObject) :
        RetiredObject(retiredObject)
        , m_pUnderlying(retiredObject.m_pUnderlying)
        , m_pResidencyHandle(std::move(retiredObject.m_pResidencyHandle)) {}
    

    CComPtr<ID3D12Object> m_pUnderlying;
    std::unique_ptr<ResidencyManagedObjectWrapper> m_pResidencyHandle;
};

typedef ConditionalAllocator<HeapSuballocationBlock, UINT64, DirectHeapAllocator, ThreadSafeBuddyHeapAllocator, bool> ConditionalHeapAllocator;
struct RetiredSuballocationBlock : public RetiredObject
{
    RetiredSuballocationBlock(HeapSuballocationBlock &block, ConditionalHeapAllocator &parentAllocator, UINT64 lastCommandListID) :
        RetiredObject(lastCommandListID)
        , m_SuballocatedBlock(block)
        , m_ParentAllocator(parentAllocator) {}

    void Destroy()
    {
        m_ParentAllocator.Deallocate(m_SuballocatedBlock);
    }

    HeapSuballocationBlock m_SuballocatedBlock;
    ConditionalHeapAllocator &m_ParentAllocator;
};

class DeferredDeletionQueueManager
{
public:
    DeferredDeletionQueueManager(ImmediateContext *pContext)
        : m_pParent(pContext)
    {}

    ~DeferredDeletionQueueManager() {
        TrimDeletedObjects(true);
    }

    bool TrimDeletedObjects(bool deviceBeingDestroyed = false);
    UINT64 GetFenceValueForObjectDeletion();
    UINT64 GetFenceValueForSuballocationDeletion();

    void AddObjectToQueue(ID3D12Object* pUnderlying, std::unique_ptr<ResidencyManagedObjectWrapper> &&pResidencyHandle, UINT64 lastCommandListID)
    {
        m_DeferredObjectDeletionQueue.push(RetiredD3D12Object(pUnderlying, std::move(pResidencyHandle), lastCommandListID));
    }

    void AddSuballocationToQueue(HeapSuballocationBlock &suballocation, ConditionalHeapAllocator &parentAllocator, UINT64 lastCommandListID)
    {
        RetiredSuballocationBlock retiredSuballocation(suballocation, parentAllocator, lastCommandListID);
        if (!retiredSuballocation.ReadyToDestroy(m_pParent))
        {
            m_DeferredSuballocationDeletionQueue.push(retiredSuballocation);
        }
        else
        {
            retiredSuballocation.Destroy();
        }
    }

private:
    bool SuballocationsReadyToBeDestroyed(bool deviceBeingDestroyed);

    ImmediateContext* m_pParent;
    std::queue<RetiredD3D12Object> m_DeferredObjectDeletionQueue;
    std::queue<RetiredSuballocationBlock> m_DeferredSuballocationDeletionQueue;
};

template <typename T, typename mutex_t = std::mutex> class CLockedContainer
{
    mutex_t m_CS;
    T m_Obj;
public:
    class LockedAccess
    {
        std::unique_lock<mutex_t> m_Lock;
        T& m_Obj;
    public:
        LockedAccess(mutex_t &CS, T& Obj)
            : m_Lock(CS)
            , m_Obj(Obj) { }
        T* operator->() { return &m_Obj; }
    };
    // Intended use: GetLocked()->member.
    // The LockedAccess temporary object ensures synchronization until the end of the expression.
    template <typename... Args> CLockedContainer(Args&&... args) : m_Obj(std::forward<Args>(args)...) { }
    LockedAccess GetLocked() { return LockedAccess(m_CS, m_Obj); }
};

using RenameResourceSet = std::deque<unique_comptr<Resource>>;

class ImmediateContext
{
public:
    // D3D12 objects
    // TODO: const
    const unique_comptr<ID3D12Device> m_pDevice12;
    unique_comptr<IDXCoreAdapter> m_pDXCoreAdapter;
    unique_comptr<ID3D12Device1> m_pDevice12_1;
    unique_comptr<ID3D12Device2> m_pDevice12_2; // TODO: Instead of adding more next time, replace
    unique_comptr<ID3D12CompatibilityDevice> m_pCompatDevice;
    unique_comptr<ID3D12CommandQueue> m_pSyncOnlyQueue;
private:
    std::unique_ptr<CommandListManager> m_CommandList;

    // Residency Manager needs to come after the deferred deletion queue so that defer deleted objects can
    // call EndTrackingObject on a valid residency manager
    ResidencyManager m_residencyManager;

    // It is important that the deferred deletion queue manager gets destroyed last, place solely strict dependencies above.
    CLockedContainer<DeferredDeletionQueueManager> m_DeferredDeletionQueueManager;
public:
    friend class Query;
    friend class CommandListManager;

    class CreationArgs
    {
    public:
        CreationArgs() { ZeroMemory(this, sizeof(*this)); }
        
        GUID CreatorID;
    };

    ImmediateContext(D3D12_FEATURE_DATA_D3D12_OPTIONS& caps,
        ID3D12Device* pDevice, ID3D12CommandQueue* pQueue, CreationArgs args) noexcept(false);
    ~ImmediateContext() noexcept;

    CreationArgs m_CreationArgs;

    CommandListManager *GetCommandListManager() noexcept;
    ID3D12CommandList *GetCommandList() noexcept;
    UINT64 GetCommandListID() noexcept;
    UINT64 GetCommandListIDInterlockedRead() noexcept;
    UINT64 GetCommandListIDWithCommands() noexcept;
    UINT64 GetCompletedFenceValue() noexcept;
    ID3D12CommandQueue *GetCommandQueue() noexcept;
    void ResetCommandList() noexcept;
    void CloseCommandList() noexcept;
    HRESULT EnqueueSetEvent(HANDLE hEvent) noexcept;
    Fence *GetFence() noexcept;
    void SubmitCommandList();

    // Returns true if synchronization was successful, false likely means device is removed
    bool WaitForCompletion();
    bool WaitForFenceValue(UINT64 FenceValue);
    bool WaitForFenceValue(UINT64 FenceValue, bool DoNotWait);

    ID3D12GraphicsCommandList *GetGraphicsCommandList() noexcept;
    void AdditionalCommandsAdded() noexcept;
    void UploadHeapSpaceAllocated(UINT64 HeapSize) noexcept;

    unique_comptr<ID3D12Resource> AllocateHeap(UINT64 HeapSize, UINT64 alignment, AllocatorHeapType heapType) noexcept(false);
    void ClearState() noexcept;

    void AddObjectToResidencySet(Resource *pResource);
    void AddResourceToDeferredDeletionQueue(ID3D12Object* pUnderlying, std::unique_ptr<ResidencyManagedObjectWrapper> &&pResidencyHandle, UINT64 lastCommandListID);
    void AddObjectToDeferredDeletionQueue(ID3D12Object* pUnderlying, UINT64 lastCommandListID);

    bool TrimDeletedObjects(bool deviceBeingDestroyed = false);
    bool TrimResourcePools();

    unique_comptr<ID3D12Resource> AcquireTransitionableUploadBuffer(AllocatorHeapType HeapType, UINT64 Size) noexcept(false);

    void ReturnTransitionableBufferToPool(AllocatorHeapType HeapType, UINT64 Size, unique_comptr<ID3D12Resource>&&spResource, UINT64 FenceValue) noexcept;

    D3D12ResourceSuballocation AcquireSuballocatedHeapForResource(_In_ Resource* pResource, ResourceAllocationContext threadingContext) noexcept(false);
    D3D12ResourceSuballocation AcquireSuballocatedHeap(AllocatorHeapType HeapType, UINT64 Size, ResourceAllocationContext threadingContext, bool bCannotBeOffset = false) noexcept(false);
    void ReleaseSuballocatedHeap(AllocatorHeapType HeapType, D3D12ResourceSuballocation &resource, UINT64 FenceValue) noexcept;

    void ReturnAllBuffersToPool( Resource& UnderlyingResource) noexcept;

    static void UploadDataToMappedBuffer(_In_reads_bytes_(Placement.Depth * DepthPitch) const void* pData, UINT SrcPitch, UINT SrcDepth, 
                                         _Out_writes_bytes_(Placement.Depth * DepthPitch) void* pMappedData,
                                         D3D12_SUBRESOURCE_FOOTPRINT& Placement, UINT DepthPitch, UINT TightRowPitch) noexcept;

    // This is similar to the D3D12 header helper method, but it can handle 11on12-emulated resources, as well as a dst box
    enum class UpdateSubresourcesFlags
    {
        ScenarioImmediateContext,           // Servicing an immediate context operation, e.g. UpdateSubresource API or some kind of clear
        ScenarioInitialData,                // Servicing a free-threaded method, but guaranteed that the dest resource is idle
        ScenarioBatchedContext,             // Servicing a queued operation, but may be occurring in parallel with immediate context operations
        ScenarioImmediateContextInternalOp, // Servicing an internal immediate context operation (e.g. updating UAV/SO counters) and should not respect predication
        ScenarioMask = 0x3,

        None = 0,
        ChannelSwapR10G10B10A2 = 0x4,
    };
    void UpdateSubresources(Resource* pDst,
                            D3D12TranslationLayer::CSubresourceSubset const& Subresources,
                            _In_reads_opt_(_Inexpressible_(Subresources.NumNonExtendedSubresources())) const D3D11_SUBRESOURCE_DATA* pSrcData,
                            _In_opt_ const D3D12_BOX* pDstBox = nullptr,
                            UpdateSubresourcesFlags flags = UpdateSubresourcesFlags::ScenarioImmediateContext,
                            _In_opt_ const void* pClearColor = nullptr );

    struct PreparedUpdateSubresourcesOperation
    {
        UINT64 OffsetAdjustment;                     // 0-8 bytes
        EncodedResourceSuballocation EncodedBlock;   // 8-32 bytes (last 4 bytes padding on x86)
        CSubresourceSubset EncodedSubresourceSubset; // 32-40 bytes
        UINT DstX;                                   // 40-44 bytes
        UINT DstY;                                   // 44-48 bytes
        UINT DstZ;                                   // 48-52 bytes
        bool bDisablePredication;                    // byte 52
        bool bDstBoxPresent;                         // byte 53
        // 2 bytes padding
    };
    static_assert(sizeof(PreparedUpdateSubresourcesOperation) == 56, "Math above is wrong. Check if padding can be removed.");
    struct PreparedUpdateSubresourcesOperationWithLocalPlacement
    {
        PreparedUpdateSubresourcesOperation Base;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT LocalPlacementDescs[2];
    };

    class CPrepareUpdateSubresourcesHelper
    {
    public:
        PreparedUpdateSubresourcesOperationWithLocalPlacement PreparedStorage;
        Resource& Dst;
        CSubresourceSubset const& Subresources;

        const bool bDeInterleavingUpload = Dst.SubresourceMultiplier() > 1;
        const UINT NumSrcData = Subresources.NumNonExtendedSubresources();
        const UINT NumDstSubresources = Subresources.NumExtendedSubresources();

        const UINT8 PlaneCount = (Dst.SubresourceMultiplier() * Dst.AppDesc()->NonOpaquePlaneCount());
        const UINT FirstDstSubresource = ComposeSubresourceIdxExtended(Subresources.m_BeginMip, Subresources.m_BeginArray, Subresources.m_BeginPlane, Dst.AppDesc()->MipLevels(), Dst.AppDesc()->ArraySize());
        const UINT LastDstSubresource = ComposeSubresourceIdxExtended(Subresources.m_EndMip - 1, Subresources.m_EndArray - 1, Subresources.m_EndPlane - 1, Dst.AppDesc()->MipLevels(), Dst.AppDesc()->ArraySize());

        const bool bDisjointSubresources = LastDstSubresource - FirstDstSubresource + 1 != NumDstSubresources;
        const bool bDstBoxPresent;
        const bool bUseLocalPlacement = bDstBoxPresent || bDisjointSubresources;

        bool FinalizeNeeded = false;

    private:
        UINT64 TotalSize = 0;
        D3D12ResourceSuballocation mappableResource;
        UINT bufferOffset = 0;
        bool CachedNeedsTemporaryUploadHeap = false;

    public:
        CPrepareUpdateSubresourcesHelper(Resource& Dst,
                                         CSubresourceSubset const& Subresources,
                                         const D3D11_SUBRESOURCE_DATA* pSrcData,
                                         const D3D12_BOX* pDstBox,
                                         UpdateSubresourcesFlags flags,
                                         const void* pClearPattern,
                                         UINT ClearPatternSize,
                                         ImmediateContext& ImmCtx);

    private:
#if TRANSLATION_LAYER_DBG
        void AssertPreconditions(const D3D11_SUBRESOURCE_DATA* pSrcData, const void* pClearPattern);
#endif

        bool InitializePlacementsAndCalculateSize(const D3D12_BOX* pDstBox, ID3D12Device* pDevice);
        bool NeedToRespectPredication(UpdateSubresourcesFlags flags) const;
        bool NeedTemporaryUploadHeap(UpdateSubresourcesFlags flags, ImmediateContext& ImmCtx) const;
        void InitializeMappableResource(UpdateSubresourcesFlags flags, ImmediateContext& ImmCtx, D3D12_BOX const* pDstBox);
        void UploadSourceDataToMappableResource(void* pDstData, D3D11_SUBRESOURCE_DATA const* pSrcData, ImmediateContext& ImmCtx, UpdateSubresourcesFlags flags);
        void UploadDataToMappableResource(D3D11_SUBRESOURCE_DATA const* pSrcData, ImmediateContext& ImmCtx, D3D12_BOX const* pDstBox, const void* pClearPattern, UINT ClearPatternSize, UpdateSubresourcesFlags flags);
        void WriteOutputParameters(D3D12_BOX const* pDstBox, UpdateSubresourcesFlags flags);
    };
    void FinalizeUpdateSubresources(Resource* pDst, PreparedUpdateSubresourcesOperation const& PreparedStorage, _In_reads_opt_(2) D3D12_PLACED_SUBRESOURCE_FOOTPRINT const* LocalPlacementDescs);

    void CopyAndConvertSubresourceRegion(Resource* pDst, UINT DstSubresource, Resource* pSrc, UINT SrcSubresource, UINT dstX, UINT dstY, UINT dstZ, const D3D12_BOX* pSrcBox) noexcept;

    void UAVBarrier() noexcept;

public:
    
    void Dispatch( UINT, UINT, UINT );

    // Returns if any work was actually submitted
    bool Flush();

    void QueryEnd(Async*);
    bool QueryGetData(Async*, void*, UINT, bool DoNotFlush, bool AsyncGetData = false);

    bool Map(_In_ Resource* pResource, _In_ UINT Subresource, _In_ MAP_TYPE MapType, _In_ bool DoNotWait, _In_opt_ const D3D12_BOX *pReadWriteRange, _Out_ MappedSubresource* pMappedSubresource);
    void Unmap(Resource*, UINT, MAP_TYPE, _In_opt_ const D3D12_BOX *pReadWriteRange);

    bool SynchronizeForMap(Resource* pResource, UINT Subresource, MAP_TYPE MapType, bool DoNotWait);
    bool MapUnderlying(Resource*, UINT, MAP_TYPE, _In_opt_ const D3D12_BOX *pReadWriteRange, MappedSubresource* );
    bool MapUnderlyingSynchronize(Resource*, UINT, MAP_TYPE, bool, _In_opt_ const D3D12_BOX *pReadWriteRange, MappedSubresource* );

    bool MapDynamicTexture( Resource* pResource, UINT Subresource, MAP_TYPE, bool, _In_opt_ const D3D12_BOX *pReadWriteRange, MappedSubresource* );
    bool MapDefault(Resource*pResource, UINT Subresource, MAP_TYPE, bool doNotWait, _In_opt_ const D3D12_BOX *pReadWriteRange, MappedSubresource*);
    void UnmapDefault( Resource* pResource, UINT Subresource, _In_opt_ const D3D12_BOX *pReadWriteRange);
    void UnmapUnderlyingSimple( Resource* pResource, UINT Subresource, _In_opt_ const D3D12_BOX *pReadWriteRange);
    void UnmapUnderlyingStaging( Resource* pResource, UINT Subresource, _In_opt_ const D3D12_BOX *pReadWriteRange);
    void UnmapDynamicTexture( Resource*pResource, UINT Subresource, _In_opt_ const D3D12_BOX *pReadWriteRange, bool bUploadMappedContents);

    void ResourceCopy( Resource*, Resource* );
    void ResourceResolveSubresource( Resource*, UINT, Resource*, UINT, DXGI_FORMAT );

    void ResourceCopyRegion( Resource*, UINT, UINT, UINT, UINT, Resource*, UINT, const D3D12_BOX*);
    void ResourceUpdateSubresourceUP( Resource*, UINT, _In_opt_ const D3D12_BOX*, _In_ const VOID*, UINT, UINT);

    HRESULT CheckFormatSupport(_Out_ D3D12_FEATURE_DATA_FORMAT_SUPPORT& formatData);
    void CheckMultisampleQualityLevels(DXGI_FORMAT, UINT, D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS, _Out_ UINT*);
    void CheckFeatureSupport(D3D12_FEATURE Feature, _Inout_updates_bytes_(FeatureSupportDataSize)void* pFeatureSupportData, UINT FeatureSupportDataSize);

    void Signal(_In_ Fence* pFence, UINT64 Value);
    void Wait(std::shared_ptr<Fence> const& pFence, UINT64 Value);

public:
    bool ResourceAllocationFallback(ResourceAllocationContext threadingContext);

    template <typename TFunc>
    auto TryAllocateResourceWithFallback(TFunc&& allocateFunc, ResourceAllocationContext threadingContext)
    {
        while (true)
        {
            try
            {
                return allocateFunc();
            }
            catch( _com_error& hrEx )
            {
                if (hrEx.Error() != E_OUTOFMEMORY ||
                    !ResourceAllocationFallback(threadingContext))
                {
                    throw;
                }
            }
        }
    }

public: // Type 
    D3D12_BOX GetBoxFromResource(Resource *pSrc, UINT SrcSubresource);
    D3D12_BOX GetSubresourceBoxFromBox(Resource* pSrc, UINT RequestedSubresource, UINT BaseSubresource, D3D12_BOX const& SrcBox);

private: // methods
    // The app should inform the translation layer when a frame has been finished
    // to hint when trimming work should start
    //
    // The translation layer makes guesses at frame ends (i.e. when flush is called) 
    // but isn't aware when a present is done.
    void PostSubmitNotification();

    void PostDispatch();

    void SameResourceCopy(Resource *pSrc, UINT SrcSubresource, Resource *pDst, UINT DstSubresource, UINT dstX, UINT dstY, UINT dstZ, const D3D12_BOX *pSrcBox);

public:
    void PostCopy(Resource *pSrc, UINT startSubresource, Resource *pDest, UINT dstSubresource, UINT totalNumSubresources);

    void CopyDataToBuffer(
        ID3D12Resource* pResource,
        UINT Offset,
        const void* pData,
        UINT Size
        ) noexcept(false);

    bool HasCommands() noexcept;
    void PrepForCommandQueueSync();

private:
    bool Shutdown() noexcept;

public: // Methods
    D3D12_HEAP_PROPERTIES GetHeapProperties(D3D12_HEAP_TYPE Type) const noexcept
    {
        if (ComputeOnly() || Type == D3D12_HEAP_TYPE_DEFAULT)
        {
            return CD3DX12_HEAP_PROPERTIES(Type, 1, 1);
        }
        else
        {
            return m_pDevice12->GetCustomHeapProperties(1, Type);
        }
    }

    const D3D12_FEATURE_DATA_D3D12_OPTIONS& GetCaps() { return m_caps; }
    bool ComputeOnly() const {return !!(FeatureLevel() == D3D_FEATURE_LEVEL_1_0_CORE);}
public: // variables

    // "Online" descriptor heaps
    struct OnlineDescriptorHeap
    {
        unique_comptr<ID3D12DescriptorHeap> m_pDescriptorHeap;
        decltype(D3D12_GPU_DESCRIPTOR_HANDLE::ptr) m_DescriptorHeapBase;
        decltype(D3D12_CPU_DESCRIPTOR_HANDLE::ptr) m_DescriptorHeapBaseCPU;

        D3D12_DESCRIPTOR_HEAP_DESC m_Desc;
        UINT m_DescriptorSize;
        UINT m_BitsToSetOnNewHeap = 0;
        UINT m_MaxHeapSize;

        CFencedRingBuffer m_DescriptorRingBuffer;

        CFencePool< unique_comptr<ID3D12DescriptorHeap> > m_HeapPool;

        inline D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle(UINT slot) { 
            assert(slot < m_Desc.NumDescriptors);
            return { m_DescriptorHeapBaseCPU + slot * m_DescriptorSize }; 
        }
        inline D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle(UINT slot) { 
            assert(slot < m_Desc.NumDescriptors);
            return { m_DescriptorHeapBase + slot * m_DescriptorSize }; 
        }
    } m_ViewHeap, m_SamplerHeap;

    void RollOverHeap(OnlineDescriptorHeap& Heap) noexcept(false);
    UINT ReserveSlotsForBindings(OnlineDescriptorHeap& Heap, UINT (ImmediateContext::*pfnCalcRequiredSlots)()) noexcept(false);
    UINT ReserveSlots(OnlineDescriptorHeap& Heap, UINT NumSlots) noexcept(false);

    D3D12_CPU_DESCRIPTOR_HANDLE m_NullUAV;

    // Offline descriptor heaps
    CDescriptorHeapManager m_SRVAllocator;
    CDescriptorHeapManager m_UAVAllocator;
    CDescriptorHeapManager m_SamplerAllocator;

    template <typename TIface> CDescriptorHeapManager& GetViewAllocator();
    template<> CDescriptorHeapManager& GetViewAllocator<ShaderResourceViewType>() { return m_SRVAllocator; }
    template<> CDescriptorHeapManager& GetViewAllocator<UnorderedAccessViewType>() { return m_UAVAllocator; }

    D3D_FEATURE_LEVEL FeatureLevel() const { return m_FeatureLevel; }

    static DXGI_FORMAT GetParentForFormat(DXGI_FORMAT format);

    ResidencyManager &GetResidencyManager() { return m_residencyManager; }
    ResourceStateManager& GetResourceStateManager() { return m_ResourceStateManager; }

private: // variables
    ResourceStateManager m_ResourceStateManager;
    D3D_FEATURE_LEVEL m_FeatureLevel;

    unique_comptr<Resource> m_pStagingTexture;
    unique_comptr<Resource> m_pStagingBuffer;

private: // Dynamic/staging resource pools
    const UINT64 m_BufferPoolTrimThreshold = 100;
    TDynamicBufferPool m_UploadBufferPool;
    TDynamicBufferPool m_ReadbackBufferPool;
    TDynamicBufferPool& GetBufferPool(AllocatorHeapType HeapType)
    {
        switch (HeapType)
        {
        case AllocatorHeapType::Upload:
            return m_UploadBufferPool;
        case AllocatorHeapType::Readback:
            return m_ReadbackBufferPool;
        default:
            assert(false);
        }
        return m_UploadBufferPool;
    }

    // This is the maximum amount of memory the buddy allocator can use. Picking an abritrarily high
    // cap that allows this to pass tests that can potentially spend the whole GPU's memory on
    // suballocated heaps
    static constexpr UINT64 cBuddyMaxBlockSize = 32ll * 1024ll * 1024ll * 1024ll;
    static bool ResourceNeedsOwnAllocation(UINT64 size, bool cannotBeOffset)
    {
        return size > cBuddyAllocatorThreshold || cannotBeOffset;
    }

    // These suballocate out of larger heaps. This should not 
    // be used for resources that require transitions since transitions
    // can only be done on the entire heap, not just the suballocated range
    ConditionalHeapAllocator m_UploadHeapSuballocator;
    ConditionalHeapAllocator m_ReadbackHeapSuballocator;
    ConditionalHeapAllocator& GetAllocator(AllocatorHeapType HeapType)
    {
        switch (HeapType)
        {
        case AllocatorHeapType::Upload:
            return m_UploadHeapSuballocator;
        case AllocatorHeapType::Readback:
            return m_ReadbackHeapSuballocator;
        default:
            assert(false);
        }
        return m_UploadHeapSuballocator;
    }

private:
    D3D12_FEATURE_DATA_D3D12_OPTIONS m_caps;
    const bool m_bUseRingBufferDescriptorHeaps;
};

DEFINE_ENUM_FLAG_OPERATORS(ImmediateContext::UpdateSubresourcesFlags);

} // namespace D3D12TranslationLayer
    
