// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "ImmediateContext.hpp"
#include "ImmediateContext.inl"
#include "ResourceState.hpp"

namespace D3D12TranslationLayer
{
    //----------------------------------------------------------------------------------------------------------------------------------
    auto CDesiredResourceState::GetSubresourceInfo(UINT SubresourceIndex) const noexcept -> SubresourceInfo const&
    {
        if (AreAllSubresourcesSame())
        {
            SubresourceIndex = 0;
        }
        return m_spSubresourceInfo[SubresourceIndex];
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CDesiredResourceState::SetResourceState(SubresourceInfo const & Info) noexcept
    {
        m_bAllSubresourcesSame = true;
        m_spSubresourceInfo[0] = Info;
    }
    
    //----------------------------------------------------------------------------------------------------------------------------------
    void CDesiredResourceState::SetSubresourceState(UINT SubresourceIndex, SubresourceInfo const & Info) noexcept
    {
        if (m_bAllSubresourcesSame && m_spSubresourceInfo.size() > 1)
        {
            static_assert(std::extent_v<decltype(m_spSubresourceInfo.m_InlineArray)> == 1, "Otherwise this fill doesn't work");
            std::fill(m_spSubresourceInfo.m_Extra.begin(), m_spSubresourceInfo.m_Extra.end(), m_spSubresourceInfo[0]);
            m_bAllSubresourcesSame = false;
        }
        if (m_spSubresourceInfo.size() == 1)
        {
            SubresourceIndex = 0;
        }
        m_spSubresourceInfo[SubresourceIndex] = Info;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CDesiredResourceState::Reset() noexcept
    {
        SetResourceState(SubresourceInfo{});
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CCurrentResourceState::ConvertToSubresourceTracking() noexcept
    {
        if (m_bAllSubresourcesSame && m_spSubresourceState.size() > 1)
        {
            static_assert(std::extent_v<decltype(m_spSubresourceState.m_InlineArray)> == 1, "Otherwise this fill doesn't work");
            std::fill(m_spSubresourceState.m_Extra.begin(), m_spSubresourceState.m_Extra.end(), m_spSubresourceState[0]);
            m_bAllSubresourcesSame = false;
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CCurrentResourceState::CCurrentResourceState(UINT SubresourceCount, bool bSimultaneousAccess, void*& pPreallocatedMemory) noexcept
        : m_bSimultaneousAccess(bSimultaneousAccess)
        , m_spSubresourceState(SubresourceCount, pPreallocatedMemory)
    {
        m_spSubresourceState[0] = SubresourceState{};
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CCurrentResourceState::SetResourceState(SubresourceState const& State) noexcept
    {
        m_bAllSubresourcesSame = true;
        m_spSubresourceState[0] = State;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CCurrentResourceState::SetSubresourceState(UINT SubresourceIndex, SubresourceState const& State) noexcept
    {
        ConvertToSubresourceTracking();
        m_spSubresourceState[SubresourceIndex] = State;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    auto CCurrentResourceState::GetSubresourceState(UINT SubresourceIndex) const noexcept -> SubresourceState const&
    {
        if (AreAllSubresourcesSame())
        {
            SubresourceIndex = 0;
        }
        return m_spSubresourceState[SubresourceIndex];
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void CCurrentResourceState::Reset() noexcept
    {
        m_bAllSubresourcesSame = true;
        m_spSubresourceState[0] = SubresourceState{};
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    ResourceStateManager::ResourceStateManager(ImmediateContext& ImmCtx) noexcept(false)
        : m_ImmCtx(ImmCtx)
    {
        D3D12TranslationLayer::InitializeListHead(&m_TransitionListHead);
        // Reserve some space in these vectors upfront. Values are arbitrary.
        m_vResourceBarriers.reserve(50);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::TransitionResource(TransitionableResourceBase& Resource,
                                                      CDesiredResourceState::SubresourceInfo const& State) noexcept
    {
        Resource.m_DesiredState.SetResourceState(State);
        if (!Resource.IsTransitionPending())
        {
            InsertHeadList(&m_TransitionListHead, &Resource.m_TransitionListEntry);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::TransitionSubresources(TransitionableResourceBase& Resource,
                                                          CViewSubresourceSubset const& Subresources,
                                                          CDesiredResourceState::SubresourceInfo const& State) noexcept
    {
        if (Subresources.IsWholeResource())
        {
            Resource.m_DesiredState.SetResourceState(State);
        }
        else
        {
            for (auto&& range : Subresources)
            {
                for (UINT i = range.first; i < range.second; ++i)
                {
                    Resource.m_DesiredState.SetSubresourceState(i, State);
                }
            }
        }
        if (!Resource.IsTransitionPending())
        {
            InsertHeadList(&m_TransitionListHead, &Resource.m_TransitionListEntry);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::TransitionSubresource(TransitionableResourceBase& Resource,
                                                         UINT SubresourceIndex,
                                                         CDesiredResourceState::SubresourceInfo const& State) noexcept
    {
        Resource.m_DesiredState.SetSubresourceState(SubresourceIndex, State);
        if (!Resource.IsTransitionPending())
        {
            InsertHeadList(&m_TransitionListHead, &Resource.m_TransitionListEntry);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::ApplyResourceTransitionsPreamble() noexcept
    {
        m_vResourceBarriers.clear();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    /*static*/ bool ResourceStateManager::TransitionRequired(D3D12_RESOURCE_STATES CurrentState, D3D12_RESOURCE_STATES& DestinationState, SubresourceTransitionFlags Flags) noexcept
    {
        // An exact match never needs a transition.
        if (CurrentState == DestinationState)
        {
            return false;
        }

        // Not an exact match, but an exact match required, so do the transition.
        if ((Flags & SubresourceTransitionFlags::StateMatchExact) != SubresourceTransitionFlags::None)
        {
            return true;
        }

        // Current state already contains the destination state, we're good.
        if ((CurrentState & DestinationState) == DestinationState)
        {
            DestinationState = CurrentState;
            return false;
        }

        // If the transition involves a write state, then the destination should just be the requested destination.
        // Otherwise, accumulate read states to minimize future transitions (by triggering the above condition).
        if (!IsD3D12WriteState(DestinationState, SubresourceTransitionFlags::None) &&
            !IsD3D12WriteState(CurrentState, SubresourceTransitionFlags::None))
        {
            DestinationState |= CurrentState;
        }
        return true;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::ProcessTransitioningResource(ID3D12Resource* pTransitioningResource,
                                                                TransitionableResourceBase& TransitionableResource,
                                                                CCurrentResourceState& CurrentState,
                                                                UINT NumTotalSubresources,
                                                                UINT64 CurrentFenceValue) noexcept(false)
    {
        // Figure out the set of subresources that are transitioning
        auto& DestinationState = TransitionableResource.m_DesiredState;
        bool bAllSubresourcesAtOnce = CurrentState.AreAllSubresourcesSame() && DestinationState.AreAllSubresourcesSame();

        D3D12_RESOURCE_BARRIER TransitionDesc;
        ZeroMemory(&TransitionDesc, sizeof(TransitionDesc));
        TransitionDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        TransitionDesc.Transition.pResource = pTransitioningResource;

        UINT numSubresources = bAllSubresourcesAtOnce ? 1 : NumTotalSubresources;
        for (UINT i = 0; i < numSubresources; ++i)
        {
            CDesiredResourceState::SubresourceInfo SubresourceDestinationInfo = DestinationState.GetSubresourceInfo(i);
            TransitionDesc.Transition.Subresource = bAllSubresourcesAtOnce ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : i;

            // Is this subresource currently being used, or is it just being iterated over?
            D3D12_RESOURCE_STATES after = SubresourceDestinationInfo.State;
            SubresourceTransitionFlags Flags = SubresourceDestinationInfo.Flags;
            if (after == UNKNOWN_RESOURCE_STATE)
            {
                // This subresource doesn't have any transition requested - move on to the next.
                continue;
            }

#if DBG
            // This subresource should not already be in any transition list
            for (auto &desc : m_vResourceBarriers)
            {
                assert(!(desc.Transition.pResource == pTransitioningResource &&
                         desc.Transition.Subresource == TransitionDesc.Transition.Subresource));
            }
#endif

            CCurrentResourceState::SubresourceState CurrentSubresourceState = CurrentState.GetSubresourceState(i);

            bool bUsedInCommandList = (SubresourceDestinationInfo.Flags & SubresourceTransitionFlags::NotUsedInCommandListIfNoStateChange) == SubresourceTransitionFlags::None;

            if (!CurrentState.SupportsSimultaneousAccess() ||
                CurrentFenceValue == CurrentSubresourceState.WriteFenceValue)
            {
                if (TransitionRequired(CurrentSubresourceState.State, /*inout*/ after, SubresourceDestinationInfo.Flags))
                {
                    TransitionDesc.Transition.StateBefore = D3D12_RESOURCE_STATES(CurrentSubresourceState.State);
                    TransitionDesc.Transition.StateAfter = D3D12_RESOURCE_STATES(after);
                    assert(TransitionDesc.Transition.StateBefore != TransitionDesc.Transition.StateAfter);
                    m_vResourceBarriers.push_back(TransitionDesc); // throw( bad_alloc )

                    bUsedInCommandList = true;
                }
            }
            else if (CurrentState.SupportsSimultaneousAccess())
            {
                assert(CurrentFenceValue != CurrentSubresourceState.WriteFenceValue);
                if (CurrentFenceValue == CurrentSubresourceState.ReadFenceValue)
                {
                    after |= CurrentSubresourceState.State;
                }
            }

            if (bUsedInCommandList)
            {
                CCurrentResourceState::SubresourceState NewState = CurrentSubresourceState;
                NewState.State = after;
                NewState.ReadFenceValue = CurrentFenceValue;
                if (IsD3D12WriteState(after, Flags))
                {
                    NewState.WriteFenceValue = CurrentFenceValue;
                }
                if (bAllSubresourcesAtOnce)
                {
                    CurrentState.SetResourceState(NewState);
                }
                else
                {
                    CurrentState.SetSubresourceState(i, NewState);
                }
                // TODO: Fix
                static_cast<Resource &>(TransitionableResource).UsedInCommandList(CurrentFenceValue);
            }
        }

        CDesiredResourceState::SubresourceInfo UnknownDestinationState = {};
        UnknownDestinationState.State = UNKNOWN_RESOURCE_STATE;
        UnknownDestinationState.Flags = SubresourceTransitionFlags::None;

        // Update destination states.
        // Coalesce destination state to ensure that it's set for the entire resource.
        DestinationState.SetResourceState(UnknownDestinationState);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::SubmitResourceBarriers(_In_reads_(Count) D3D12_RESOURCE_BARRIER const * pBarriers, UINT Count, _In_ CommandListManager * pManager) noexcept
    {
        pManager->GetGraphicsCommandList()->ResourceBarrier(Count, pBarriers);
        pManager->AdditionalCommandsAdded();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::SubmitResourceTransitions(CommandListManager *pManager) noexcept(false)
    {
        if (!m_vResourceBarriers.empty())
        {
            SubmitResourceBarriers(m_vResourceBarriers.data(), (UINT)m_vResourceBarriers.size(), pManager);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::TransitionResource(Resource* pResource, D3D12_RESOURCE_STATES State, SubresourceTransitionFlags Flags) noexcept
    {
        CDesiredResourceState::SubresourceInfo DesiredState = { State, Flags };
        ResourceStateManager::TransitionResource(*pResource, DesiredState);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::TransitionSubresources(Resource* pResource, CViewSubresourceSubset const & Subresources, D3D12_RESOURCE_STATES State, SubresourceTransitionFlags Flags) noexcept
    {
        CDesiredResourceState::SubresourceInfo DesiredState = { State, Flags };
        ResourceStateManager::TransitionSubresources(*pResource, Subresources, DesiredState);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::TransitionSubresource(Resource* pResource, UINT SubresourceIndex, D3D12_RESOURCE_STATES State, SubresourceTransitionFlags Flags) noexcept
    {
        CDesiredResourceState::SubresourceInfo DesiredState = { State, Flags };
        ResourceStateManager::TransitionSubresource(*pResource, SubresourceIndex, DesiredState);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    void ResourceStateManager::ApplyAllResourceTransitions() noexcept(false)
    {
        CommandListManager *pCommandListManager = m_ImmCtx.GetCommandListManager();
        UINT64 CurrentFenceValue = m_ImmCtx.GetCommandListID();

        ApplyResourceTransitionsPreamble();
        ForEachTransitioningResource([=](TransitionableResourceBase& ResourceBase)
        {
            Resource& CurResource = static_cast<Resource&>(ResourceBase);

            ProcessTransitioningResource(
                CurResource.GetUnderlyingResource(),
                CurResource,
                CurResource.GetIdentity()->m_currentState,
                CurResource.NumSubresources(),
                CurrentFenceValue); // throw( bad_alloc )
        });

        SubmitResourceTransitions(pCommandListManager); // throw( bad_alloc )
    }
};

