#pragma once

#define NOMINMAX
#define CL_USE_DEPRECATED_OPENCL_1_0_APIS
#define CL_USE_DEPRECATED_OPENCL_1_1_APIS
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS

#include <CL/cl.h>
#include <CL/cl_ext.h>
#include <icd_dispatch.h>

#include <type_traits>
#include <memory>
#include <vector>
#include <atomic>
#include <map>
#include <algorithm>
#ifndef assert
#include <assert.h>
#endif

using std::min;
using std::max;

#include <D3D12TranslationLayerDependencyIncludes.h>
#include <D3D12TranslationLayerIncludes.h>

#include <initguid.h>
#include <d3d12.h>
#include <dxcore.h>

#include <wrl.h>
using Microsoft::WRL::ComPtr;

#define WIL_ENABLE_EXCEPTIONS
#include <wil/result_macros.h>

template <typename TClass, typename TCLPtr>
class CLBase : public std::remove_pointer_t<TCLPtr>
{
public:
    static TClass* CastFrom(TCLPtr handle) { return static_cast<TClass*>(handle); }
};

struct adopt_ref {};

class Device;
class Platform : public CLBase<Platform, cl_platform_id>
{
public:
    static constexpr char* Profile = "FULL_PROFILE";
    static constexpr char* Version = "OpenCL 1.0 D3D12 Implementation";
    static constexpr char* Name = "OpenCLOn12";
    static constexpr char* Vendor = "Microsoft";
    static constexpr char* Extensions = "cl_khr_icd";
    static constexpr char* ICDSuffix = "oclon12";

    Platform();
    ~Platform();

    cl_uint GetNumDevices() const noexcept;
    cl_device_id GetDevice(cl_uint i) const noexcept;

    class ref_int
    {
        Platform& m_obj;
    public:
        Platform& get() const { return m_obj; }
        ref_int(Platform& obj, adopt_ref const& = {}) noexcept : m_obj(obj) { }
        ref_int(ref_int const& o) noexcept : m_obj(o.get()) { m_obj; }
        Platform* operator->() const { return &m_obj; }
    };

protected:
    ComPtr<IDXCoreAdapterList> m_spAdapters;
    std::vector<std::unique_ptr<Device>> m_Devices;
};
extern std::unique_ptr<Platform> g_Platform;

template <typename TClass>
class ref_ptr
{
    TClass* m_pPtr = nullptr;
    void Retain() noexcept { if (m_pPtr) { m_pPtr->Retain(); } }
public:
    void Release() noexcept { if (m_pPtr) { m_pPtr->Release(); m_pPtr = nullptr; } }
    TClass* Detach() noexcept { auto pPtr = m_pPtr; m_pPtr = nullptr; return pPtr; }
    TClass* Get() const noexcept { return m_pPtr; }

    ref_ptr(TClass* p) noexcept : m_pPtr(p) { Retain(); }
    ref_ptr(TClass* p, adopt_ref const&) noexcept : m_pPtr(p) { }
    ref_ptr() noexcept = default;
    ref_ptr(ref_ptr const& o) noexcept : m_pPtr(o.Get()) { Retain(); }
    ref_ptr& operator=(ref_ptr const& o) noexcept { Release(); m_pPtr = o.m_pPtr; Retain(); return *this; }
    ref_ptr(ref_ptr&& o) noexcept : m_pPtr(o.Detach()) { }
    ref_ptr& operator=(ref_ptr &&o) noexcept { Release(); m_pPtr = o.Detach(); return *this; }
    ~ref_ptr() noexcept { Release(); }

    TClass* operator->() const { return m_pPtr; }
};
template <typename TClass>
class ref_ptr_int
{
    TClass* m_pPtr = nullptr;
    void Retain() noexcept { if (m_pPtr) { m_pPtr->AddInternalRef(); } }
public:
    void Release() noexcept { if (m_pPtr) { m_pPtr->ReleaseInternalRef(); m_pPtr = nullptr; } }
    TClass* Detach() noexcept { auto pPtr = m_pPtr; m_pPtr = nullptr; return pPtr; }
    TClass* Get() const noexcept { return m_pPtr; }

    ref_ptr_int(TClass* p) noexcept : m_pPtr(p) { Retain(); }
    ref_ptr_int(TClass* p, adopt_ref const&) noexcept : m_pPtr(p) { }
    ref_ptr_int() noexcept = default;
    ref_ptr_int(ref_ptr_int const& o) noexcept : m_pPtr(o.Get()) { Retain(); }
    ref_ptr_int& operator=(ref_ptr_int const& o) noexcept { Release(); m_pPtr = o.m_pPtr; Retain(); return *this; }
    ref_ptr_int(ref_ptr_int&& o) noexcept : m_pPtr(o.Detach()) { }
    ref_ptr_int& operator=(ref_ptr_int &&o) noexcept { Release(); m_pPtr = o.Detach(); return *this; }
    ~ref_ptr_int() noexcept { Release(); }

