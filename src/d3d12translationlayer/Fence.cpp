// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "Fence.hpp"
#include "ImmediateContext.hpp"

namespace D3D12TranslationLayer
{
Fence::Fence(ImmediateContext* pParent, FENCE_FLAGS Flags, UINT64 InitialValue)
    : DeviceChild(pParent)
{
    D3D12_FENCE_FLAGS Flags12 =
        ((Flags & FENCE_FLAG_SHARED) ? D3D12_FENCE_FLAG_SHARED : D3D12_FENCE_FLAG_NONE) |
        ((Flags & FENCE_FLAG_SHARED_CROSS_ADAPTER) ? D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER : D3D12_FENCE_FLAG_NONE);

    ThrowFailure(pParent->m_pDevice12->CreateFence(InitialValue, Flags12, IID_PPV_ARGS(&m_spFence)));
}

Fence::Fence(ImmediateContext* pParent, HANDLE hSharedHandle)
    : DeviceChild(pParent)
{
    ThrowFailure(pParent->m_pDevice12->OpenSharedHandle(hSharedHandle, IID_PPV_ARGS(&m_spFence)));
}

Fence::Fence(ImmediateContext* pParent, ID3D12Fence* pFence)
    : DeviceChild(pParent)
{
    ThrowFailure(pFence->QueryInterface(&m_spFence));
}

Fence::~Fence()
{
    AddToDeferredDeletionQueue(m_spFence);
}

HRESULT  Fence::CreateSharedHandle(
    _In_opt_ const SECURITY_ATTRIBUTES *pAttributes,
    _In_ DWORD dwAccess,
    _In_opt_ LPCWSTR lpName,
    _Out_ HANDLE *pHandle)
{
    return m_pParent->m_pDevice12->CreateSharedHandle(m_spFence.get(), pAttributes, dwAccess, lpName, pHandle);
}
}