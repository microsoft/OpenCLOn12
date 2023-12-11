// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
namespace D3D12TranslationLayer
{
    //----------------------------------------------------------------------------------------------------------------------------------
    inline void ImmediateContext::SetShaderResources(UINT StartSlot, __in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) UINT NumSRVs, SRV* const* ppSRVs)
    {
        ImmediateContext::SStageState& CurrentStageState = m_CurrentState.m_CS;

        for (UINT i = 0; i < NumSRVs; ++i)
        {
            UINT slot = i + StartSlot;
            auto pSRV = ppSRVs[i];
            CurrentStageState.m_SRVs.UpdateBinding(slot, pSRV);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    inline void ImmediateContext::SetSamplers(UINT StartSlot, __in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) UINT NumSamplers, Sampler* const* ppSamplers)
    {
        ImmediateContext::SStageState& CurrentStageState = m_CurrentState.m_CS;

        for (UINT i = 0; i < NumSamplers; ++i)
        {
            UINT slot = i + StartSlot;
            auto pSampler = ppSamplers[i];
            CurrentStageState.m_Samplers.UpdateBinding(slot, pSampler);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    inline void ImmediateContext::SetConstantBuffers(UINT StartSlot, __in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_HW_SLOT_COUNT) UINT NumBuffers, Resource* const* ppCBs, __in_ecount_opt(NumBuffers) CONST UINT* pFirstConstant, __in_ecount_opt(NumBuffers) CONST UINT* pNumConstants)
    {
        ImmediateContext::SStageState& CurrentStageState = m_CurrentState.m_CS;

        for (UINT i = 0; i < NumBuffers; ++i)
        {
            UINT slot = i + StartSlot;
            Resource* pCB = ppCBs[i];
            CurrentStageState.m_CBs.UpdateBinding(slot, pCB);

            UINT prevFirstConstant = CurrentStageState.m_uConstantBufferOffsets[slot];
            UINT prevNumConstants = CurrentStageState.m_uConstantBufferCounts[slot];

            UINT newFirstConstant = pFirstConstant ? pFirstConstant[i] : 0;
            UINT newNumConstants = pNumConstants ? pNumConstants[i] : D3D10_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;

            if (prevFirstConstant != newFirstConstant || prevNumConstants != newNumConstants)
            {
                CurrentStageState.m_CBs.SetDirtyBit(slot);
            }

            CurrentStageState.m_uConstantBufferOffsets[slot] = newFirstConstant;
            CurrentStageState.m_uConstantBufferCounts[slot] = newNumConstants;
        }
    }
};