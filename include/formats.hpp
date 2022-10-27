// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <limits>
#include "gl_tokens.hpp"

constexpr DXGI_FORMAT GetDXGIFormatForCLImageFormat(cl_image_format const& image_format)
{
    switch (image_format.image_channel_data_type)
    {
    case CL_UNSIGNED_INT32:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R32G32B32A32_UINT;
        case CL_RGB: return DXGI_FORMAT_R32G32B32_UINT;
        case CL_RG: return DXGI_FORMAT_R32G32_UINT;
        case CL_R: return DXGI_FORMAT_R32_UINT;
        }
        break;
    case CL_SIGNED_INT32:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R32G32B32A32_SINT;
        case CL_RGB: return DXGI_FORMAT_R32G32B32_SINT;
        case CL_RG: return DXGI_FORMAT_R32G32_SINT;
        case CL_R: return DXGI_FORMAT_R32_SINT;
        }
        break;
    case CL_FLOAT:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case CL_RGB: return DXGI_FORMAT_R32G32B32_FLOAT;
        case CL_RG: return DXGI_FORMAT_R32G32_FLOAT;
        case CL_R: return DXGI_FORMAT_R32_FLOAT;
        }
        break;
    case CL_UNSIGNED_INT16:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R16G16B16A16_UINT;
        case CL_RG: return DXGI_FORMAT_R16G16_UINT;
        case CL_R: return DXGI_FORMAT_R16_UINT;
        }
        break;
    case CL_SIGNED_INT16:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R16G16B16A16_SINT;
        case CL_RG: return DXGI_FORMAT_R16G16_SINT;
        case CL_R: return DXGI_FORMAT_R16_SINT;
        }
        break;
    case CL_UNORM_INT16:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case CL_RG: return DXGI_FORMAT_R16G16_UNORM;
        case CL_R: return DXGI_FORMAT_R16_UNORM;
        }
        break;
    case CL_SNORM_INT16:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case CL_RG: return DXGI_FORMAT_R16G16_SNORM;
        case CL_R: return DXGI_FORMAT_R16_SNORM;
        }
        break;
    case CL_HALF_FLOAT:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case CL_RG: return DXGI_FORMAT_R16G16_FLOAT;
        case CL_R: return DXGI_FORMAT_R16_FLOAT;
        }
        break;
    case CL_UNSIGNED_INT8:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R8G8B8A8_UINT;
        case CL_RG: return DXGI_FORMAT_R8G8_UINT;
        case CL_R: return DXGI_FORMAT_R8_UINT;
        }
        break;
    case CL_SIGNED_INT8:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R8G8B8A8_SINT;
        case CL_RG: return DXGI_FORMAT_R8G8_SINT;
        case CL_R: return DXGI_FORMAT_R8_SINT;
        }
        break;
    case CL_UNORM_INT8:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case CL_RG: return DXGI_FORMAT_R8G8_UNORM;
        case CL_R: return DXGI_FORMAT_R8_UNORM;
        case CL_BGRA: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case CL_A: return DXGI_FORMAT_A8_UNORM;
        }
        break;
    case CL_SNORM_INT8:
        switch (image_format.image_channel_order)
        {
        case CL_RGBA: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case CL_RG: return DXGI_FORMAT_R8G8_SNORM;
        case CL_R: return DXGI_FORMAT_R8_SNORM;
        }
        break;
    case CL_UNORM_INT_101010:
        switch (image_format.image_channel_order)
        {
        case CL_RGBx: return DXGI_FORMAT_R10G10B10A2_UNORM;
        }
        break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

