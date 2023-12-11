// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

namespace D3D12TranslationLayer
{
    enum FENCE_FLAGS
    {
    FENCE_FLAG_NONE = 0x0,
    FENCE_FLAG_SHARED = 0x1,
    FENCE_FLAG_SHARED_CROSS_ADAPTER = 0x2,
    };
    DEFINE_ENUM_FLAG_OPERATORS(FENCE_FLAGS);

    class Fence : public DeviceChild
    {
    public:
        Fence(ImmediateContext* pParent, FENCE_FLAGS Flags, UINT64 InitialValue);
        Fence(ImmediateContext* pParent, HANDLE SharedHandle);
        Fence(ImmediateContext* pParent, ID3D12Fence* pFence);
        Fence(Fence const&) = delete;
        Fence& operator=(Fence const&) = delete;
        Fence(Fence&&) = delete;
        Fence& operator=(Fence&&) = delete;

        ~Fence();

        UINT64 GetCompletedValue() const { return m_spFence->GetCompletedValue(); }
        void Signal(UINT64 Value) const { ThrowFailure(m_spFence->Signal(Value)); }
        HRESULT SetEventOnCompletion(UINT64 Value, HANDLE hEvent) const { return m_spFence->SetEventOnCompletion(Value, hEvent); }
        HRESULT CreateSharedHandle(
            _In_opt_ const SECURITY_ATTRIBUTES *pAttributes,
            _In_ DWORD dwAccess,
            _In_opt_ LPCWSTR lpName,
            _Out_ HANDLE *pHandle);

        ID3D12Fence1* Get() const { return m_spFence.get(); }

    private:
        unique_comptr<ID3D12Fence1> m_spFence;
    };
}