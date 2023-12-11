// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

namespace D3D12TranslationLayer
{
    class CommandListManager;

// These are defined in the private d3d12 header
#define UNKNOWN_RESOURCE_STATE (D3D12_RESOURCE_STATES)0x8000u
#define RESOURCE_STATE_VALID_BITS 0x2f3fff
#define RESOURCE_STATE_VALID_INTERNAL_BITS 0x2fffff
constexpr D3D12_RESOURCE_STATES RESOURCE_STATE_ALL_WRITE_BITS =
    D3D12_RESOURCE_STATE_RENDER_TARGET          |
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS       |
    D3D12_RESOURCE_STATE_DEPTH_WRITE            |
    D3D12_RESOURCE_STATE_STREAM_OUT             |
    D3D12_RESOURCE_STATE_COPY_DEST              |
    D3D12_RESOURCE_STATE_RESOLVE_DEST           |
    D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE     |
    D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;

    enum class SubresourceTransitionFlags
    {
        None = 0,
        StateMatchExact = 1,
        ForceWriteState = 2,
        NotUsedInCommandListIfNoStateChange = 4,
    };
    DEFINE_ENUM_FLAG_OPERATORS(SubresourceTransitionFlags);

    inline bool IsD3D12WriteState(UINT State, SubresourceTransitionFlags Flags)
    {
        return (State & RESOURCE_STATE_ALL_WRITE_BITS) != 0 ||
            (Flags & SubresourceTransitionFlags::ForceWriteState) != SubresourceTransitionFlags::None;
    }

    //==================================================================================================================================
    // CDesiredResourceState
    // Stores the current desired state of either an entire resource, or each subresource.
    //==================================================================================================================================
    class CDesiredResourceState
    {
    public:
        struct SubresourceInfo
        {
            D3D12_RESOURCE_STATES State = UNKNOWN_RESOURCE_STATE;
            SubresourceTransitionFlags Flags = SubresourceTransitionFlags::None;
        };

    private:
        bool m_bAllSubresourcesSame = true;

        PreallocatedInlineArray<SubresourceInfo, 1> m_spSubresourceInfo;

    public:
        static size_t CalcPreallocationSize(UINT SubresourceCount) { return sizeof(SubresourceInfo) * (SubresourceCount - 1); }
        CDesiredResourceState(UINT SubresourceCount, void*& pPreallocatedMemory) noexcept
            : m_spSubresourceInfo(SubresourceCount, pPreallocatedMemory) // throw( bad_alloc )
        {
        }

        bool AreAllSubresourcesSame() const noexcept { return m_bAllSubresourcesSame; }

        SubresourceInfo const& GetSubresourceInfo(UINT SubresourceIndex) const noexcept;
        void SetResourceState(SubresourceInfo const& Info) noexcept;
        void SetSubresourceState(UINT SubresourceIndex, SubresourceInfo const& Info) noexcept;

        void Reset() noexcept;
    };

    //==================================================================================================================================
    // CCurrentResourceState
    // Stores the current state of either an entire resource, or each subresource.
    // Current state can either be shared read across multiple queues, or exclusive on a single queue.
    //==================================================================================================================================
    class CCurrentResourceState
    {
    public:
        struct SubresourceState
        {
            UINT64 WriteFenceValue = 0;
            UINT64 ReadFenceValue = 0;
            D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
        };

    private:
        const bool m_bSimultaneousAccess;
        bool m_bAllSubresourcesSame = true;

        PreallocatedInlineArray<SubresourceState, 1> m_spSubresourceState;

        void ConvertToSubresourceTracking() noexcept;

    public:
        static size_t CalcPreallocationSize(UINT SubresourceCount)
        {
            return sizeof(SubresourceState) * (SubresourceCount - 1);
        }
        CCurrentResourceState(UINT SubresourceCount, bool bSimultaneousAccess, void*& pPreallocatedMemory) noexcept;

        bool SupportsSimultaneousAccess() const noexcept { return m_bSimultaneousAccess; }
        bool AreAllSubresourcesSame() const noexcept { return m_bAllSubresourcesSame; }

        void SetResourceState(SubresourceState const& State) noexcept;
        void SetSubresourceState(UINT SubresourceIndex, SubresourceState const& State) noexcept;
        SubresourceState const& GetSubresourceState(UINT SubresourceIndex) const noexcept;

        void Reset() noexcept;
    };
    
    //==================================================================================================================================
    // TransitionableResourceBase
    // A base class that transitionable resources should inherit from.
    //==================================================================================================================================
    struct TransitionableResourceBase
    {
        LIST_ENTRY m_TransitionListEntry;
        CDesiredResourceState m_DesiredState;

