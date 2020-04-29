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
    Sampler(Context& Parent, Desc const& desc);

    Desc const& GetDesc() const { return m_Desc; }
    D3D12TranslationLayer::Sampler& GetUnderlying() { return m_UnderlyingSampler; }

private:
    const Desc m_Desc;
    D3D12TranslationLayer::Sampler m_UnderlyingSampler;
};
