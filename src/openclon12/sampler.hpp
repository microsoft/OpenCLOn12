// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "context.hpp"

class Sampler : public CLChildBase<Sampler, Context, cl_sampler>
{
public:
    struct Desc
    {
        cl_bool NormalizedCoords;
        cl_addressing_mode AddressingMode;
        cl_filter_mode FilterMode;
    };
    Sampler(Context& Parent, Desc const& desc, const cl_sampler_properties *properties);

    D3D12TranslationLayer::Sampler& GetUnderlying(D3DDevice*);

    const Desc m_Desc;
    const std::vector<cl_sampler_properties> m_Properties;
private:
    std::mutex m_Lock;
    std::unordered_map<class D3DDevice*, D3D12TranslationLayer::Sampler> m_UnderlyingSamplers;
};