        static size_t CalcPreallocationSize(UINT NumSubresources) { return CDesiredResourceState::CalcPreallocationSize(NumSubresources); }
        TransitionableResourceBase(UINT NumSubresources, void*& pPreallocatedMemory) noexcept
            : m_DesiredState(NumSubresources, pPreallocatedMemory)
        {
            D3D12TranslationLayer::InitializeListHead(&m_TransitionListEntry);
        }
        ~TransitionableResourceBase() noexcept
        {
            if (IsTransitionPending())
            {
                D3D12TranslationLayer::RemoveEntryList(&m_TransitionListEntry);
            }
        }
        bool IsTransitionPending() const noexcept { return !D3D12TranslationLayer::IsListEmpty(&m_TransitionListEntry); }
    };

    //==================================================================================================================================
    // ResourceStateManager
    // The implementation of state management tailored to the ImmediateContext and Resource classes.
    //==================================================================================================================================
    class ResourceStateManager
    {
    private:
        ImmediateContext& m_ImmCtx;

    public:
        ResourceStateManager(ImmediateContext &ImmCtx) noexcept(false);

        ~ResourceStateManager() noexcept
        {
            // All resources should be gone by this point, and each resource ensures it is no longer in this list.
            assert(D3D12TranslationLayer::IsListEmpty(&m_TransitionListHead));
        }

        // Transition the entire resource to a particular destination state on a particular command list.
        void TransitionResource(Resource* pResource,
                                D3D12_RESOURCE_STATES State,
                                SubresourceTransitionFlags Flags = SubresourceTransitionFlags::None) noexcept;
        // Transition a set of subresources to a particular destination state. Fast-path provided when subset covers entire resource.
        void TransitionSubresources(Resource* pResource,
                                    CViewSubresourceSubset const& Subresources,
                                    D3D12_RESOURCE_STATES State,
                                    SubresourceTransitionFlags Flags = SubresourceTransitionFlags::None) noexcept;
        // Transition a single subresource to a particular destination state.
        void TransitionSubresource(Resource* pResource,
                                   UINT SubresourceIndex,
                                   D3D12_RESOURCE_STATES State,
                                   SubresourceTransitionFlags Flags = SubresourceTransitionFlags::None) noexcept;

        // Submit all barriers
        void ApplyAllResourceTransitions() noexcept(false);

    private:
        LIST_ENTRY m_TransitionListHead;
        std::vector<D3D12_RESOURCE_BARRIER> m_vResourceBarriers;

        // These methods set the destination state of the resource/subresources and ensure it's in the transition list.
        void TransitionResource(TransitionableResourceBase &Resource,
                                CDesiredResourceState::SubresourceInfo const &State) noexcept;
        void TransitionSubresources(TransitionableResourceBase &Resource,
                                    CViewSubresourceSubset const &Subresources,
                                    CDesiredResourceState::SubresourceInfo const &State) noexcept;
        void TransitionSubresource(TransitionableResourceBase &Resource,
                                   UINT SubresourceIndex,
                                   CDesiredResourceState::SubresourceInfo const &State) noexcept;

        // Clear out any state from previous iterations.
        void ApplyResourceTransitionsPreamble() noexcept;

        // For every entry in the transition list, call a routine.
        // This routine must return a TransitionResult which indicates what to do with the list.
        template <typename TFunc>
        void ForEachTransitioningResource(TFunc &&func)
            noexcept(noexcept(func(std::declval<TransitionableResourceBase &>())))
        {
            for (LIST_ENTRY *pListEntry = m_TransitionListHead.Flink; pListEntry != &m_TransitionListHead;)
            {
                TransitionableResourceBase *pResource = CONTAINING_RECORD(pListEntry, TransitionableResourceBase, m_TransitionListEntry);
                func(*pResource);

                auto pNextListEntry = pListEntry->Flink;
                D3D12TranslationLayer::RemoveEntryList(pListEntry);
                D3D12TranslationLayer::InitializeListHead(pListEntry);
                pListEntry = pNextListEntry;
            }
        }

        // Updates vectors with the operations that should be applied to the requested resource.
        // May update the destination state of the resource.
        void ProcessTransitioningResource(ID3D12Resource *pTransitioningResource,
                                          TransitionableResourceBase &TransitionableResource,
                                          CCurrentResourceState &CurrentState,
                                          UINT NumTotalSubresources,
                                          UINT64 CurrentFenceValues) noexcept(false);

        void SubmitResourceTransitions(CommandListManager *pManager) noexcept(false);

    private:
        // Helpers
        static bool TransitionRequired(D3D12_RESOURCE_STATES CurrentState, D3D12_RESOURCE_STATES &DestinationState, SubresourceTransitionFlags Flags) noexcept;
        void SubmitResourceBarriers(_In_reads_(Count) D3D12_RESOURCE_BARRIER const *pBarriers, UINT Count, _In_ CommandListManager *pManager) noexcept;
    };
};