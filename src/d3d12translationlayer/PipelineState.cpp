// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "ImmediateContext.hpp"
#include "RootSignature.hpp"
#include "PipelineState.hpp"

namespace D3D12TranslationLayer
{

    PipelineState::PipelineState(ImmediateContext *pContext, const D3D12_SHADER_BYTECODE &CS, RootSignature* pRS, D3D12_CACHED_PIPELINE_STATE Cached)
        : DeviceChildImpl(pContext)
        , m_pRootSignature(pRS)
        , m_Desc { nullptr, CS, 0, Cached }
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