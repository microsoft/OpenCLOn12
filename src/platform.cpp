// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "platform.hpp"
#include "cache.hpp"
#include "clc_compiler.h"

CL_API_ENTRY cl_int CL_API_CALL
clGetPlatformInfo(cl_platform_id   platform,
    cl_platform_info param_name,
    size_t           param_value_size,
    void *           param_value,
    size_t *         param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (param_value_size == 0 && param_value != NULL)
    {
        return CL_INVALID_VALUE;
    }

    if (param_name == CL_PLATFORM_HOST_TIMER_RESOLUTION)
    {
        if (param_value_size && param_value_size < sizeof(cl_ulong))
        {
            return CL_INVALID_VALUE;
        }
        if (param_value_size)
        {
            LARGE_INTEGER TicksPerSecond;
            QueryPerformanceFrequency(&TicksPerSecond);
            *reinterpret_cast<cl_ulong*>(param_value) =
                1000000000 / TicksPerSecond.QuadPart;
        }
        if (param_value_size_ret)
        {
            *param_value_size_ret = sizeof(cl_ulong);
        }
        return CL_SUCCESS;
    }

    auto pPlatform = Platform::CastFrom(platform);
    auto pString = [pPlatform, param_name]() -> const char*
    {
        switch (param_name)
        {
        case CL_PLATFORM_PROFILE: return pPlatform->Profile;
        case CL_PLATFORM_VERSION: return pPlatform->Version;
        case CL_PLATFORM_NAME: return pPlatform->Name;
        case CL_PLATFORM_VENDOR: return pPlatform->Vendor;
        case CL_PLATFORM_EXTENSIONS: return pPlatform->Extensions;
        case CL_PLATFORM_ICD_SUFFIX_KHR: return pPlatform->ICDSuffix;
        }
        return nullptr;
    }();

    if (!pString)
    {
        return CL_INVALID_VALUE;
    }

    auto stringlen = strlen(pString) + 1;
    if (param_value_size && param_value_size < stringlen)
    {
        return CL_INVALID_VALUE;
    }
    if (param_value_size)
    {
        memcpy(param_value, pString, stringlen);
    }
    if (param_value_size_ret)
    {
        *param_value_size_ret = stringlen;
    }
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clUnloadPlatformCompiler(cl_platform_id platform) CL_API_SUFFIX__VERSION_1_2
{
    if (!platform)
    {
        return CL_INVALID_PLATFORM;
    }
    static_cast<Platform*>(platform)->UnloadCompiler();
    return CL_SUCCESS;
}

#include "device.hpp"
Platform::Platform(cl_icd_dispatch* dispatch)
{
    this->dispatch = dispatch;

    ComPtr<IDXCoreAdapterFactory> spFactory;
    THROW_IF_FAILED(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&spFactory)));

    THROW_IF_FAILED(spFactory->CreateAdapterList(1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE, IID_PPV_ARGS(&m_spAdapters)));

    m_Devices.resize(m_spAdapters->GetAdapterCount());
    for (cl_uint i = 0; i < m_Devices.size(); ++i)
    {
        ComPtr<IDXCoreAdapter> spAdapter;
        THROW_IF_FAILED(m_spAdapters->GetAdapter(i, IID_PPV_ARGS(&spAdapter)));
        m_Devices[i] = std::make_unique<Device>(*this, spAdapter.Get());
    }

    // TODO: Find some runtime way to enable this logic to force WARP
    //(void)std::remove_if(m_Devices.begin(), m_Devices.end(), [](std::unique_ptr<Device> const& a)
    //{
    //    auto&& hwids = a->GetHardwareIds();
    //    return hwids.deviceID != 0x8c && hwids.vendorID != 0x1414;
    //});
    //m_Devices.resize(1);
}

Platform::~Platform() = default;

cl_uint Platform::GetNumDevices() const noexcept
{
    return (cl_uint)m_Devices.size();
}

cl_device_id Platform::GetDevice(cl_uint i) const noexcept
{
    return m_Devices[i].get();
}

TaskPoolLock Platform::GetTaskPoolLock()
{
    TaskPoolLock lock;
    lock.m_Lock = std::unique_lock<std::recursive_mutex>{ m_TaskLock };
    return lock;
}

void Platform::FlushAllDevices(TaskPoolLock const& Lock)
{
    for (auto& device : m_Devices)
    {
        if (device->GetDevice())
        {
            device->Flush(Lock);
        }
    }
}

void Platform::DeviceInit()
{
    std::lock_guard Lock(m_ModuleLock);
    if (m_ActiveDeviceCount++ > 0)
    {
        return;
    }

    BackgroundTaskScheduler::SchedulingMode mode{ 1u, BackgroundTaskScheduler::Priority::Normal };
    m_CallbackScheduler.SetSchedulingMode(mode);

    mode.NumThreads = std::thread::hardware_concurrency();
    m_CompileAndLinkScheduler.SetSchedulingMode(mode);
}

