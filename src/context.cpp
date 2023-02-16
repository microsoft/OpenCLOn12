// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "context.hpp"

#include <mesa_glinterop.h>
#include <d3d12_interop_public.h>

#include "gl_tokens.hpp"

#include <wil/resource.h>

struct GLProperties
{
    EGLDisplay eglDisplay;
    EGLContext eglContext;
    HDC wglDisplay;
    HGLRC wglContext;
};

void GLInteropManager::PrepQueryDeviceInfo(mesa_glinterop_device_info &mesaDevInfo,
                                           d3d12_interop_device_info &d3d12DevInfo)
{
    mesaDevInfo.version = 2;
    mesaDevInfo.driver_data_size = sizeof(d3d12DevInfo);
    mesaDevInfo.driver_data = &d3d12DevInfo;
}
bool GLInteropManager::SyncWait(GLsync sync, bool deleteSync)
{
    if (!BindContext())
        return false;
    m_WaitSync(sync, 0, UINT64_MAX);
    if (deleteSync)
        m_DeleteSync(sync);
    UnbindContext();
    return true;
}

class WGLInteropManager : public GLInteropManager
{
public:
    virtual bool GetDeviceData(d3d12_interop_device_info &d3d12DevInfo) final
    {
        mesa_glinterop_device_info mesaDevInfo = {};
        PrepQueryDeviceInfo(mesaDevInfo, d3d12DevInfo);
        return m_QueryDeviceInfo(m_Display, m_AppContext, &mesaDevInfo) == MESA_GLINTEROP_SUCCESS;
    }
    virtual int GetResourceData(mesa_glinterop_export_in &in, mesa_glinterop_export_out &out) final
    {
        return m_ExportObject(m_Display, m_AppContext, &in, &out);
    }
    virtual bool AcquireResources(std::vector<mesa_glinterop_export_in> &resources, GLsync *sync) final
    {
        return m_FlushObjects(m_Display, m_AppContext, (unsigned)resources.size(), resources.data(), sync) == MESA_GLINTEROP_SUCCESS;
    }
    virtual bool IsAppContextBoundToThread() final
    {
        return m_GetCurrentContext() == m_AppContext;
    }

    virtual bool BindContext() final
    {
        auto hdc = wil::GetDC(m_HiddenWindow.get());
        return m_MakeCurrent(hdc.get(), m_MyContext.get());
    }
    virtual void UnbindContext() final
    {
        m_MakeCurrent(nullptr, nullptr);
    }

    ~WGLInteropManager() = default;

private:
    const HDC m_Display;
    const HGLRC m_AppContext;
    wil::unique_hwnd m_HiddenWindow;
    std::unique_ptr<std::remove_pointer_t<HGLRC>,
        decltype(&wglDeleteContext)> m_MyContext {nullptr, nullptr};
    decltype(&wglMakeCurrent) m_MakeCurrent;
    decltype(&wglGetCurrentContext) m_GetCurrentContext;
    decltype(&wglMesaGLInteropQueryDeviceInfo) m_QueryDeviceInfo;
    decltype(&wglMesaGLInteropExportObject) m_ExportObject;
    decltype(&wglMesaGLInteropFlushObjects) m_FlushObjects;

