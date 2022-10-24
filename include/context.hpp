// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "platform.hpp"
#include "device.hpp"

class GLInteropManager;

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

    static void CL_CALLBACK DummyCallback(const char*, const void*, size_t, void*) {}

    friend cl_int CL_API_CALL clGetContextInfo(cl_context, cl_context_info, size_t, void*, size_t*);

public:
    Context(Platform& Platform, std::vector<D3DDeviceAndRef> Devices,
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
    std::vector<D3DDeviceAndRef> GetDevices() const noexcept { return m_AssociatedDevices; }

    void AddDestructionCallback(DestructorCallback::Fn pfn, void* pUserData);
};