void Platform::DeviceUninit()
{
    std::lock_guard Lock(m_ModuleLock);
    if (--m_ActiveDeviceCount > 0)
    {
        return;
    }

    BackgroundTaskScheduler::SchedulingMode mode{ 0u, BackgroundTaskScheduler::Priority::Normal };
    m_CallbackScheduler.SetSchedulingMode(mode);
    m_CompileAndLinkScheduler.SetSchedulingMode(mode);
}

#ifdef _WIN32
extern "C" extern IMAGE_DOS_HEADER __ImageBase;
#endif

void LoadFromNextToSelf(XPlatHelpers::unique_module& mod, const char* name)
{
#ifdef _WIN32
    char selfPath[MAX_PATH] = "";
    if (auto pathSize = GetModuleFileNameA((HINSTANCE)&__ImageBase, selfPath, sizeof(selfPath));
        pathSize == 0 || pathSize == sizeof(selfPath))
    {
        return;
    }

    auto lastSlash = strrchr(selfPath, '\\');
    if (!lastSlash)
    {
        return;
    }

    *(lastSlash + 1) = '\0';
    if (strcat_s(selfPath, name) != 0)
    {
        return;
    }

    mod.load(selfPath);
#endif
}

XPlatHelpers::unique_module const& Platform::GetCompiler()
{
    std::lock_guard lock(m_ModuleLock);
    if (!m_Compiler)
    {
        m_Compiler.load("CLGLOn12Compiler.dll");
    }
    if (!m_Compiler)
    {
        LoadFromNextToSelf(m_Compiler, "CLGLOn12Compiler.dll");
    }
    return m_Compiler;
}

XPlatHelpers::unique_module const& Platform::GetDXIL()
{
    std::lock_guard lock(m_ModuleLock);
    if (!m_DXIL)
    {
        m_DXIL.load("DXIL.dll");
    }
    if (!m_DXIL)
    {
        LoadFromNextToSelf(m_DXIL, "DXIL.dll");
    }
    return m_DXIL;
}

clc_context* Platform::GetCompilerContext(ShaderCache& cache)
{
    std::lock_guard lock(m_ModuleLock);
    if (!m_CompilerContext)
    {
        auto& compiler = GetCompiler();
        auto freeContext = compiler.proc_address<decltype(&clc_free_context)>("clc_free_context");

        // {1B9DC5F4-545A-4356-98D3-B4C0062E6253}
        static const GUID ClcContextKey =
        { 0x1b9dc5f4, 0x545a, 0x4356, { 0x98, 0xd3, 0xb4, 0xc0, 0x6, 0x2e, 0x62, 0x53 } };

        if (auto CachedContext = cache.Find(&ClcContextKey, sizeof(ClcContextKey)); 
            CachedContext.first)
        {
            auto deserializeContext = compiler.proc_address<decltype(&clc_context_deserialize)>("clc_context_deserialize");
            if (deserializeContext)
            {
                m_CompilerContext = decltype(m_CompilerContext)(deserializeContext(CachedContext.first.get(), CachedContext.second), freeContext);
                return m_CompilerContext.get();
            }
        }

        auto createContext = compiler.proc_address<decltype(&clc_context_new)>("clc_context_new");
        auto serialize = compiler.proc_address<decltype(&clc_context_serialize)>("clc_context_serialize");
        auto freeSerialized = compiler.proc_address<decltype(&clc_context_free_serialized)>("clc_context_free_serialized");
        clc_context_options options = {};
        options.optimize = cache.HasCache() && serialize && freeSerialized;
        m_CompilerContext = decltype(m_CompilerContext)(createContext(nullptr, &options), freeContext);

        if (options.optimize)
        {
            void* serialized = nullptr;
            size_t serializedSize = 0;
            serialize(m_CompilerContext.get(), &serialized, &serializedSize);

            if (serialized)
            {
                try
                {
                    cache.Store(&ClcContextKey, sizeof(ClcContextKey), serialized, serializedSize);
                }
                catch (...) {}
                freeSerialized(serialized);
            }
        }
    }
    return m_CompilerContext.get();
}

void Platform::UnloadCompiler()
{
    // If we want to actually support unloading the compiler,
    // we'll need to track all live programs/kernels, because
    // they need to call back into the compiler to be able to
    // free their program memory.
}

bool Platform::AnyD3DDevicesExist() const noexcept
{
    return std::any_of(m_Devices.begin(), m_Devices.end(), [](auto& dev) { return dev->GetDevice(); });
}

void Platform::CloseCaches()
{
    for (auto& device : m_Devices)
    {
        if (device->GetDevice())
        {
            device->GetShaderCache().Close();
        }
    }
}
