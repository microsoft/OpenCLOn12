// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "D3D12TranslationLayerDependencyIncludes.h"
#include "DeviceChild.hpp"

namespace D3D12TranslationLayer
{
    class RootSignature;

    struct PipelineState : protected DeviceChildImpl<ID3D12PipelineState>
    {
    public:
        RootSignature* GetRootSignature() { return m_pRootSignature; }

        PipelineState(ImmediateContext *pContext, const D3D12_SHADER_BYTECODE &CS, RootSignature *pRS, D3D12_CACHED_PIPELINE_STATE Cached = {});
        ~PipelineState();

        using DeviceChildImpl::GetForUse;
        using DeviceChildImpl::GetForImmediateUse;

    protected:
        RootSignature* const m_pRootSignature;
        D3D12_COMPUTE_PIPELINE_STATE_DESC m_Desc;

        void Create();
    };
};
