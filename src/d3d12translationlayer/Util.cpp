// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "FormatDesc.hpp"
#include "Util.hpp"

namespace D3D12TranslationLayer
{
    UINT GetByteAlignment(DXGI_FORMAT format)
    {
        return CD3D11FormatHelper::GetByteAlignment(format);
    }
}