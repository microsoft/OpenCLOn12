// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"

namespace D3D12TranslationLayer
{

    PipelineState::PipelineState(ImmediateContext *pContext, const COMPUTE_PIPELINE_STATE_DESC &desc)
        : DeviceChildImpl(pContext)
        , m_PipelineStateType(e_Dispatch)
        , m_pRootSignature(pContext->CreateOrRetrieveRootSignature(
            RootSignatureDesc(desc.pCompute)))
    {
        Compute.m_Desc = desc;
        Compute.m_Desc.pRootSignature = m_pRootSignature->GetForImmediateUse();
        Compute.pComputeShader = desc.pCompute;

        Create<e_Dispatch>();
    }

    PipelineState::~PipelineState()
    {
        if (m_pParent->GetPipelineState() == this)
        {
            m_pParent->SetPipelineState(nullptr);
        }
    }

    template<EPipelineType Type> struct PSOTraits;
    template<> struct PSOTraits<e_Dispatch>
    {
        static decltype(&ID3D12Device::CreateComputePipelineState) GetCreate() { return &ID3D12Device::CreateComputePipelineState; }
        static const D3D12_COMPUTE_PIPELINE_STATE_DESC &GetDesc(PipelineState &p) { return p.GetComputeDesc(); }
    };


    template<EPipelineType Type>
    inline void PipelineState::Create()
    {
        CreateImpl<Type>();
    }

    template<EPipelineType Type>
    inline void PipelineState::CreateImpl()
    {
        typedef PSOTraits<Type> PSOTraits;

        HRESULT hr = (m_pParent->m_pDevice12.get()->*PSOTraits::GetCreate())(&PSOTraits::GetDesc(*this), IID_PPV_ARGS(GetForCreate()));
        ThrowFailure(hr); // throw( _com_error )
    }
}