// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "sampler.hpp"

static D3D12_SAMPLER_DESC TranslateSamplerDesc(Sampler::Desc const& desc)
{
    D3D12_SAMPLER_DESC ret = {};
    ret.AddressU = ret.AddressV = ret.AddressW =
        [](cl_addressing_mode mode)
    {
        switch (mode)
        {
        default:
        case CL_ADDRESS_CLAMP: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case CL_ADDRESS_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case CL_ADDRESS_MIRRORED_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case CL_ADDRESS_CLAMP_TO_EDGE: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        }
    }(desc.AddressingMode);
    ret.Filter = [](cl_filter_mode mode)
    {
        switch (mode)
        {
        default:
        case CL_FILTER_NEAREST: return D3D12_FILTER_MIN_MAG_MIP_POINT;
        case CL_FILTER_LINEAR: return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        }
    }(desc.FilterMode);
    ret.MaxLOD = std::numeric_limits<float>::max();
    return ret;
}

Sampler::Sampler(Context& Parent, Desc const& desc, const cl_sampler_properties *properties)
    : CLChildBase(Parent)
    , m_Desc(desc)
    , m_Properties(PropertiesToVector(properties))
{
}

D3D12TranslationLayer::Sampler& Sampler::GetUnderlying(D3DDevice* device)
{
    std::lock_guard Lock(m_Lock);
    auto iter = m_UnderlyingSamplers.find(device);
    if (iter != m_UnderlyingSamplers.end())
        return iter->second;

    auto ret = m_UnderlyingSamplers.try_emplace(device, &device->ImmCtx(), TranslateSamplerDesc(m_Desc));
    return ret.first->second;
}



template <typename TReporter>
bool ValidateSamplerProperties(cl_sampler_properties const* properties, TReporter&& ReportError)
{
    constexpr cl_sampler_properties KnownProperties[] =
    {
        CL_SAMPLER_NORMALIZED_COORDS,
        CL_SAMPLER_ADDRESSING_MODE,
        CL_SAMPLER_FILTER_MODE
    };
    bool SeenProperties[std::extent_v<decltype(KnownProperties)>] = {};
    for (auto CurProp = properties; properties && *CurProp; CurProp += 2)
    {
        auto KnownPropIter = std::find(KnownProperties, std::end(KnownProperties), *CurProp);
        if (KnownPropIter == std::end(KnownProperties))
        {
            return !ReportError("Unknown property.", CL_INVALID_PROPERTY);
        }

        auto PropIndex = std::distance(KnownProperties, KnownPropIter);
        if (SeenProperties[PropIndex])
        {
            return !ReportError("Property specified twice.", CL_INVALID_PROPERTY);
        }

        SeenProperties[PropIndex] = true;
    }

    return true;
}

