// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "DeviceChild.hpp"
#include "ImmediateContext.hpp"

namespace D3D12TranslationLayer
{
    void DeviceChild::AddToDeferredDeletionQueue(ID3D12Object* pObject)
    {
        m_pParent->AddObjectToDeferredDeletionQueue(pObject, m_LastUsedCommandListID);
    }
};