    friend class GLInteropManager;
    WGLInteropManager(GLProperties const &glProps)
        : GLInteropManager(XPlatHelpers::unique_module("opengl32.dll"))
        , m_Display(glProps.wglDisplay)
        , m_AppContext(glProps.wglContext)
    {
        auto getProcAddress = m_hMod.proc_address<decltype(&wglGetProcAddress)>("wglGetProcAddress");
        auto createContext = m_hMod.proc_address<decltype(&wglCreateContext)>("wglCreateContext");
        auto deleteContext = m_hMod.proc_address<decltype(&wglDeleteContext)>("wglDeleteContext");
        m_MakeCurrent = m_hMod.proc_address<decltype(&wglMakeCurrent)>("wglMakeCurrent");
        m_GetCurrentContext = m_hMod.proc_address<decltype(&wglGetCurrentContext)>("wglGetCurrentContext");
        if (!getProcAddress || !createContext || !deleteContext || !m_MakeCurrent || !m_GetCurrentContext)
        {
            throw std::runtime_error("Failed to get wglGetProcAddress");
        }

        static ATOM windowClass = 0;
        if (!windowClass)
        {
            WNDCLASSW classDesc = {};
            classDesc.lpfnWndProc = DefWindowProcW;
            classDesc.lpszClassName = L"CLOn12";
            windowClass = RegisterClassW(&classDesc);
        }
        m_HiddenWindow.reset(CreateWindowExW(0, L"CLOn12", L"CLOn12Window",
                                                0, 0, 0, 1, 1,
                                                nullptr, nullptr, nullptr, nullptr));
        if (!m_HiddenWindow.get())
        {
            throw std::runtime_error("Failed to create hidden window for binding context");
        }

        bool unbindContext = false;
        if (m_GetCurrentContext() == nullptr)
        {
            m_MyContext = { createContext(m_Display), deleteContext };
            if (!m_MyContext)
            {
                throw std::runtime_error("Failed to create temp WGL context");
            }

            auto hdc = wil::GetDC(m_HiddenWindow.get());
            if (!hdc.get())
            {
                throw std::runtime_error("Failed to get HDC for temp window");
            }
            int ipfd = GetPixelFormat(m_Display);
            if (ipfd <= 0)
            {
                throw std::runtime_error("Failed to get pixel format for app display");
            }
            PIXELFORMATDESCRIPTOR pfd;
            DescribePixelFormat(m_Display, ipfd, sizeof(pfd), &pfd);
            SetPixelFormat(hdc.get(), ipfd, &pfd);
            if (!m_MakeCurrent(hdc.get(), m_MyContext.get()))
            {
                throw std::runtime_error("Failed to make interop context current");
            }
            unbindContext = true;
        }

        m_QueryDeviceInfo = reinterpret_cast<decltype(m_QueryDeviceInfo)>(getProcAddress("wglMesaGLInteropQueryDeviceInfo"));
        m_ExportObject = reinterpret_cast<decltype(m_ExportObject)>(getProcAddress("wglMesaGLInteropExportObject"));
        m_FlushObjects = reinterpret_cast<decltype(m_FlushObjects)>(getProcAddress("wglMesaGLInteropFlushObjects"));
        m_WaitSync = reinterpret_cast<decltype(m_WaitSync)>(getProcAddress("glWaitSync"));
        m_DeleteSync = reinterpret_cast<decltype(m_DeleteSync)>(getProcAddress("glDeleteSync"));
        auto createContextAttrib = reinterpret_cast<decltype(&wglCreateContextAttribsARB)>(getProcAddress("wglCreateContextAttribsARB"));

        if (unbindContext)
        {
            m_MakeCurrent(nullptr, nullptr);
            m_MyContext.reset(nullptr);
        }

        if (!m_QueryDeviceInfo || !m_ExportObject || !m_FlushObjects || !m_WaitSync || !m_DeleteSync || !createContextAttrib)
        {
            throw std::runtime_error("Failed to get Mesa interop functions for WGL");
        }

        m_MyContext = { createContextAttrib(m_Display, m_AppContext, nullptr), deleteContext };
        if (!m_MyContext)
        {
            throw std::runtime_error("Failed to create WGL context");
        }
    }
};

class EGLInteropManager : public GLInteropManager
{
public:
    virtual bool GetDeviceData(d3d12_interop_device_info &d3d12DevInfo) final
    {
        mesa_glinterop_device_info mesaDevInfo = {};
        PrepQueryDeviceInfo(mesaDevInfo, d3d12DevInfo);
        return m_QueryDeviceInfo(m_Display, m_AppContext, &mesaDevInfo) == MESA_GLINTEROP_SUCCESS;
    }
    virtual int GetResourceData(mesa_glinterop_export_in &in, mesa_glinterop_export_out &out) final
    {
        return m_ExportObject(m_Display, m_AppContext, &in, &out);
    }
    virtual bool AcquireResources(std::vector<mesa_glinterop_export_in> &resources, GLsync *sync) final
    {
        return m_FlushObjects(m_Display, m_AppContext, (unsigned)resources.size(), resources.data(), sync) == MESA_GLINTEROP_SUCCESS;
    }
    virtual bool IsAppContextBoundToThread() final
    {
        return m_GetCurrentContext() == m_AppContext;
    }
    virtual bool BindContext() final
    {
        return m_MakeCurrent(m_Display, nullptr, nullptr, m_MyContext);
    }
    virtual void UnbindContext() final
    {
        m_MakeCurrent(m_Display, nullptr, nullptr, nullptr);
    }