constexpr cl_image_format GetCLImageFormatForDXGIFormat(DXGI_FORMAT fmt, cl_GLuint glFmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return { CL_RGBA, CL_FLOAT };
    case DXGI_FORMAT_R32G32B32A32_UINT: return { CL_RGBA, CL_UNSIGNED_INT32 };
    case DXGI_FORMAT_R32G32B32A32_SINT: return { CL_RGBA, CL_SIGNED_INT32 };
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        switch (glFmt)
        {
        case GL_RGBA32F: return { CL_RGBA, CL_FLOAT };
        case GL_RGBA32UI: return { CL_RGBA, CL_UNSIGNED_INT32 };
        case GL_RGBA32I: return { CL_RGBA, CL_SIGNED_INT32 };
        }
        break;
    case DXGI_FORMAT_R32G32B32_FLOAT: return { CL_RGB, CL_FLOAT };
    case DXGI_FORMAT_R32G32B32_UINT: return { CL_RGB, CL_UNSIGNED_INT32 };
    case DXGI_FORMAT_R32G32B32_SINT: return { CL_RGB, CL_SIGNED_INT32 };
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return { CL_RGBA, CL_HALF_FLOAT };
    case DXGI_FORMAT_R16G16B16A16_UNORM: return { CL_RGBA, CL_UNORM_INT16 };
    case DXGI_FORMAT_R16G16B16A16_UINT: return { CL_RGBA, CL_UNSIGNED_INT16 };
    case DXGI_FORMAT_R16G16B16A16_SNORM: return { CL_RGBA, CL_SNORM_INT16 };
    case DXGI_FORMAT_R16G16B16A16_SINT: return { CL_RGBA, CL_SIGNED_INT16 };
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        switch (glFmt)
        {
        case GL_RGBA16F: return { CL_RGBA, CL_HALF_FLOAT };
        case GL_RGBA16: return { CL_RGBA, CL_UNORM_INT16 };
        case GL_RGBA16UI: return { CL_RGBA, CL_UNSIGNED_INT16 };
        case GL_RGBA16_SNORM: return { CL_RGBA, CL_SNORM_INT16 };
        case GL_RGBA16I: return { CL_RGBA, CL_SIGNED_INT16 };
        }
        break;
    case DXGI_FORMAT_R32G32_FLOAT: return { CL_RG, CL_FLOAT };
    case DXGI_FORMAT_R32G32_UINT: return { CL_RG, CL_UNSIGNED_INT32 };
    case DXGI_FORMAT_R32G32_SINT: return { CL_RG, CL_SIGNED_INT32 };
    case DXGI_FORMAT_R32G32_TYPELESS:
        switch (glFmt)
        {
        case GL_RG32F: return { CL_RG, CL_FLOAT };
        case GL_RG32UI: return { CL_RG, CL_UNSIGNED_INT32 };
        case GL_RG32I: return { CL_RG, CL_SIGNED_INT32 };
        }
        break;
    // This is failing a bunch of tests - see about re-enabling as a proper 1010102
    //case DXGI_FORMAT_R10G10B10A2_UNORM: return { CL_RGBx, CL_UNORM_INT_101010 };
    case DXGI_FORMAT_R8G8B8A8_UNORM: return { CL_RGBA, CL_UNORM_INT8 };
    case DXGI_FORMAT_R8G8B8A8_UINT: return { CL_RGBA, CL_UNSIGNED_INT8 };
    case DXGI_FORMAT_R8G8B8A8_SNORM: return { CL_RGBA, CL_SNORM_INT8 };
    case DXGI_FORMAT_R8G8B8A8_SINT: return { CL_RGBA, CL_SIGNED_INT8 };
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        switch (glFmt)
        {
        case GL_RGBA8: return { CL_RGBA, CL_UNORM_INT8 };
        case GL_RGBA8UI: return { CL_RGBA, CL_UNSIGNED_INT8 };
        case GL_RGBA8_SNORM: return { CL_RGBA, CL_SNORM_INT8 };
        case GL_RGBA8I: return { CL_RGBA, CL_SIGNED_INT8 };
        case GL_UNSIGNED_INT_8_8_8_8_REV: return { CL_RGBA, CL_UNORM_INT8 };
        case GL_RGBA: return { CL_RGBA, CL_UNORM_INT8 };
        }
        break;
    case DXGI_FORMAT_R16G16_FLOAT: return { CL_RG, CL_HALF_FLOAT };
    case DXGI_FORMAT_R16G16_UNORM: return { CL_RG, CL_UNORM_INT16 };
    case DXGI_FORMAT_R16G16_UINT: return { CL_RG, CL_UNSIGNED_INT16 };
    case DXGI_FORMAT_R16G16_SNORM: return { CL_RG, CL_SNORM_INT16 };
    case DXGI_FORMAT_R16G16_SINT: return { CL_RG, CL_SIGNED_INT16 };
    case DXGI_FORMAT_R16G16_TYPELESS:
        switch (glFmt)
        {
        case GL_RG16F: return { CL_RG, CL_HALF_FLOAT };
        case GL_RG16: return { CL_RG, CL_UNORM_INT16 };
        case GL_RG16UI: return { CL_RG, CL_UNSIGNED_INT16 };
        case GL_RG16_SNORM: return { CL_RG, CL_SNORM_INT16 };
        case GL_RG16I: return { CL_RG, CL_SIGNED_INT16 };
        }
        break;
    case DXGI_FORMAT_R32_FLOAT: return { CL_R, CL_FLOAT };
    case DXGI_FORMAT_R32_UINT: return { CL_R, CL_UNSIGNED_INT32 };
    case DXGI_FORMAT_R32_SINT: return { CL_R, CL_SIGNED_INT32 };
    case DXGI_FORMAT_R32_TYPELESS:
        switch (glFmt)
        {
        case GL_R32F: return { CL_R, CL_FLOAT };
        case GL_R32UI: return { CL_R, CL_UNSIGNED_INT32 };
        case GL_R32I: return { CL_R, CL_SIGNED_INT32 };
        }
        break;
    case DXGI_FORMAT_R8G8_UNORM: return { CL_RG, CL_UNORM_INT8 };
    case DXGI_FORMAT_R8G8_UINT: return { CL_RG, CL_UNSIGNED_INT8 };
    case DXGI_FORMAT_R8G8_SNORM: return { CL_RG, CL_SNORM_INT8 };
    case DXGI_FORMAT_R8G8_SINT: return { CL_RG, CL_SIGNED_INT8 };
    case DXGI_FORMAT_R8G8_TYPELESS:
        switch (glFmt)
        {
        case GL_RG8: return { CL_RG, CL_UNORM_INT8 };
        case GL_RG8UI: return { CL_RG, CL_UNSIGNED_INT8 };
        case GL_RG8_SNORM: return { CL_RG, CL_SNORM_INT8 };
        case GL_RG8I: return { CL_RG, CL_SIGNED_INT8 };
        }
        break;
    case DXGI_FORMAT_R16_FLOAT: return { CL_R, CL_HALF_FLOAT };
    case DXGI_FORMAT_R16_UNORM: return { CL_R, CL_UNORM_INT16 };
    case DXGI_FORMAT_R16_UINT: return { CL_R, CL_UNSIGNED_INT16 };
    case DXGI_FORMAT_R16_SNORM: return { CL_R, CL_SNORM_INT16 };
    case DXGI_FORMAT_R16_SINT: return { CL_R, CL_SIGNED_INT16 };
    case DXGI_FORMAT_R16_TYPELESS:
        switch (glFmt)
        {
        case GL_R16F: return { CL_R, CL_HALF_FLOAT };
        case GL_R16: return { CL_R, CL_UNORM_INT16 };
        case GL_R16UI: return { CL_R, CL_UNSIGNED_INT16 };
        case GL_R16_SNORM: return { CL_R, CL_SNORM_INT16 };
        case GL_R16I: return { CL_R, CL_SIGNED_INT16 };
        }
        break;
    case DXGI_FORMAT_R8_UNORM: return { CL_R, CL_UNORM_INT8 };
    case DXGI_FORMAT_R8_UINT: return { CL_R, CL_UNSIGNED_INT8 };
    case DXGI_FORMAT_R8_SNORM: return { CL_R, CL_SNORM_INT8 };
    case DXGI_FORMAT_R8_SINT: return { CL_R, CL_SIGNED_INT8 };
    case DXGI_FORMAT_R8_TYPELESS:
        switch (glFmt)
        {
        case GL_R8: return { CL_R, CL_UNORM_INT8 };
        case GL_R8UI: return { CL_R, CL_UNSIGNED_INT8 };
        case GL_R8_SNORM: return { CL_R, CL_SNORM_INT8 };
        case GL_R8I: return { CL_R, CL_SIGNED_INT8 };
        }
        break;
    case DXGI_FORMAT_A8_UNORM: return { CL_A, CL_UNORM_INT8 };
    case DXGI_FORMAT_B8G8R8A8_UNORM: return { CL_BGRA, CL_UNORM_INT8 };
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return { CL_BGRA, CL_UNORM_INT8 };
    }
    return {};
}

