// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

namespace D3D12TranslationLayer
{

    void ViewBase::UsedInCommandList(COMMAND_LIST_TYPE commandListType, UINT64 id) 
    {
        if (m_pResource) { m_pResource->UsedInCommandList(commandListType, id); }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UAV::UAV(ImmediateContext* pDevice, const TTranslationLayerDesc &Desc, Resource &ViewResource) noexcept(false)
        : TUAV(pDevice, Desc.m_Desc12, ViewResource), m_D3D11UAVFlags(Desc.m_D3D11UAVFlags)
    {
    }

    UAV::~UAV() noexcept(false)
    {
    }
};