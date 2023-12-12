// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "ImmediateContext.hpp"
#include "Sampler.hpp"

namespace D3D12TranslationLayer
{

    //----------------------------------------------------------------------------------------------------------------------------------
    Sampler::Sampler(ImmediateContext* pDevice, D3D12_SAMPLER_DESC const& desc) noexcept(false)
        : DeviceChild(pDevice)
    {
        if (!pDevice->ComputeOnly())
        {
            m_Descriptor = pDevice->m_SamplerAllocator.AllocateHeapSlot(&m_DescriptorHeapIndex); // throw( _com_error )
            pDevice->m_pDevice12->CreateSampler(&desc, m_Descriptor);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    Sampler::~Sampler() noexcept
    {
        if (!m_pParent->ComputeOnly())
        {
            m_pParent->m_SamplerAllocator.FreeHeapSlot(m_Descriptor, m_DescriptorHeapIndex);
        }
    }
};