// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"

namespace D3D12TranslationLayer
{

    PipelineState::PipelineState(ImmediateContext *pContext, const D3D12_SHADER_BYTECODE &CS, RootSignature* pRS)
        : DeviceChildImpl(pContext)
        , m_pRootSignature(pRS)
        , m_Desc { 0, CS }
    {
        m_Desc.pRootSignature = m_pRootSignature->GetForImmediateUse();

        Create();
    }

    PipelineState::~PipelineState() = default;

    void PipelineState::Create()
    {
        HRESULT hr = m_pParent->m_pDevice12->CreateComputePipelineState(&m_Desc, IID_PPV_ARGS(GetForCreate()));
        ThrowFailure(hr); // throw( _com_error )
    }
}