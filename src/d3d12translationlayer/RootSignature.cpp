// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include <numeric>

namespace D3D12TranslationLayer
{
    void RootSignature::Create(D3D12_VERSIONED_ROOT_SIGNATURE_DESC const& rootDesc) noexcept(false)
    {
        CComPtr<ID3DBlob> spBlob;

        ThrowFailure(D3D12SerializeVersionedRootSignature(&rootDesc, &spBlob, NULL));

        Create(spBlob->GetBufferPointer(), spBlob->GetBufferSize());
    }
    void RootSignature::Create(const void* pBlob, SIZE_T BlobSize) noexcept(false)
    {
        Destroy();

        ThrowFailure(m_pParent->m_pDevice12->CreateRootSignature(1, pBlob, BlobSize, IID_PPV_ARGS(GetForCreate())));
    }
};