// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "Resource.hpp"
#include "View.hpp"

namespace D3D12TranslationLayer
{
    void ViewBase::UsedInCommandList(UINT64 id) 
    {
        if (m_pResource) { m_pResource->UsedInCommandList(id); }
    }
};