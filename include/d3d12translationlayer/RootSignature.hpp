// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "D3D12TranslationLayerDependencyIncludes.h"
#include "DeviceChild.hpp"
#include <array>

namespace D3D12TranslationLayer
{
    class RootSignature : protected DeviceChildImpl<ID3D12RootSignature>
    {
    public:
        RootSignature(ImmediateContext* pParent)
            : DeviceChildImpl(pParent)
        {
        }

        void Create(D3D12_VERSIONED_ROOT_SIGNATURE_DESC const& rootDesc) noexcept(false);
        void Create(const void* pBlob, SIZE_T BlobSize) noexcept(false);
        using DeviceChildImpl::GetForUse;
        using DeviceChildImpl::GetForImmediateUse;
    };
};
