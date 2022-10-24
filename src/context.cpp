// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "context.hpp"

template <typename TReporter>
bool ValidateContextProperties(cl_context_properties const* properties, TReporter&& ReportError)
{
    constexpr cl_context_properties KnownProperties[] =
    {
        CL_CONTEXT_PLATFORM, CL_CONTEXT_INTEROP_USER_SYNC,
        CL_GL_CONTEXT_KHR, CL_EGL_DISPLAY_KHR, CL_GLX_DISPLAY_KHR,
        CL_WGL_HDC_KHR, CL_CGL_SHAREGROUP_KHR,
    };
    bool SeenProperties[std::extent_v<decltype(KnownProperties)>] = {};
    bool glContext = false, eglDisplay = false, wglDisplay = false;
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

        switch (*CurProp)
        {
        case CL_GL_CONTEXT_KHR:
            glContext = *(CurProp + 1) != 0;
            break;
        case CL_EGL_DISPLAY_KHR:
            eglDisplay = *(CurProp + 1) != 0;
            break;
        case CL_WGL_HDC_KHR:
            wglDisplay = *(CurProp + 1) != 0;
            break;
        case CL_CGL_SHAREGROUP_KHR:
            return !ReportError("CGL unsupported.", CL_INVALID_OPERATION);
        case CL_GLX_DISPLAY_KHR:
            return !ReportError("GLX unsupported.", CL_INVALID_OPERATION);
        }
    }

    if (glContext && !(eglDisplay || wglDisplay))
    {
        return !ReportError("A GL context was provided, but no WGL or EGL display.", CL_INVALID_OPERATION);
    }
    if (!glContext && (eglDisplay || wglDisplay))
    {
        return !ReportError("A GL context was not provided, but a WGL or EGL display was.", CL_INVALID_OPERATION);
    }
    if (eglDisplay && wglDisplay)
    {
        return ReportError("If a GL context is provided, only one of WGL or EGL displays should be present.", CL_INVALID_OPERATION);
    }

    return true;
}

