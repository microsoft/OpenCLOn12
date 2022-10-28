// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "platform.hpp"
#include "device.hpp"
#include "gl_tokens.hpp"

struct GLProperties;
struct d3d12_interop_device_info;
struct mesa_glinterop_device_info;
struct mesa_glinterop_export_in;
struct mesa_glinterop_export_out;

class GLInteropManager
{
public:
    static std::unique_ptr<GLInteropManager> Create(GLProperties const &glProps);
    virtual ~GLInteropManager() = default;
    virtual bool GetDeviceData(d3d12_interop_device_info &d3d12DevInfo) = 0;
    virtual int GetResourceData(mesa_glinterop_export_in &in, mesa_glinterop_export_out &out) = 0;
    virtual bool AcquireResources(std::vector<mesa_glinterop_export_in> &resources, GLsync *sync) = 0;
    virtual bool IsAppContextBoundToThread() = 0;
    bool SyncWait(GLsync fence, bool deleteSync);
protected:
    void PrepQueryDeviceInfo(mesa_glinterop_device_info &mesaDevInfo,
                             d3d12_interop_device_info &d3d12DevInfo);
    virtual bool BindContext() = 0;
    virtual void UnbindContext() = 0;
    GLInteropManager(XPlatHelpers::unique_module mod)
        : m_hMod(std::move(mod))
    {
    }
    XPlatHelpers::unique_module m_hMod;
    decltype(&glWaitSync) m_WaitSync;
    decltype(&glDeleteSync) m_DeleteSync;
};

class Context : public CLChildBase<Context, Platform, cl_context>
{
public:
    using PfnCallbackType = void (CL_CALLBACK *)(const char * errinfo,
        const void * private_info,
        size_t       cb,
        void *       user_data);

    struct DestructorCallback
    {
        using Fn = void(CL_CALLBACK *)(cl_context, void*);
        Fn m_pfn;
        void* m_userData;
    };

private:
    std::vector<D3DDeviceAndRef> m_AssociatedDevices;
    const PfnCallbackType m_ErrorCallback;
    void* const m_CallbackContext;

    std::vector<cl_context_properties> const m_Properties;

    mutable std::mutex m_DestructorLock;
    std::vector<DestructorCallback> m_DestructorCallbacks;

    std::unique_ptr<GLInteropManager> m_GLInteropManager;
    ID3D12CommandQueue *m_GLCommandQueue = nullptr; // weak

    static void CL_CALLBACK DummyCallback(const char*, const void*, size_t, void*) {}

    friend cl_int CL_API_CALL clGetContextInfo(cl_context, cl_context_info, size_t, void*, size_t*);

public:
    Context(std::vector<D3DDeviceAndRef> Devices,
            const cl_context_properties* Properties,
            std::unique_ptr<GLInteropManager> glManager,
            PfnCallbackType pfnErrorCb, void* CallbackContext);
    ~Context();

    void ReportError(const char* Error);
    auto GetErrorReporter(cl_int* errcode_ret)
    {
        if (errcode_ret)
            *errcode_ret = CL_SUCCESS;
        return [=](const char* ErrorMsg, cl_int ErrorCode)
        {
            if (ErrorMsg)
                ReportError(ErrorMsg);
            if (errcode_ret)
                *errcode_ret = ErrorCode;
            return nullptr;
        };
    }
    auto GetErrorReporter()
    {
        return [this](const char* ErrorMsg, cl_int ErrorCode)
        {
            if (ErrorMsg)
                ReportError(ErrorMsg);
            return ErrorCode;
        };
    }

    cl_uint GetDeviceCount() const noexcept;
    Device& GetDevice(cl_uint index) const noexcept;
    D3DDevice &GetD3DDevice(cl_uint index) const noexcept;
    D3DDevice *D3DDeviceForContext(Device &device) const noexcept;
    GLInteropManager *GetGLManager() const noexcept { return m_GLInteropManager.get(); }
    void InsertGLWait(ID3D12Fence *fence, UINT64 value) const noexcept { m_GLCommandQueue->Wait(fence, value); }
    std::vector<D3DDeviceAndRef> GetDevices() const noexcept { return m_AssociatedDevices; }

    void AddDestructionCallback(DestructorCallback::Fn pfn, void* pUserData);
};
