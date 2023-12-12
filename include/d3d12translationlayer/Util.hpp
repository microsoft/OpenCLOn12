// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "D3D12TranslationLayerDependencyIncludes.h"
#include <mutex>
#include <optional>

namespace D3D12TranslationLayer
{
#define ASSUME( _exp ) { assert( _exp ); __analysis_assume( _exp ); __assume( _exp ); }

    class ImmediateContext;
    class Resource;

    enum class AllocatorHeapType
    {
        None,
        Upload,
        Readback,
    };

    inline D3D12_HEAP_TYPE GetD3D12HeapType(AllocatorHeapType HeapType)
    {
        assert(HeapType != AllocatorHeapType::None);
        switch (HeapType)
        {
        case AllocatorHeapType::Readback:
            return D3D12_HEAP_TYPE_READBACK;

        case AllocatorHeapType::Upload:
        default:
            return D3D12_HEAP_TYPE_UPLOAD;
        }
    }

    //
    // Converts an HRESULT to an exception.  This matches ThrowFailure used
    // elsewhere in dxg.
    //
    _At_(return, _When_(FAILED(hr), __analysis_noreturn))
    inline void ThrowFailure(HRESULT hr)
    {
        if (FAILED(hr))
        {
            throw _com_error(hr);
        }
    }