    ~EGLInteropManager()
    {
        assert(m_MyContext);
        m_DestroyContext(m_Display, m_MyContext);
    }

private:
    const EGLDisplay m_Display;
    const EGLContext m_AppContext;
    EGLContext m_MyContext = nullptr;
    decltype(&MesaGLInteropEGLQueryDeviceInfo) m_QueryDeviceInfo;
    decltype(&MesaGLInteropEGLExportObject) m_ExportObject;
    decltype(&MesaGLInteropEGLFlushObjects) m_FlushObjects;
    decltype(&eglMakeCurrent) m_MakeCurrent;
    decltype(&eglDestroyContext) m_DestroyContext;
    decltype(&eglGetCurrentContext) m_GetCurrentContext;

    friend class GLInteropManager;
    EGLInteropManager(GLProperties const &glProps)
        : GLInteropManager(XPlatHelpers::unique_module("libEGL.dll"))
        , m_Display(glProps.eglDisplay)
        , m_AppContext(glProps.eglContext)
    {
        m_QueryDeviceInfo = m_hMod.proc_address<decltype(m_QueryDeviceInfo)>("MesaGLInteropEGLQueryDeviceInfo");
        m_ExportObject = m_hMod.proc_address<decltype(m_ExportObject)>("MesaGLInteropEGLExportObject");
        m_FlushObjects = m_hMod.proc_address<decltype(m_FlushObjects)>("MesaGLInteropEGLFlushObjects");
        m_MakeCurrent = m_hMod.proc_address<decltype(m_MakeCurrent)>("eglMakeCurrent");
        m_DestroyContext = m_hMod.proc_address<decltype(m_DestroyContext)>("eglDestroyContext");
        m_GetCurrentContext = m_hMod.proc_address<decltype(m_GetCurrentContext)>("eglGetCurrentContext");
        auto getProcAddress = m_hMod.proc_address<decltype(&eglGetProcAddress)>("eglGetProcAddress");
        auto createContext = m_hMod.proc_address<decltype(&eglCreateContext)>("eglCreateContext");
        if (!m_QueryDeviceInfo || !m_ExportObject || !m_FlushObjects || !m_MakeCurrent ||
            !m_DestroyContext || !m_GetCurrentContext || !getProcAddress || !createContext)
        {
            throw std::runtime_error("Failed to get Mesa interop functions for EGL");
        }

        m_WaitSync = reinterpret_cast<decltype(m_WaitSync)>(getProcAddress("glWaitSync"));
        m_DeleteSync = reinterpret_cast<decltype(m_DeleteSync)>(getProcAddress("glDeleteSync"));
        if (!m_WaitSync || !m_DeleteSync)
        {
            throw std::runtime_error("Failed to get Mesa interop functions for EGL");
        }

        m_MyContext = createContext(m_Display, nullptr, m_AppContext, nullptr);
        if (!m_MyContext)
        {
            throw std::runtime_error("Failed to create EGL context");
        }
    }
};

std::unique_ptr<GLInteropManager> GLInteropManager::Create(GLProperties const &glProps) try
{
    if (glProps.eglContext)
    {
        return std::unique_ptr<GLInteropManager>(new EGLInteropManager(glProps));
    }
    else if (glProps.wglContext)
    {
        return std::unique_ptr<GLInteropManager>(new WGLInteropManager(glProps));
    }
    return nullptr;
}
catch (...)
{
    return nullptr;
}

