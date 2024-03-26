// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "d3d12.h"
#include <memory>
#include <utility>
#include <wrl/client.h>

class ShaderCache
{
public:
    ShaderCache(ID3D12Device*, bool driverVersioned);

    bool HasCache() const
    {
#ifdef __ID3D12ShaderCacheSession_INTERFACE_DEFINED__
        return m_pSession;
#else
        return false;
#endif
    }

    void Store(const void* key, size_t keySize, const void* value, size_t valueSize) noexcept;
    void Store(const void* const* keys, const size_t* keySizes, unsigned keyParts, const void* value, size_t valueSize);

    using FoundValue = std::pair<std::unique_ptr<byte[]>, size_t>;
    FoundValue Find(const void* key, size_t keySize);
    FoundValue Find(const void* const* keys, const size_t* keySizes, unsigned keyParts);

    void Close();

#ifdef __ID3D12ShaderCacheSession_INTERFACE_DEFINED__
private:
    Microsoft::WRL::ComPtr<ID3D12ShaderCacheSession> m_pSession;
#endif
};
