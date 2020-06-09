// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "platform.hpp"
#include "device.hpp"

class Context : public CLChildBase<Context, Platform, cl_context>
{
public:
    using PfnCallbackType = void (CL_CALLBACK *)(const char * errinfo,
        const void * private_info,
        size_t       cb,
        void *       user_data);

private:
    Device::ref_int m_AssociatedDevice;
    const PfnCallbackType m_ErrorCallback;
    void* const m_CallbackContext;

    std::vector<cl_context_properties> const m_Properties;

    static void CL_CALLBACK DummyCallback(const char*, const void*, size_t, void*) {}

    friend cl_int CL_API_CALL clGetContextInfo(cl_context, cl_context_info, size_t, void*, size_t*);

public:
    Context(Platform& Platform, Device& Device, const cl_context_properties* Properties, PfnCallbackType pfnErrorCb, void* CallbackContext);
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

    Device& GetDevice() const noexcept;
};