constexpr cl_image_format GetCLImageFormatForGLFormat(cl_GLuint fmt)
{
    switch (fmt)
    {
    case GL_RGBA32F: return { CL_RGBA, CL_FLOAT };
    case GL_RGBA32UI: return { CL_RGBA, CL_UNSIGNED_INT32 };
    case GL_RGBA32I: return { CL_RGBA, CL_SIGNED_INT32 };
    case GL_RGBA16F: return { CL_RGBA, CL_HALF_FLOAT };
    case GL_RGBA16: return { CL_RGBA, CL_UNORM_INT16 };
    case GL_RGBA16UI: return { CL_RGBA, CL_UNSIGNED_INT16 };
    case GL_RGBA16_SNORM: return { CL_RGBA, CL_SNORM_INT16 };
    case GL_RGBA16I: return { CL_RGBA, CL_SIGNED_INT16 };
    case GL_RG32F: return { CL_RG, CL_FLOAT };
    case GL_RG32UI: return { CL_RG, CL_UNSIGNED_INT32 };
    case GL_RG32I: return { CL_RG, CL_SIGNED_INT32 };
    case GL_RGBA8: return { CL_RGBA, CL_UNORM_INT8 };
    case GL_RGBA8UI: return { CL_RGBA, CL_UNSIGNED_INT8 };
    case GL_RGBA8_SNORM: return { CL_RGBA, CL_SNORM_INT8 };
    case GL_RGBA8I: return { CL_RGBA, CL_SIGNED_INT8 };
    case GL_UNSIGNED_INT_8_8_8_8_REV: return { CL_RGBA, CL_UNORM_INT8 };
    case GL_RGBA: return { CL_RGBA, CL_UNORM_INT8 };
    case GL_RG16F: return { CL_RG, CL_HALF_FLOAT };
    case GL_RG16: return { CL_RG, CL_UNORM_INT16 };
    case GL_RG16UI: return { CL_RG, CL_UNSIGNED_INT16 };
    case GL_RG16_SNORM: return { CL_RG, CL_SNORM_INT16 };
    case GL_RG16I: return { CL_RG, CL_SIGNED_INT16 };
    case GL_R32F: return { CL_R, CL_FLOAT };
    case GL_R32UI: return { CL_R, CL_UNSIGNED_INT32 };
    case GL_R32I: return { CL_R, CL_SIGNED_INT32 };
    case GL_RG8: return { CL_RG, CL_UNORM_INT8 };
    case GL_RG8UI: return { CL_RG, CL_UNSIGNED_INT8 };
    case GL_RG8_SNORM: return { CL_RG, CL_SNORM_INT8 };
    case GL_RG8I: return { CL_RG, CL_SIGNED_INT8 };
    case GL_R16F: return { CL_R, CL_HALF_FLOAT };
    case GL_R16: return { CL_R, CL_UNORM_INT16 };
    case GL_R16UI: return { CL_R, CL_UNSIGNED_INT16 };
    case GL_R16_SNORM: return { CL_R, CL_SNORM_INT16 };
    case GL_R16I: return { CL_R, CL_SIGNED_INT16 };
    case GL_R8: return { CL_R, CL_UNORM_INT8 };
    case GL_R8UI: return { CL_R, CL_UNSIGNED_INT8 };
    case GL_R8_SNORM: return { CL_R, CL_SNORM_INT8 };
    case GL_R8I: return { CL_R, CL_SIGNED_INT8 };
    case GL_BGRA: return { CL_BGRA, CL_UNORM_INT8 };
    }
    return {};
}