    _At_(return, _When_(h == nullptr, __analysis_noreturn))
    inline void ThrowIfHandleNull(HANDLE h)
    {
        if (h == nullptr)
        {
            throw _com_error(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    class SafeHANDLE
    {
    public:
        SafeHANDLE() : m_h(NULL)
        { }
        ~SafeHANDLE()
        {
            if (m_h) CloseHandle(m_h);
        }
        operator HANDLE() const
        {
            return m_h;
        }
        HANDLE release()
        {
            HANDLE h = m_h; m_h = NULL; return h;
        }

        HANDLE m_h;
    };

    class ThrowingSafeHandle : public SafeHANDLE
    {
    public:
        ThrowingSafeHandle(HANDLE h) noexcept(false)
        {
            if (h == NULL)
            {
                ThrowFailure(E_OUTOFMEMORY);
            }
            m_h = h;
        }
    };

    inline void* AlignedHeapAlloc16(size_t size) noexcept
    {
#ifdef _WIN64  
        return HeapAlloc(GetProcessHeap(), 0, size);
#else  
        size_t totalSize = size + 16;
        void* original = HeapAlloc(GetProcessHeap(), 0, totalSize);
        if (original == NULL)
        {
            return NULL;
        }
        const size_t alignedPtr = (reinterpret_cast<size_t>(original) + 16) & ~(15);
        assert(alignedPtr > reinterpret_cast<size_t>(original));
        size_t offset = alignedPtr - reinterpret_cast<size_t>(original);
        *(reinterpret_cast<char*>(alignedPtr) - 1) = static_cast<char>(offset);
        return reinterpret_cast<void*>(alignedPtr);
#endif  
    }

    //HeapAlloc is guaranteed to be 8-byte aligned on x86, and 16-byte aligned on x64  
    //From windows\directx\dxg\d3d11\D3DCore\Inc\SWCommandList.hpp  
    inline void AlignedHeapFree16(void* p) noexcept
    {
        if (p == NULL)
            return;
#ifdef _WIN64  
        HeapFree(GetProcessHeap(), 0, p);
#else  
        char* pChar = reinterpret_cast<char*>(p);
        size_t offset = *(pChar - 1);
        HeapFree(GetProcessHeap(), 0, pChar - offset);
#endif  
    }


    template< typename T >
    inline T Align(T uValue, T uAlign)
    {
        T uResult;

        if (IsPow2(uAlign))
        {
            T uMask = uAlign - 1;
            uResult = (uValue + uMask) & ~uMask;
        }
        else
        {
            uResult = ((uValue + uAlign - 1) / uAlign) * uAlign;
        }

        assert(uResult >= uValue);
        assert(0 == (uResult % uAlign));
        return uResult;
    }

    template< typename T >
    inline T AlignAtLeast(T uValue, T uAlign)
    {
        return std::max<T>(Align(uValue, uAlign), uAlign);
    }

    // Avoid including kernel libraries by adding list implementation here:

    inline BOOLEAN IsListEmpty(_In_ const LIST_ENTRY * ListHead)
    {
        return (BOOLEAN)(ListHead->Flink == ListHead);
    }

    inline void InitializeListHead(_Out_ PLIST_ENTRY ListHead)
    {
        ListHead->Flink = ListHead->Blink = ListHead;
    }

    inline BOOLEAN RemoveEntryList(_In_ PLIST_ENTRY Entry)
    {
        PLIST_ENTRY PrevEntry;
        PLIST_ENTRY NextEntry;

        NextEntry = Entry->Flink;
        PrevEntry = Entry->Blink;
        if ((NextEntry->Blink != Entry) || (PrevEntry->Flink != Entry))
        {
            assert(false);
        }

        PrevEntry->Flink = NextEntry;
        NextEntry->Blink = PrevEntry;
        return (BOOLEAN)(PrevEntry == NextEntry);
    }

    inline void InsertHeadList(_Inout_ PLIST_ENTRY ListHead, _Out_ PLIST_ENTRY Entry)
    {
        PLIST_ENTRY NextEntry;

        NextEntry = ListHead->Flink;
        Entry->Flink = NextEntry;
        Entry->Blink = ListHead;
        if (NextEntry->Blink != ListHead)
        {
            assert(false);
        }

        NextEntry->Blink = Entry;
        ListHead->Flink = Entry;
        return;
    }

    inline void InsertTailList(_Inout_ PLIST_ENTRY ListHead, _Out_ __drv_aliasesMem PLIST_ENTRY Entry)
    {
        PLIST_ENTRY PrevEntry;

        PrevEntry = ListHead->Blink;
        Entry->Flink = ListHead;
        Entry->Blink = PrevEntry;
        if (PrevEntry->Flink != ListHead)
        {
            assert(false);
        }

        PrevEntry->Flink = Entry;
        ListHead->Blink = Entry;
        return;
    }

    //==================================================================================================================================
    //
    // unique_comptr, like unique_ptr except for Ref-held
    //
    //==================================================================================================================================

    struct unique_comptr_deleter
    {
        template<typename T>
        void operator()(T *pUC) const
        {
            pUC->Release();
        }
    };

    template<typename T, class Deleter = unique_comptr_deleter>
    struct unique_comptr : protected std::unique_ptr<T, Deleter>
    {
        static_assert(std::is_empty<Deleter>::value, "unique_comptr doesn't support stateful deleter.");
        typedef std::unique_ptr<T, Deleter> parent_t;
        using pointer = typename parent_t::pointer;

        unique_comptr()
            : parent_t(nullptr)
        {
        }

        explicit unique_comptr(T *p)
            : parent_t(p)
        {
            if (p)
            {
                p->AddRef();
            }
        }

        template <typename Del2>
        unique_comptr(unique_comptr<T, Del2> && other)
            : parent_t(other.release())
        {
        }

        template <typename Del2>
        unique_comptr& operator=(unique_comptr<T, Del2> && other)
        {
            parent_t::reset(other.release());
            return *this;
        }

        unique_comptr& operator=(pointer p)
        {
            reset(p);
            return *this;
        }

        unique_comptr& operator=(std::nullptr_t p)
        {
            reset(p);
            return *this;
        }

        void reset(pointer p = pointer())
        {
            if (p)
            {
                p->AddRef();
            }
            parent_t::reset(p);
        }

        void reset(std::nullptr_t p)
        {
            parent_t::reset(p);
        }

        T** operator&()
        {
            assert(*this == nullptr);
            return reinterpret_cast<T**>(this);
        }

        T*const* operator&() const
        {
            return reinterpret_cast<T*const*>(this);
        }

        using parent_t::release;
        using parent_t::get;
        using parent_t::operator->;
        using parent_t::operator*;
        using parent_t::operator bool;

    private:
        unique_comptr& operator=(unique_comptr const&) = delete;
        unique_comptr(unique_comptr const&) = delete;
    };

    template <typename T>
    struct PreallocatedArray
    {
        T* const m_pBegin;
        T* m_pEnd;

        template<typename... TConstructionArgs>
        PreallocatedArray(UINT ArraySize, void*& Address, TConstructionArgs&&... constructionArgs)
            : m_pBegin(reinterpret_cast<T*>(Address))
            , m_pEnd(m_pBegin + ArraySize)
        {
            // Leave uninitialized otherwise
            if (!std::is_trivially_constructible<T>::value)
            {
                for (T& t : *this)
                {
                    new (std::addressof(t)) T(std::forward<TConstructionArgs>(constructionArgs)...);
                }
            }
            Address = m_pEnd;
        }
        ~PreallocatedArray()
        {
            clear();
        }
        PreallocatedArray(PreallocatedArray const&) = delete;
        PreallocatedArray& operator=(PreallocatedArray const&) = delete;

        void clear()
        {
            if (!std::is_trivially_destructible<T>::value)
            {
                for (T& t : *this)
                {
                    t.~T();
                }
            }
            m_pEnd = m_pBegin;
        }

        size_t size() const { return std::distance(m_pBegin, m_pEnd); }
        bool empty() const { return m_pBegin == m_pEnd; }

        T* begin() { return m_pBegin; }
        T const* begin() const { return m_pBegin; }

        T* end() { return m_pEnd; }
        T const* end() const { return m_pEnd; }

        T& operator[](UINT i) { assert(m_pBegin + i < m_pEnd); return m_pBegin[i]; }
        T const& operator[](UINT i) const { assert(m_pBegin + i < m_pEnd); return m_pBegin[i]; }
    };

    template <typename T, size_t InlineSize>
    struct PreallocatedInlineArray
    {
        T m_InlineArray[InlineSize];
        PreallocatedArray<T> m_Extra;
        UINT m_Size;

        template<typename... TConstructionArgs>
        PreallocatedInlineArray(UINT ArraySize, void*& Address, TConstructionArgs&&... constructionArgs)
            : m_Extra(ArraySize > InlineSize ? ArraySize - InlineSize : 0, Address, std::forward<TConstructionArgs>(constructionArgs)...)
            , m_Size(ArraySize)
        {
            // Leave uninitialized otherwise
            if constexpr (!std::is_trivially_constructible<T>::value)
            {
                for (UINT i = 0; i < m_Size && i < InlineSize; ++i)
                {
                    new (std::addressof(m_InlineArray[i])) T(std::forward<TConstructionArgs>(constructionArgs)...);
                }
            }
        }
        ~PreallocatedInlineArray()
        {
            clearInline();
        }
        PreallocatedInlineArray(PreallocatedInlineArray const&) = delete;
        PreallocatedInlineArray& operator=(PreallocatedInlineArray const&) = delete;

        void clearInline()
        {
            if constexpr (!std::is_trivially_destructible<T>::value)
            {
                for (UINT i = 0; i < m_Size && i < InlineSize; ++i)
                {
                    m_InlineArray[i].~T();
                }
            }
        }
        void clear()
        {
            clearInline();
            m_Extra.clear();
            m_Size = 0;
        }

        size_t size() const { return m_Size; }
        bool empty() const { return m_Size == 0; }

        T &operator[](UINT i) { assert(i < m_Size); return i < InlineSize ? m_InlineArray[i] : m_Extra[i - InlineSize]; }
        T const& operator[](UINT i) const { assert(i < m_Size); return i < InlineSize ? m_InlineArray[i] : m_Extra[i - InlineSize]; }
    };

    enum class ResourceAllocationContext
    {
        ImmediateContextThreadLongLived,
        ImmediateContextThreadTemporary,
        FreeThread,
    };

    UINT GetByteAlignment(DXGI_FORMAT format);

    inline D3D12_RESOURCE_STATES GetDefaultPoolState(AllocatorHeapType heapType)
    {
        switch (heapType)
        {
        case AllocatorHeapType::Upload:
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case AllocatorHeapType::Readback:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        default:
            return D3D12_RESOURCE_STATE_COMMON;
        }
    }

    inline D3D_FEATURE_LEVEL GetHardwareFeatureLevel(ID3D12Device *pDevice)
    {
        static const D3D_FEATURE_LEVEL RequestedD3D12FeatureLevels[] =
        {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_1_0_CORE,
        };

        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = { ARRAYSIZE(RequestedD3D12FeatureLevels), RequestedD3D12FeatureLevels };
        ThrowFailure(pDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels)));
        return featureLevels.MaxSupportedFeatureLevel;
    }

    template<typename T>
    inline void SetFeatureDataNodeIndex(void *pFeatureSupportData, UINT FeatureSupportDataSize, UINT NodeIndex)
    {
        if (FeatureSupportDataSize != sizeof(T))
        {
            ThrowFailure(E_INVALIDARG);
        }
        T *pSupportData = (T*)pFeatureSupportData;
        pSupportData->NodeIndex = NodeIndex;
    }

    template <typename F>
    struct ScopeExit {
        ScopeExit(F &&f) : f(std::forward<F>(f)) {}
        ~ScopeExit() { f(); }
        F f;
    };

    template <typename F>
    inline ScopeExit<F> MakeScopeExit(F &&f) {
        return ScopeExit<F>(std::forward<F>(f));
    };

    template <typename T>
    inline void hash_combine(size_t & seed, const T & v)
    {
        std::hash<T> hasher;
        seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

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
};