/* Context APIs */
extern CL_API_ENTRY cl_context CL_API_CALL
clCreateContext(const cl_context_properties * properties,
    cl_uint              num_devices,
    const cl_device_id * devices,
    Context::PfnCallbackType pfn_notify,
    void *               user_data,
    cl_int *             errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    auto ReportError = [&](const char* errorMessage, cl_int errorCode) -> cl_context
    {
        if (pfn_notify && errorMessage)
            pfn_notify(errorMessage, nullptr, 0, user_data);
        if (errcode_ret)
            *errcode_ret = errorCode;
        return nullptr;
    };

    if (num_devices == 0)
    {
        return ReportError("num_devices must not be zero.", CL_INVALID_VALUE);
    }
    if (devices == nullptr)
    {
        return ReportError("devices must not be NULL.", CL_INVALID_VALUE);
    }
    if (pfn_notify == nullptr && user_data != nullptr)
    {
        return ReportError("user_data must be NULL if pfn_notify is NULL.", CL_INVALID_VALUE);
    }
    if (!ValidateContextProperties(properties, ReportError))
    {
        return nullptr;
    }

    Platform* platform = nullptr;
    auto platformProp = FindProperty<cl_context_properties>(properties, CL_CONTEXT_PLATFORM);
    if (platformProp)
    {
        platform = static_cast<Platform*>(reinterpret_cast<cl_platform_id>(*platformProp));
        if (!platform)
        {
            return ReportError("Platform specified but null.", CL_INVALID_PLATFORM);
        }
    }

    std::vector<D3DDeviceAndRef> device_refs;

    for (cl_uint i = 0; i < num_devices; ++i)
    {
        Device* device = static_cast<Device*>(devices[i]);
        if (!device->IsAvailable())
        {
            return ReportError("Device not available.", CL_DEVICE_NOT_AVAILABLE);
        }
        if (platform && platform != &device->m_Parent.get())
        {
            return ReportError("Platform specified in properties doesn't match device platform.", CL_INVALID_PLATFORM);
        }
        platform = &device->m_Parent.get();
        if (platform != g_Platform)
        {
            return ReportError("Invalid platform.", CL_INVALID_PLATFORM);
        }
        device_refs.emplace_back(std::make_pair(device, nullptr));
    }

    try
    {
        if (errcode_ret)
            *errcode_ret = CL_SUCCESS;
        return new Context(*platform, std::move(device_refs), properties, pfn_notify, user_data);
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_context CL_API_CALL
clCreateContextFromType(const cl_context_properties * properties,
    cl_device_type      device_type,
    Context::PfnCallbackType pfn_notify,
    void *              user_data,
    cl_int *            errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    auto ReportError = [&](const char* errorMessage, cl_int errorCode) -> cl_context
    {
        if (pfn_notify && errorMessage)
            pfn_notify(errorMessage, nullptr, 0, user_data);
        if (errcode_ret)
            *errcode_ret = errorCode;
        return nullptr;
    };

    if (pfn_notify == nullptr && user_data != nullptr)
    {
        return ReportError("user_data must be NULL if pfn_notify is NULL.", CL_INVALID_VALUE);
    }
    if (!ValidateContextProperties(properties, ReportError))
    {
        return nullptr;
    }
    if (device_type != CL_DEVICE_TYPE_GPU &&
        device_type != CL_DEVICE_TYPE_DEFAULT)
    {
        return ReportError("This platform only supports GPUs.", CL_INVALID_DEVICE_TYPE);
    }

    auto platformProp = FindProperty<cl_context_properties>(properties, CL_CONTEXT_PLATFORM);
    if (!platformProp)
    {
        return ReportError("Platform not provided.", CL_INVALID_PLATFORM);
    }
    Platform* platform = static_cast<Platform*>(reinterpret_cast<cl_platform_id>(*platformProp));
    if (!platform)
    {
        return ReportError("Platform specified but null.", CL_INVALID_PLATFORM);
    }
    if (platform != g_Platform)
    {
        return ReportError("Invalid platform.", CL_INVALID_PLATFORM);
    }

    std::vector<D3DDeviceAndRef> device_refs;

    for (cl_uint i = 0; i < platform->GetNumDevices(); ++i)
    {
        Device* device = static_cast<Device*>(platform->GetDevice(i));
        if (!device->IsAvailable())
        {
            return ReportError("Device not available.", CL_DEVICE_NOT_AVAILABLE);
        }
        device_refs.emplace_back(std::make_pair(device, nullptr));
    }

    try
    {
        if (errcode_ret)
            *errcode_ret = CL_SUCCESS;
        return new Context(*platform, std::move(device_refs), properties, pfn_notify, user_data);
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error &) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clRetainContext(cl_context context) CL_API_SUFFIX__VERSION_1_0
{
    if (!context)
    {
        return CL_INVALID_CONTEXT;
    }
    static_cast<Context*>(context)->Retain();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clReleaseContext(cl_context context) CL_API_SUFFIX__VERSION_1_0
{
    if (!context)
    {
        return CL_INVALID_CONTEXT;
    }
    static_cast<Context*>(context)->Release();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetContextInfo(cl_context         context_,
    cl_context_info    param_name,
    size_t             param_value_size,
    void *             param_value,
    size_t *           param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!context_)
    {
        return CL_INVALID_CONTEXT;
    }
    Context* context = static_cast<Context*>(context_);
    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };

    switch (param_name)
    {
    case CL_CONTEXT_REFERENCE_COUNT: return RetValue((cl_uint)context->GetRefCount());
    case CL_CONTEXT_NUM_DEVICES: return RetValue(context->GetDeviceCount());
    case CL_CONTEXT_DEVICES:
        return CopyOutParameterImpl(context->m_AssociatedDevices.data(),
            context->m_AssociatedDevices.size() * sizeof(context->m_AssociatedDevices[0]),
            param_value_size, param_value, param_value_size_ret);
    case CL_CONTEXT_PROPERTIES:
        return CopyOutParameterImpl(context->m_Properties.data(),
            context->m_Properties.size() * sizeof(context->m_Properties[0]),
            param_value_size, param_value, param_value_size_ret);
    }

    return context->GetErrorReporter()("Unknown param_name", CL_INVALID_VALUE);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clSetContextDestructorCallback(cl_context context_,
    void (CL_CALLBACK* pfn_notify)(cl_context context, void* user_data),
    void* user_data) CL_API_SUFFIX__VERSION_3_0
{
    if (!context_)
    {
        return CL_INVALID_CONTEXT;
    }
    if (!pfn_notify)
    {
        return CL_INVALID_VALUE;
    }
    static_cast<Context*>(context_)->AddDestructionCallback(pfn_notify, user_data);
    return CL_SUCCESS;
}

Context::Context(Platform& Platform, decltype(m_AssociatedDevices) Devices, const cl_context_properties* Properties, PfnCallbackType pfnErrorCb, void* CallbackContext)
    : CLChildBase(Platform)
    , m_AssociatedDevices(std::move(Devices))
    , m_ErrorCallback(pfnErrorCb ? pfnErrorCb : DummyCallback)
    , m_CallbackContext(CallbackContext)
    , m_Properties(PropertiesToVector(Properties))
{
    for (auto& [device, d3ddevice] : m_AssociatedDevices)
    {
        d3ddevice = &device->InitD3D();
    }
}

Context::~Context()
{
    for (auto iter = m_DestructorCallbacks.rbegin(); iter != m_DestructorCallbacks.rend(); ++iter)
    {
        auto& callback = *iter;
        callback.m_pfn(this, callback.m_userData);
    }

    for (auto& [device, _] : m_AssociatedDevices)
    {
        device->ReleaseD3D();
    }
}

void Context::ReportError(const char* Error)
{
    m_ErrorCallback(Error, nullptr, 0, m_CallbackContext);
}

cl_uint Context::GetDeviceCount() const noexcept
{
    return (cl_uint)m_AssociatedDevices.size();
}

Device& Context::GetDevice(cl_uint i) const noexcept
{
    assert(i < m_AssociatedDevices.size() && m_AssociatedDevices[i].first.Get());
    return *m_AssociatedDevices[i].first.Get();
}

D3DDevice& Context::GetD3DDevice(cl_uint i) const noexcept
{
    assert(i < m_AssociatedDevices.size() && m_AssociatedDevices[i].second);
    return *m_AssociatedDevices[i].second;
}

D3DDevice *Context::D3DDeviceForContext(Device& device) const noexcept
{
    auto iter = std::find_if(m_AssociatedDevices.begin(), m_AssociatedDevices.end(),
                             [&device](D3DDeviceAndRef const& d) { return d.first.Get() == &device; });
    return iter == m_AssociatedDevices.end() ? nullptr : iter->second;
}

void Context::AddDestructionCallback(DestructorCallback::Fn pfn, void *pUserData)
{
    std::lock_guard DestructorLock(m_DestructorLock);
    m_DestructorCallbacks.push_back({ pfn, pUserData });
}