inline cl_uint GetNumChannelsInOrder(cl_channel_order order)
{
    switch (order)
    {
    default:
    case CL_RGBA: return 4;
    case CL_ARGB: return 4;
    case CL_BGRA: return 4;
    case CL_RGB: return 3;
    case CL_RG: return 2;
    case CL_R: return 1;
    case CL_A: return 1;
    }
}

inline cl_uint GetChannelSizeBits(cl_channel_type type)
{
    switch (type)
    {
    default:
    case CL_UNSIGNED_INT32:
    case CL_SIGNED_INT32:
    case CL_FLOAT:
        return 32;
    case CL_UNSIGNED_INT16:
    case CL_SIGNED_INT16:
    case CL_UNORM_INT16:
    case CL_SNORM_INT16:
    case CL_HALF_FLOAT:
        return 16;
    case CL_UNSIGNED_INT8:
    case CL_SIGNED_INT8:
    case CL_UNORM_INT8:
    case CL_SNORM_INT8:
        return 8;
    case CL_UNORM_INT_101010:
        return 10;
    }
}

inline cl_uint GetFormatSizeBytes(cl_image_format format)
{
    switch (format.image_channel_data_type)
    {
    case 0:
        return 1;
    case CL_UNORM_INT_101010:
        return 4;
    default:
        return GetChannelSizeBits(format.image_channel_data_type) *
            GetNumChannelsInOrder(format.image_channel_order) / 8;
    }
}