template <typename TReporter>
bool ValidateContextProperties(cl_context_properties const* properties, TReporter&& ReportError, GLProperties &glProps)
{
    constexpr cl_context_properties KnownProperties[] =
    {
        CL_CONTEXT_PLATFORM, CL_CONTEXT_INTEROP_USER_SYNC,
        CL_GL_CONTEXT_KHR, CL_EGL_DISPLAY_KHR, CL_GLX_DISPLAY_KHR,
        CL_WGL_HDC_KHR, CL_CGL_SHAREGROUP_KHR,
    };
    bool SeenProperties[std::extent_v<decltype(KnownProperties)>] = {};
    cl_context_properties glContext = 0;
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
        case CL_CONTEXT_PLATFORM:
            if (reinterpret_cast<Platform *>(*(CurProp + 1)) != g_Platform)
            {
                return !ReportError("Invalid platform.", CL_INVALID_PLATFORM);
            }
            break;
        case CL_GL_CONTEXT_KHR:
            glContext = *(CurProp + 1);
            break;
        case CL_EGL_DISPLAY_KHR:
            glProps.eglDisplay = reinterpret_cast<EGLDisplay>(*(CurProp + 1));
            break;
        case CL_WGL_HDC_KHR:
            glProps.wglDisplay = reinterpret_cast<HDC>(*(CurProp + 1));
            break;
        case CL_CGL_SHAREGROUP_KHR:
            return !ReportError("CGL unsupported.", CL_INVALID_OPERATION);
        case CL_GLX_DISPLAY_KHR:
            return !ReportError("GLX unsupported.", CL_INVALID_OPERATION);
        }
    }

    if (glContext && !(glProps.eglDisplay || glProps.wglDisplay))
    {
        return !ReportError("A GL context was provided, but no WGL or EGL display.", CL_INVALID_OPERATION);
    }
    if (!glContext && (glProps.eglDisplay || glProps.wglDisplay))
    {
        return !ReportError("A GL context was not provided, but a WGL or EGL display was.", CL_INVALID_OPERATION);
    }
    if (glProps.eglDisplay && glProps.wglDisplay)
    {
        return ReportError("If a GL context is provided, only one of WGL or EGL displays should be present.", CL_INVALID_OPERATION);
    }
    if (glProps.eglDisplay)
    {
        glProps.eglContext = reinterpret_cast<EGLContext>(glContext);
    }
    else if (glProps.wglDisplay)
    {
        glProps.wglContext = reinterpret_cast<HGLRC>(glContext);
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
    GLProperties glProps = {};
    if (!ValidateContextProperties(properties, ReportError, glProps))
    {
        return nullptr;
    }

    std::unique_ptr<GLInteropManager> glManager;
    d3d12_interop_device_info d3d12DevInfo = {};
    if (glProps.eglContext || glProps.wglContext)
    {
        glManager = GLInteropManager::Create(glProps);
        if (!glManager || !glManager->GetDeviceData(d3d12DevInfo))
        {
            return ReportError("Failed to retrieve GL interop data for provided GL context.", CL_INVALID_OPERATION);
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

        if (glManager)
        {
            LUID luid = device->GetAdapterLuid();
            if (memcmp(&luid, &d3d12DevInfo.adapter_luid, sizeof(luid)) != 0)
            {
                return ReportError("Device does not support interop with requested GL context.", CL_INVALID_OPERATION);
            }
        }
        device_refs.emplace_back(std::make_pair(device, nullptr));
    }

    try
    {
        if (errcode_ret)
            *errcode_ret = CL_SUCCESS;
        return new Context(std::move(device_refs), properties, std::move(glManager), pfn_notify, user_data);
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
    GLProperties glProps = {};
    if (!ValidateContextProperties(properties, ReportError, glProps))
    {
        return nullptr;
    }
    if (device_type == CL_DEVICE_TYPE_DEFAULT)
    {
        device_type = CL_DEVICE_TYPE_GPU;
    }

    std::unique_ptr<GLInteropManager> glManager;
    d3d12_interop_device_info d3d12DevInfo = {};
    if (glProps.eglContext || glProps.wglContext)
    {
        glManager = GLInteropManager::Create(glProps);
        if (!glManager || !glManager->GetDeviceData(d3d12DevInfo))
        {
            return ReportError("Failed to retrieve GL interop data for provided GL context.", CL_INVALID_OPERATION);
        }
    }

    std::vector<D3DDeviceAndRef> device_refs;

    for (cl_uint i = 0; i < g_Platform->GetNumDevices(); ++i)
    {
        Device* device = static_cast<Device*>(g_Platform->GetDevice(i));
        if (!(device->GetType() & device_type))
        {
            continue;
        }
        if (!device->IsAvailable())
        {
            return ReportError("Device not available.", CL_DEVICE_NOT_AVAILABLE);
        }
        if (glManager)
        {
            LUID luid = device->GetAdapterLuid();
            if (memcmp(&luid, &d3d12DevInfo.adapter_luid, sizeof(luid)) != 0)
            {
                return ReportError("Device does not support interop with requested GL context.", CL_INVALID_OPERATION);
            }
        }
        device_refs.emplace_back(std::make_pair(device, nullptr));
    }

    if (device_refs.size() == 0)
    {
        return ReportError("No devices found.", CL_DEVICE_NOT_FOUND);
    }

    try
    {
        if (errcode_ret)
            *errcode_ret = CL_SUCCESS;
        return new Context(std::move(device_refs), properties, std::move(glManager), pfn_notify, user_data);
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
    {
        size_t expectedSize = context->m_AssociatedDevices.size() * sizeof(cl_device_id);
        if (param_value_size && param_value_size < expectedSize)
        {
            return CL_INVALID_VALUE;
        }
        if (param_value_size)
        {
            std::transform(context->m_AssociatedDevices.begin(),
                           context->m_AssociatedDevices.end(),
                           static_cast<cl_device_id *>(param_value),
                           [](D3DDeviceAndRef const &dev) { return dev.first.Get(); });
        }
        if (param_value_size_ret)
        {
            *param_value_size_ret = expectedSize;
        }
        return CL_SUCCESS;
    }
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

extern CL_API_ENTRY cl_int CL_API_CALL
clGetGLContextInfoKHR(const cl_context_properties *properties,
                      cl_gl_context_info            param_name,
                      size_t                        param_value_size,
                      void *param_value,
                      size_t *param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!properties)
    {
        return CL_INVALID_PROPERTY;
    }
    GLProperties glProps = {};
    {
        cl_int ret = CL_SUCCESS;
        auto ReportError = [&ret](const char *, cl_int err) { ret = err; return nullptr; };
        if (!ValidateContextProperties(properties, ReportError, glProps))
        {
            return ret;
        }
    }
    auto glManager = GLInteropManager::Create(glProps);
    d3d12_interop_device_info d3d12DevInfo = {};
    if (!glManager || !glManager->GetDeviceData(d3d12DevInfo))
    {
        return CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR;
    }

    cl_device_id matchingDevice = nullptr;
    for (cl_uint i = 0; i < g_Platform->GetNumDevices(); ++i)
    {
        Device* device = static_cast<Device*>(g_Platform->GetDevice(i));
        LUID luid = device->GetAdapterLuid();
        if (device->IsAvailable() &&
            memcmp(&luid, &d3d12DevInfo.adapter_luid, sizeof(luid)) == 0)
        {
            matchingDevice = device;
            break;
        }
    }
    switch (param_name)
    {
    case CL_CURRENT_DEVICE_FOR_GL_CONTEXT_KHR:
    case CL_DEVICES_FOR_GL_CONTEXT_KHR:
        if (matchingDevice)
        {
            return CopyOutParameter(matchingDevice, param_value_size, param_value, param_value_size_ret);
        }
        return CopyOutParameter(nullptr, param_value_size, param_value, param_value_size_ret);
    default:
        return CL_INVALID_VALUE;
    }
}

Context::Context(std::vector<D3DDeviceAndRef> Devices,
                 const cl_context_properties* Properties,
                 std::unique_ptr<GLInteropManager> glManager,
                 PfnCallbackType pfnErrorCb, void* CallbackContext)
    : CLChildBase(*g_Platform)
    , m_AssociatedDevices(std::move(Devices))
    , m_ErrorCallback(pfnErrorCb ? pfnErrorCb : DummyCallback)
    , m_CallbackContext(CallbackContext)
    , m_Properties(PropertiesToVector(Properties))
    , m_GLInteropManager(std::move(glManager))
{
    for (auto& [device, d3ddevice] : m_AssociatedDevices)
    {
        d3d12_interop_device_info glInfo = {};
        if (m_GLInteropManager)
        {
            m_GLInteropManager->GetDeviceData(glInfo);
        }
        d3ddevice = &device->InitD3D(glInfo.device);
        m_GLCommandQueue = glInfo.queue;
    }
}

Context::~Context()
{
    for (auto iter = m_DestructorCallbacks.rbegin(); iter != m_DestructorCallbacks.rend(); ++iter)
    {
        auto& callback = *iter;
        callback.m_pfn(this, callback.m_userData);
    }

    for (auto& [device, d3dDevice] : m_AssociatedDevices)
    {
        device->ReleaseD3D(*d3dDevice);
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
