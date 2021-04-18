// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "platform.hpp"
#include "cache.hpp"
#include "compiler.hpp"
#include <numeric>

#pragma warning(disable: 4100)

ShaderCache::ShaderCache(ID3D12Device* d)
{
#ifdef __ID3D12ShaderCacheSession_INTERFACE_DEFINED__
    ComPtr<ID3D12Device9> device9;
    if (FAILED(d->QueryInterface(device9.ReleaseAndGetAddressOf())))
        return;

    D3D12_SHADER_CACHE_SESSION_DESC Desc = {};
    // {17CB474E-4C55-4DBC-BC2E-D5132115BDA3}
    Desc.Identifier = { 0x17cb474e, 0x4c55, 0x4dbc, { 0xbc, 0x2e, 0xd5, 0x13, 0x21, 0x15, 0xbd, 0xa3 } };
    Desc.Mode = D3D12_SHADER_CACHE_MODE_DISK;

    auto pCompiler = g_Platform->GetCompiler();
    Desc.Version = pCompiler->GetVersionForCache();

    (void)device9->CreateShaderCacheSession(&Desc, IID_PPV_ARGS(&m_pSession));
#endif
}

void ShaderCache::Store(const void* key, size_t keySize, const void* value, size_t valueSize) noexcept
{
#ifdef __ID3D12ShaderCacheSession_INTERFACE_DEFINED__
    if (m_pSession)
    {
        (void)m_pSession->StoreValue(key, (UINT)keySize, value, (UINT)valueSize);
    }
#endif
}

void ShaderCache::Store(const void* const* keys, const size_t* keySizes, unsigned keyParts, const void* value, size_t valueSize)
{
#ifdef __ID3D12ShaderCacheSession_INTERFACE_DEFINED__
    if (m_pSession)
    {
        size_t combinedSize = std::accumulate(keySizes, keySizes + keyParts, (size_t)0);
        std::unique_ptr<byte[]> combinedKey(new byte[combinedSize]);

        unsigned i = 0;
        for (byte* ptr = combinedKey.get(); ptr != combinedKey.get() + combinedSize; ptr += keySizes[i++])
        {
            memcpy(ptr, keys[i], keySizes[i]);
        }

        Store(combinedKey.get(), combinedSize, value, valueSize);
    }
#endif
}

ShaderCache::FoundValue ShaderCache::Find(const void* key, size_t keySize)
{
#ifdef __ID3D12ShaderCacheSession_INTERFACE_DEFINED__
    if (m_pSession)
    {
        UINT valueSize = 0;
        if (SUCCEEDED(m_pSession->FindValue(key, (UINT)keySize, nullptr, &valueSize)))
        {
            ShaderCache::FoundValue value(new byte[valueSize], valueSize);
            if (SUCCEEDED(m_pSession->FindValue(key, (UINT)keySize, value.first.get(), &valueSize)))
            {
                return value;
            }
        }
    }
#endif
    return {};
}

ShaderCache::FoundValue ShaderCache::Find(const void* const* keys, const size_t* keySizes, unsigned keyParts)
{
#ifdef __ID3D12ShaderCacheSession_INTERFACE_DEFINED__
    if (m_pSession)
    {
        size_t combinedSize = std::accumulate(keySizes, keySizes + keyParts, (size_t)0);
        std::unique_ptr<byte[]> combinedKey(new byte[combinedSize]);

        unsigned i = 0;
        for (byte* ptr = combinedKey.get(); ptr != combinedKey.get() + combinedSize; ptr += keySizes[i++])
        {
            memcpy(ptr, keys[i], keySizes[i]);
        }

        return Find(combinedKey.get(), combinedSize);
    }
#endif
    return {};
}

void ShaderCache::Close()
{
#ifdef __ID3D12ShaderCacheSession_INTERFACE_DEFINED__
    m_pSession.Reset();
#endif
}