inline float ConvertHalfToFloat(unsigned short halfValue)
{
    int sign = (halfValue >> 15) & 0x0001;
    int exponent = (halfValue >> 10) & 0x001f;
    int mantissa = (halfValue) & 0x03ff;

    union
    {
        unsigned int bits;
        float floatValue;
    } outFloat;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            outFloat.bits = sign << 31;
            return outFloat.floatValue;
        }

        while ((mantissa & 0x00000400) == 0)
        {
            mantissa <<= 1;
            exponent--;
        }

        exponent++;
        mantissa &= ~(0x00000400);
    }
    else if (exponent == 31)
    {
        outFloat.bits = (sign << 31) | 0x7f800000 | (mantissa << 13);
        return outFloat.floatValue;
    }

    exponent += (127 - 15);
    mantissa <<= 13;

    outFloat.bits = (sign << 31) | (exponent << 23) | mantissa;
    return outFloat.floatValue;
}

inline cl_ushort ConvertFloatToHalf(float f)
{
    union { float f; cl_uint u; } u = { f };
    cl_uint sign = (u.u >> 16) & 0x8000;
    float x = fabsf(f);

    if (x != x)
    {
        u.u >>= (24 - 11);
        u.u &= 0x7fff;
        u.u |= 0x0200;
        return (cl_ushort)(u.u | sign);
    }

    if (x >= 0x1.0p16f)
    {
        if (x == INFINITY)
            return (cl_ushort)(0x7c00 | sign);
        return (cl_ushort)(0x7bff | sign);
    }

    if (x < 0x1.0p-24f)
        return (cl_ushort)sign;

    if (x < 0x1.0p-14f)
    {
        x *= 0x1.0p24f;
        return (cl_ushort)((int)x | sign);
    }

    u.u &= 0xFFFFE000U;
    u.u -= 0x38000000U;

    return (cl_ushort)((u.u >> (24 - 11)) | sign);
}
