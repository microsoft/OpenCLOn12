// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "D3D12TranslationLayerDependencyIncludes.h"
#include "DeviceChild.hpp"
#include "Resource.hpp"

namespace D3D12TranslationLayer
{
    class TimestampQuery : public DeviceChild
    {
    public:
        TimestampQuery(ImmediateContext* pDevice) noexcept(false);
        ~TimestampQuery() noexcept;

        void End() noexcept;
        UINT64 GetData() noexcept;

    private:
        unique_comptr<ID3D12QueryHeap> m_spQueryHeap;
        D3D12ResourceSuballocation m_spResultBuffer;
    };
};