    TClass* operator->() const { return m_pPtr; }
};
template <typename TClass>
class ref
{
    TClass& m_obj;
public:
    TClass& get() const noexcept { return m_obj; }
    ref(TClass& obj) noexcept : m_obj(obj) { m_obj.Retain(); }
    ref(TClass& obj, adopt_ref const&) noexcept : m_obj(obj) { }
    ref(ref const& o) noexcept : m_obj(o.get()) { m_obj.Retain(); }
    ~ref() noexcept { m_obj.Release(); }

    TClass* operator->() const { return &m_obj; }
};
template <typename TClass>
class ref_int
{
    TClass& m_obj;
public:
    TClass& get() const { return m_obj; }
    ref_int(TClass& obj) noexcept : m_obj(obj) { m_obj.AddInternalRef(); }
    ref_int(TClass& obj, adopt_ref const&) noexcept : m_obj(obj) { }
    ref_int(ref_int const& o) noexcept : m_obj(o.get()) { m_obj.AddInternalRef(); }
    ~ref_int() noexcept { m_obj.ReleaseInternalRef(); }

    TClass* operator->() const { return &m_obj; }
};
template <typename TClass, typename TParent, typename TCLPtr>
class CLChildBase : public CLBase<TClass, TCLPtr>
{
public:
    typename TParent::ref_int m_Parent;
    std::atomic<uint64_t> m_RefCount = 1;

    CLChildBase(TParent& parent) : m_Parent(parent)
    {
        this->dispatch = m_Parent->dispatch;
    }
    void Retain() { ++m_RefCount; }
    void Release() { if (--m_RefCount == 0) delete static_cast<TClass*>(this); }
    void AddInternalRef() { m_RefCount += (1ull << 32); }
    void ReleaseInternalRef() { if ((m_RefCount -= (1ull << 32)) == 0) delete static_cast<TClass*>(this); }
    uint32_t GetRefCount() { return static_cast<uint32_t>(m_RefCount.load()); }

    using ref_ptr = ::ref_ptr<TClass>;
    using ref_ptr_int = ::ref_ptr_int<TClass>;
    using ref = ::ref<TClass>;
    using ref_int = ::ref_int<TClass>;
};

// Helpers for property arrays as inputs
template <typename TProperties>
std::vector<TProperties> PropertiesToVector(const TProperties* Props)
{
    std::vector<TProperties> Ret;
    if (Props == nullptr)
        return Ret;
    auto EndProp = Props;
    for (; *EndProp != 0; EndProp += 2);
    Ret.assign(Props, EndProp);
    return Ret;
}

template <typename TProperties>
TProperties const* FindProperty(const TProperties* Props, TProperties value)
{
    if (Props == nullptr)
        return nullptr;
    for (auto CurProp = Props; *CurProp != 0; CurProp += 2)
    {
        if (*CurProp == value)
            return &CurProp[1];
    }
    return nullptr;
}

// Helpers for property getters
inline cl_int CopyOutParameterImpl(const void* pValue, size_t ValueSize, size_t InputValueSize, void* pOutValue, size_t* pOutValueSize)
{
    if (InputValueSize && InputValueSize < ValueSize)
    {
        return CL_INVALID_VALUE;
    }
    if (InputValueSize)
    {
        memcpy(pOutValue, pValue, ValueSize);
    }
    if (pOutValueSize)
    {
        *pOutValueSize = ValueSize;
    }
    return CL_SUCCESS;
}
template <typename T>
inline cl_int CopyOutParameter(T value, size_t param_value_size, void* param_value, size_t* param_value_size_ret)
{
    return CopyOutParameterImpl(&value, sizeof(T), param_value_size, param_value, param_value_size_ret);
}
template <typename T, size_t size>
inline cl_int CopyOutParameter(const T(&value)[size], size_t param_value_size, void* param_value, size_t* param_value_size_ret)
{
    return CopyOutParameterImpl(&value, sizeof(value), param_value_size, param_value, param_value_size_ret);
}
inline cl_int CopyOutParameter(const char* value, size_t param_value_size, void* param_value, size_t* param_value_size_ret)
{
    return CopyOutParameterImpl(value, strlen(value) + 1, param_value_size, param_value, param_value_size_ret);
}

inline bool IsZeroOrPow2(cl_bitfield bits)
{
    return !bits || !(bits & (bits - 1));
}
inline bool IsPow2(cl_bitfield bits)
{
    return bits && !(bits & (bits - 1));
}