static cl_sampler
clCreateSamplerWithPropertiesImpl(cl_context                     context_,
    const cl_sampler_properties *  sampler_properties,
    Sampler::Desc &                desc,
    cl_int *                       errcode_ret)
{
    if (!context_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return nullptr;
    }
    Context& context = *static_cast<Context*>(context_);
    auto ReportError = context.GetErrorReporter(errcode_ret);

    if (desc.NormalizedCoords > 1)
        desc.NormalizedCoords = 1;
    switch (desc.AddressingMode)
    {
    case CL_ADDRESS_NONE:
    case CL_ADDRESS_CLAMP_TO_EDGE:
    case CL_ADDRESS_CLAMP:
    case CL_ADDRESS_REPEAT:
    case CL_ADDRESS_MIRRORED_REPEAT:
        break;
    default: return ReportError("Invalid sampler addressing mode.", CL_INVALID_VALUE);
    }
    switch (desc.FilterMode)
    {
    case CL_FILTER_LINEAR:
    case CL_FILTER_NEAREST:
        break;
    default: return ReportError("Invalid sampler filter mode.", CL_INVALID_VALUE);
    }

    try
    {
        if (errcode_ret) *errcode_ret = CL_SUCCESS;
        return new Sampler(context, desc, sampler_properties);
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error &) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

CL_API_ENTRY cl_sampler CL_API_CALL
clCreateSamplerWithProperties(cl_context                     context_,
    const cl_sampler_properties *  sampler_properties,
    cl_int *                       errcode_ret) CL_API_SUFFIX__VERSION_2_0
{
    if (!context_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return nullptr;
    }
    Context& context = *static_cast<Context*>(context_);
    auto ReportError = context.GetErrorReporter(errcode_ret);

    if (!ValidateSamplerProperties(sampler_properties, ReportError))
    {
        return nullptr;
    }

    Sampler::Desc desc = { false, CL_ADDRESS_CLAMP, CL_FILTER_NEAREST };
    if (auto FoundVal = FindProperty<cl_sampler_properties>(sampler_properties, CL_SAMPLER_NORMALIZED_COORDS); FoundVal)
        desc.NormalizedCoords = (cl_bool)*FoundVal;
    if (auto FoundVal = FindProperty<cl_sampler_properties>(sampler_properties, CL_SAMPLER_ADDRESSING_MODE); FoundVal)
        desc.AddressingMode = (cl_addressing_mode)*FoundVal;
    if (auto FoundVal = FindProperty<cl_sampler_properties>(sampler_properties, CL_SAMPLER_FILTER_MODE); FoundVal)
        desc.FilterMode = (cl_filter_mode)*FoundVal;

    return clCreateSamplerWithPropertiesImpl(context_, sampler_properties, desc, errcode_ret);
}

CL_API_ENTRY CL_API_PREFIX__VERSION_1_2_DEPRECATED cl_sampler CL_API_CALL
clCreateSampler(cl_context          context,
    cl_bool             normalized_coords,
    cl_addressing_mode  addressing_mode,
    cl_filter_mode      filter_mode,
    cl_int *            errcode_ret) CL_API_SUFFIX__VERSION_1_2_DEPRECATED
{
    Sampler::Desc desc = { normalized_coords, addressing_mode, filter_mode };
    return clCreateSamplerWithPropertiesImpl(context, nullptr, desc, errcode_ret);
}

CL_API_ENTRY cl_int CL_API_CALL
clRetainSampler(cl_sampler sampler) CL_API_SUFFIX__VERSION_1_0
{
    if (!sampler) return CL_INVALID_SAMPLER;
    static_cast<Sampler*>(sampler)->Retain();
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseSampler(cl_sampler sampler) CL_API_SUFFIX__VERSION_1_0
{
    if (!sampler) return CL_INVALID_SAMPLER;
    static_cast<Sampler*>(sampler)->Release();
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetSamplerInfo(cl_sampler         sampler_,
    cl_sampler_info    param_name,
    size_t             param_value_size,
    void *             param_value,
    size_t *           param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!sampler_)
    {
        return CL_INVALID_SAMPLER;
    }

    Sampler& sampler = *static_cast<Sampler*>(sampler_);
    auto& desc = sampler.m_Desc;
    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };

    switch (param_name)
    {
    case CL_SAMPLER_REFERENCE_COUNT: return RetValue(sampler.GetRefCount());
    case CL_SAMPLER_CONTEXT: return RetValue((cl_context)&sampler.m_Parent.get());
    case CL_SAMPLER_NORMALIZED_COORDS: return RetValue(desc.NormalizedCoords);
    case CL_SAMPLER_ADDRESSING_MODE: return RetValue(desc.AddressingMode);
    case CL_SAMPLER_FILTER_MODE: return RetValue(desc.FilterMode);
    case CL_SAMPLER_PROPERTIES:
        return CopyOutParameterImpl(sampler.m_Properties.data(),
            sampler.m_Properties.size() * sizeof(sampler.m_Properties[0]),
            param_value_size, param_value, param_value_size_ret);
    }
    return sampler.m_Parent->GetErrorReporter()("Unknown param_name", CL_INVALID_VALUE);
}

