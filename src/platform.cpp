// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "platform.hpp"
#include "cache.hpp"
#include "compiler.hpp"

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
    if (platform != g_Platform)
    {
        return CL_INVALID_PLATFORM;
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
    else if (param_name == CL_PLATFORM_NUMERIC_VERSION)
    {
        return CopyOutParameter(
#ifdef CLON12_SUPPORT_3_0
            CL_MAKE_VERSION(3, 0, 0),
#else
            CL_MAKE_VERSION(1, 2, 0),
#endif
            param_value_size, param_value, param_value_size_ret);
    }
    else if (param_name == CL_PLATFORM_EXTENSIONS_WITH_VERSION)
    {
        constexpr cl_name_version extensions[] =
        {
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_icd" },
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_extended_versioning" },
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_global_int32_base_atomics" },
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_global_int32_extended_atomics" },
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_local_int32_base_atomics" },
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_local_int32_extended_atomics" },
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_byte_addressable_store" },
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_il_program" },
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_gl_sharing" },
            { CL_MAKE_VERSION(1, 0, 0), "cl_khr_gl_event" },
            // TODO: Maybe loop over devices to see if they're all GPUs?
            // { CL_MAKE_VERSION(1, 0, 0), "cl_khr_3d_image_writes" },
        };
        return CopyOutParameter(extensions, param_value_size, param_value, param_value_size_ret);
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

    char *forceWarpStr = nullptr;
    bool forceWarp = _dupenv_s(&forceWarpStr, nullptr, "CLON12_FORCE_WARP") == 0 &&
        forceWarpStr &&
        strcmp(forceWarpStr, "1") == 0;
    free(forceWarpStr);

    char *forceHardwareStr = nullptr;
    bool forceHardware = !forceWarp &&
        _dupenv_s(&forceHardwareStr, nullptr, "CLON12_FORCE_HARDWARE") == 0 &&
        forceHardwareStr &&
        strcmp(forceHardwareStr, "1") == 0;
    free(forceHardwareStr);

    if (forceWarp)
    {
        (void)std::remove_if(m_Devices.begin(), m_Devices.end(), [](std::unique_ptr<Device> const& a)
            {
                auto&& hwids = a->GetHardwareIds();
                return hwids.deviceID != 0x8c && hwids.vendorID != 0x1414;
            });
    }
    if (forceWarp || forceHardware)
    {
        m_Devices.resize(1);
    }
    m_Devices[0]->SetDefaultDevice();
}

Platform::~Platform() = default;

cl_uint Platform::GetNumDevices() const noexcept
{
    return (cl_uint)m_Devices.size();
}

Device *Platform::GetDevice(cl_uint i) const noexcept
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
    for (auto &device : m_Devices)
    {
        device->FlushAllDevices(Lock);
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

Compiler *Platform::GetCompiler()
{
    std::lock_guard lock(m_ModuleLock);
    if (!m_Compiler)
    {
        m_Compiler = Compiler::GetV2();
    }
    return m_Compiler.get();
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

void Platform::UnloadCompiler()
{
    // If we want to actually support unloading the compiler,
    // we'll need to track all live programs/kernels, because
    // they need to call back into the compiler to be able to
    // free their program memory.
}

bool Platform::AnyD3DDevicesExist() const noexcept
{
    return std::any_of(m_Devices.begin(), m_Devices.end(), 
                       [](std::unique_ptr<Device> const& dev) { return dev->HasD3DDevice(); });
}

void Platform::CloseCaches()
{
    for (auto& device : m_Devices)
    {
        device->CloseCaches();
    }
}
