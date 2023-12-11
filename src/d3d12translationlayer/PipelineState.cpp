// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"

namespace D3D12TranslationLayer
{

    PipelineState::PipelineState(ImmediateContext *pContext, const COMPUTE_PIPELINE_STATE_DESC &desc)
        : DeviceChildImpl(pContext)
        , m_pRootSignature(pContext->CreateOrRetrieveRootSignature(
            RootSignatureDesc(desc.pCompute)))
    {
        Compute.m_Desc = desc;
        Compute.m_Desc.pRootSignature = m_pRootSignature->GetForImmediateUse();
        Compute.pComputeShader = desc.pCompute;

        Create();
    }

    PipelineState::~PipelineState()
    {
        if (m_pParent->GetPipelineState() == this)
        {
            m_pParent->SetPipelineState(nullptr);
        }
    }

    void PipelineState::Create()
    {
        HRESULT hr = m_pParent->m_pDevice12->CreateComputePipelineState(&GetComputeDesc(), IID_PPV_ARGS(GetForCreate()));
        ThrowFailure(hr); // throw( _com_error )
    }
}