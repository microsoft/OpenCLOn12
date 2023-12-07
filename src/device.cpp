// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "device.hpp"
#include "task.hpp"
#include "queue.hpp"

#include <wil/resource.h>
#include <directx/d3d12compatibility.h>

extern CL_API_ENTRY cl_int CL_API_CALL
clGetDeviceIDs(cl_platform_id   platform,
    cl_device_type   device_type,
    cl_uint          num_entries,
    cl_device_id *   devices,
    cl_uint *        num_devices) CL_API_SUFFIX__VERSION_1_0
{
    if (!platform)
    {
        return CL_INVALID_PLATFORM;
    }

    if (num_entries && !devices)
    {
        return CL_INVALID_VALUE;
    }

    try
    {
        auto pPlatform = Platform::CastFrom(platform);
        cl_uint NumTotalDevices = pPlatform->GetNumDevices();
        cl_uint NumDevices = 0;
        for (cl_uint i = 0, output = 0; i < NumTotalDevices; ++i)
        {
            Device *device = pPlatform->GetDevice(i);
            if (device->GetType() & device_type)
            {
                NumDevices++;
                if (output < num_entries)
                {
                    devices[output++] = device;
                }
            }
        }
        if (num_devices)
        {
            *num_devices = NumDevices;
        }
    }
    catch (std::bad_alloc&) { return CL_OUT_OF_HOST_MEMORY; }
    catch (std::exception&) { return CL_OUT_OF_RESOURCES; }
    catch (_com_error&) { return CL_OUT_OF_RESOURCES; }

    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetDeviceInfo(cl_device_id    device,
    cl_device_info  param_name,
    size_t          param_value_size,
    void *          param_value,
    size_t *        param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!device)
    {
        return CL_INVALID_DEVICE;
    }

    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    auto AppendValue = [&](auto &&param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret, true);
    };
    auto pDevice = Device::CastFrom(device);
    auto ImageRetValue = [&](auto&& GPUValue, auto&& MCDMValue)
    {
        return RetValue(pDevice->IsMCDM() ? MCDMValue : GPUValue);
    };
    auto ImageRetValueOrZero = [&](auto GPUValue)
    {
        return RetValue(pDevice->IsMCDM() ? (decltype(GPUValue))0 : GPUValue);
    };
    try
    {
        switch (param_name)
        {
        case CL_DEVICE_TYPE: return RetValue(pDevice->GetType());
        case CL_DEVICE_VENDOR_ID: return RetValue(pDevice->GetHardwareIds().vendorID);
        case CL_DEVICE_MAX_COMPUTE_UNITS: return RetValue((cl_uint)1);
        case CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS: return RetValue((cl_uint)3);
        case CL_DEVICE_MAX_WORK_ITEM_SIZES:
        {
            constexpr size_t WorkItemSizes[3] =
            {
                D3D12_CS_THREAD_GROUP_MAX_X,
                D3D12_CS_THREAD_GROUP_MAX_Y,
                D3D12_CS_THREAD_GROUP_MAX_Z
            };
            return RetValue(WorkItemSizes);
        }
        case CL_DEVICE_MAX_WORK_GROUP_SIZE: return RetValue((size_t)D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP);

        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_CHAR: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_CHAR: return RetValue((cl_uint)16);

        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_HALF: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_HALF: // Fallthrough
        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_SHORT: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_SHORT: return RetValue((cl_uint)8);

        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_INT: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_INT: // Fallthrough
        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_FLOAT: return RetValue((cl_uint)4);

        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_LONG: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_LONG: // Fallthrough
        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_DOUBLE: return RetValue((cl_uint)2);

        case CL_DEVICE_MAX_CLOCK_FREQUENCY: return RetValue((cl_uint)12);
        case CL_DEVICE_ADDRESS_BITS: return RetValue(64u);
        case CL_DEVICE_MAX_MEM_ALLOC_SIZE: return RetValue(min((size_t)pDevice->GetGlobalMemSize() / 4, (size_t)1024 * 1024 * 1024));

        case CL_DEVICE_IMAGE_SUPPORT: return ImageRetValue((cl_bool)CL_TRUE, (cl_bool)CL_FALSE);
        case CL_DEVICE_MAX_READ_IMAGE_ARGS: /*SRVs*/ return ImageRetValueOrZero((cl_uint)128);
        case CL_DEVICE_MAX_WRITE_IMAGE_ARGS: /*UAVs*/return ImageRetValueOrZero((cl_uint)64);
        case CL_DEVICE_MAX_READ_WRITE_IMAGE_ARGS: /*Typed UAVs*/ return ImageRetValueOrZero((cl_uint)(pDevice->SupportsTypedUAVLoad() ? 64 : 0));

        case CL_DEVICE_IL_VERSION: return RetValue("SPIR-V_1.0 ");
        case CL_DEVICE_ILS_WITH_VERSION:
        {
            constexpr cl_name_version values[] = {
                { CL_MAKE_VERSION(1, 0, 0), "SPIR-V" },
            };
            return RetValue(values);
        }

        case CL_DEVICE_IMAGE2D_MAX_WIDTH: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        case CL_DEVICE_IMAGE2D_MAX_HEIGHT: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        case CL_DEVICE_IMAGE3D_MAX_WIDTH: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION);
        case CL_DEVICE_IMAGE3D_MAX_HEIGHT: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION);
        case CL_DEVICE_IMAGE3D_MAX_DEPTH: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION);
        case CL_DEVICE_IMAGE_MAX_BUFFER_SIZE: return ImageRetValueOrZero((size_t)(2 << D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP));
        case CL_DEVICE_IMAGE_MAX_ARRAY_SIZE: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
        case CL_DEVICE_MAX_SAMPLERS: return ImageRetValueOrZero((cl_uint)D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT);
        case CL_DEVICE_IMAGE_PITCH_ALIGNMENT: return ImageRetValueOrZero((cl_uint)0);
        case CL_DEVICE_IMAGE_BASE_ADDRESS_ALIGNMENT: return RetValue((cl_uint)0);

        case CL_DEVICE_MAX_PARAMETER_SIZE: return RetValue((size_t)1024);
        case CL_DEVICE_MEM_BASE_ADDR_ALIGN: return RetValue((cl_uint)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT * 8);
        case CL_DEVICE_MIN_DATA_TYPE_ALIGN_SIZE: return RetValue((cl_int)(64 * 16)); // sizeof(long16)

        case CL_DEVICE_SINGLE_FP_CONFIG: // Fallthrough
        {
            constexpr cl_device_fp_config fp_config =
                CL_FP_FMA | CL_FP_ROUND_TO_NEAREST | CL_FP_INF_NAN;
            return RetValue(fp_config);
        }
        case CL_DEVICE_DOUBLE_FP_CONFIG: return RetValue((cl_device_fp_config)0);

        case CL_DEVICE_GLOBAL_MEM_CACHE_TYPE: return RetValue((cl_device_mem_cache_type)CL_NONE);
        case CL_DEVICE_GLOBAL_MEM_CACHE_SIZE: return RetValue((cl_ulong)0);
        case CL_DEVICE_GLOBAL_MEM_CACHELINE_SIZE: return RetValue((cl_uint)0);

        case CL_DEVICE_GLOBAL_MEM_SIZE: return RetValue(pDevice->GetGlobalMemSize());

        case CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE: return RetValue((cl_ulong)(D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16));
        case CL_DEVICE_MAX_CONSTANT_ARGS: return RetValue((cl_uint)15);

        case CL_DEVICE_MAX_GLOBAL_VARIABLE_SIZE: return RetValue((size_t)0);
        case CL_DEVICE_GLOBAL_VARIABLE_PREFERRED_TOTAL_SIZE: return RetValue((size_t)0);

        case CL_DEVICE_LOCAL_MEM_TYPE: return RetValue((cl_device_local_mem_type)CL_LOCAL);
        case CL_DEVICE_LOCAL_MEM_SIZE: return RetValue((cl_ulong)(D3D12_CS_TGSM_REGISTER_COUNT * sizeof(UINT)));

        case CL_DEVICE_ERROR_CORRECTION_SUPPORT: return RetValue((cl_bool)CL_FALSE);
        case CL_DEVICE_PROFILING_TIMER_RESOLUTION: return RetValue((size_t)80);
        case CL_DEVICE_ENDIAN_LITTLE: return RetValue((cl_bool)CL_TRUE);

        case CL_DEVICE_AVAILABLE: return RetValue(pDevice->IsAvailable());
        case CL_DEVICE_COMPILER_AVAILABLE: return RetValue((cl_bool)CL_TRUE);
        case CL_DEVICE_LINKER_AVAILABLE: return RetValue((cl_bool)CL_TRUE);
        case CL_DEVICE_EXECUTION_CAPABILITIES: return RetValue((cl_device_exec_capabilities)CL_EXEC_KERNEL);

        case CL_DEVICE_QUEUE_ON_HOST_PROPERTIES: return RetValue(
            (cl_command_queue_properties)(CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE));
        case CL_DEVICE_QUEUE_ON_DEVICE_PROPERTIES: return RetValue((cl_command_queue_properties)0);
        case CL_DEVICE_QUEUE_ON_DEVICE_PREFERRED_SIZE: return RetValue((cl_uint)0);
        case CL_DEVICE_QUEUE_ON_DEVICE_MAX_SIZE: return RetValue((cl_uint)0);
        case CL_DEVICE_MAX_ON_DEVICE_QUEUES: return RetValue((cl_uint)0);
        case CL_DEVICE_MAX_ON_DEVICE_EVENTS: return RetValue((cl_uint)0);

        case CL_DEVICE_BUILT_IN_KERNELS: return RetValue("");
        case CL_DEVICE_BUILT_IN_KERNELS_WITH_VERSION: return RetValue(nullptr);
        case CL_DEVICE_PLATFORM: return RetValue(static_cast<cl_platform_id>(&pDevice->m_Parent.get()));
        case CL_DEVICE_NAME: return RetValue(pDevice->GetDeviceName().c_str());
        case CL_DEVICE_VENDOR: return RetValue(pDevice->m_Parent->Vendor);
        case CL_DRIVER_VERSION: return RetValue("1.1.0");
        case CL_DEVICE_PROFILE: return RetValue(pDevice->m_Parent->Profile);
        case CL_DEVICE_VERSION: return RetValue(pDevice->m_Parent->Version);
#ifdef CLON12_SUPPORT_3_0
        case CL_DEVICE_NUMERIC_VERSION: return RetValue(CL_MAKE_VERSION(3, 0, 0));
#endif
        case CL_DEVICE_OPENCL_C_VERSION: return RetValue("OpenCL C 1.2 ");
        case CL_DEVICE_OPENCL_C_ALL_VERSIONS:
        {
            constexpr cl_name_version versions[] =
            {
                { CL_MAKE_VERSION(1, 0, 0), "OpenCL C" },
                { CL_MAKE_VERSION(1, 1, 0), "OpenCL C" },
                { CL_MAKE_VERSION(1, 2, 0), "OpenCL C" },
#ifdef CLON12_SUPPORT_3_0
                { CL_MAKE_VERSION(3, 0, 0), "OpenCL C" },
#endif
            };
            return RetValue(versions);
        }

        case CL_DEVICE_EXTENSIONS:
        {
            constexpr char alwaysSupported[] =
                "cl_khr_global_int32_base_atomics "
                "cl_khr_global_int32_extended_atomics "
                "cl_khr_local_int32_base_atomics "
                "cl_khr_local_int32_extended_atomics "
                "cl_khr_byte_addressable_store "
                "cl_khr_il_program "
                "cl_khr_gl_sharing "
                "cl_khr_gl_event ";
            constexpr char imagesSupported[] = "cl_khr_3d_image_writes ";
            cl_int ret = RetValue(alwaysSupported);
            if (ret == CL_SUCCESS && !pDevice->IsMCDM())
                ret = AppendValue(imagesSupported);
            return ret;
        }
        case CL_DEVICE_EXTENSIONS_WITH_VERSION:
        {
            constexpr cl_name_version alwaysSupported[] = {
                { CL_MAKE_VERSION(1, 0, 0), "cl_khr_global_int32_base_atomics" },
                { CL_MAKE_VERSION(1, 0, 0), "cl_khr_global_int32_extended_atomics" },
                { CL_MAKE_VERSION(1, 0, 0), "cl_khr_local_int32_base_atomics" },
                { CL_MAKE_VERSION(1, 0, 0), "cl_khr_local_int32_extended_atomics" },
                { CL_MAKE_VERSION(1, 0, 0), "cl_khr_byte_addressable_store" },
                { CL_MAKE_VERSION(1, 0, 0), "cl_khr_il_program" },
                { CL_MAKE_VERSION(1, 0, 0), "cl_khr_gl_sharing" },
                { CL_MAKE_VERSION(1, 0, 0), "cl_khr_gl_event" },
            };
            constexpr cl_name_version imagesSupported[] = {
                { CL_MAKE_VERSION(1, 0, 0), "cl_khr_3d_image_writes" },
            };
            cl_int ret = RetValue(alwaysSupported);
            if (ret == CL_SUCCESS && !pDevice->IsMCDM())
                ret = AppendValue(imagesSupported);
            return ret;
        }
#ifdef CLON12_SUPPORT_3_0
        case CL_DEVICE_OPENCL_C_FEATURES:
        {
            constexpr cl_name_version alwaysSupported[] = {
                { CL_MAKE_VERSION(3, 0, 0), "__opencl_c_int64" },
            };
            constexpr cl_name_version imagesSupported[] = {
                { CL_MAKE_VERSION(3, 0, 0), "__opencl_c_images" },
                { CL_MAKE_VERSION(3, 0, 0), "__opencl_c_3d_image_writes" },
            };
            constexpr cl_name_version readWriteImagesSupported[] = {
                { CL_MAKE_VERSION(3, 0, 0), "__opencl_c_read_write_images" },
            };
            cl_int ret = RetValue(alwaysSupported);
            if (ret == CL_SUCCESS && !pDevice->IsMCDM())
                ret = AppendValue(imagesSupported);
            if (ret == CL_SUCCESS && pDevice->SupportsTypedUAVLoad())
                ret = AppendValue(readWriteImagesSupported);
            return ret;
        }
#endif

        case CL_DEVICE_PRINTF_BUFFER_SIZE: return RetValue((size_t)1024 * 1024);
        case CL_DEVICE_PREFERRED_INTEROP_USER_SYNC: return RetValue((cl_bool)CL_TRUE);

        case CL_DEVICE_PARENT_DEVICE: return RetValue((cl_device_id)nullptr);
        case CL_DEVICE_PARTITION_MAX_SUB_DEVICES: return RetValue((cl_uint)0);
        case CL_DEVICE_PARTITION_PROPERTIES: return RetValue(nullptr);
        case CL_DEVICE_PARTITION_AFFINITY_DOMAIN: return RetValue((cl_device_affinity_domain)0);
        case CL_DEVICE_PARTITION_TYPE: return CL_INVALID_VALUE;

        case CL_DEVICE_REFERENCE_COUNT: return RetValue((cl_uint)1);

        case CL_DEVICE_SVM_CAPABILITIES: return RetValue((cl_device_svm_capabilities)0);
        case CL_DEVICE_PREFERRED_PLATFORM_ATOMIC_ALIGNMENT: return RetValue((cl_uint)0);
        case CL_DEVICE_PREFERRED_GLOBAL_ATOMIC_ALIGNMENT: return RetValue((cl_uint)0);
        case CL_DEVICE_PREFERRED_LOCAL_ATOMIC_ALIGNMENT: return RetValue((cl_uint)0);

        case CL_DEVICE_MAX_NUM_SUB_GROUPS: return RetValue((cl_uint)0);
        case CL_DEVICE_SUB_GROUP_INDEPENDENT_FORWARD_PROGRESS: return RetValue((cl_bool)CL_FALSE);

        case CL_DEVICE_HOST_UNIFIED_MEMORY: return RetValue((cl_bool)pDevice->IsUMA());

        case CL_DEVICE_MAX_PIPE_ARGS: return RetValue((cl_uint)0);
        case CL_DEVICE_PIPE_MAX_ACTIVE_RESERVATIONS: return RetValue((cl_uint)0);
        case CL_DEVICE_PIPE_MAX_PACKET_SIZE: return RetValue((cl_uint)0);

        // Supporting more than these requires defining compiler feature macros
        case CL_DEVICE_ATOMIC_MEMORY_CAPABILITIES: return RetValue((cl_device_atomic_capabilities)(
            CL_DEVICE_ATOMIC_ORDER_RELAXED | CL_DEVICE_ATOMIC_SCOPE_WORK_GROUP));
        case CL_DEVICE_ATOMIC_FENCE_CAPABILITIES: return RetValue((cl_device_atomic_capabilities)(
            CL_DEVICE_ATOMIC_ORDER_RELAXED | CL_DEVICE_ATOMIC_ORDER_ACQ_REL | CL_DEVICE_ATOMIC_SCOPE_WORK_GROUP));

        case CL_DEVICE_NON_UNIFORM_WORK_GROUP_SUPPORT: return RetValue((cl_bool)CL_FALSE);
        case CL_DEVICE_WORK_GROUP_COLLECTIVE_FUNCTIONS_SUPPORT: return RetValue((cl_bool)CL_FALSE);
        case CL_DEVICE_GENERIC_ADDRESS_SPACE_SUPPORT: return RetValue((cl_bool)CL_FALSE);
        case CL_DEVICE_DEVICE_ENQUEUE_CAPABILITIES: return RetValue((cl_device_device_enqueue_capabilities)0);
        case CL_DEVICE_PIPE_SUPPORT: return RetValue((cl_bool)CL_FALSE);

        case CL_DEVICE_PREFERRED_WORK_GROUP_SIZE_MULTIPLE: return RetValue((size_t)64);

        case CL_DEVICE_LATEST_CONFORMANCE_VERSION_PASSED: return RetValue("");
        }

        return CL_INVALID_VALUE;
    }
    catch (_com_error &) { return CL_DEVICE_NOT_AVAILABLE; }
    catch (std::bad_alloc&) { return CL_OUT_OF_HOST_MEMORY; }
}

Device::Device(Platform& parent, IDXCoreAdapter* pAdapter)
    : CLChildBase(parent)
    , m_spAdapter(pAdapter)
{
    pAdapter->GetProperty(DXCoreAdapterProperty::HardwareID, sizeof(m_HWIDs), &m_HWIDs);
}

Device::~Device() = default;

static ImmCtx::CreationArgs GetImmCtxCreationArgs()
{
    ImmCtx::CreationArgs Args = {};
    Args.CreatesAndDestroysAreMultithreaded = true;
    Args.RenamingIsMultithreaded = true;
    Args.UseResidencyManagement = true;
    Args.UseThreadpoolForPSOCreates = true;
    Args.CreatorID = __uuidof(OpenCLOn12CreatorID);
    return Args;
}

static D3D12TranslationLayer::TranslationLayerCallbacks GetImmCtxCallbacks()
{
    D3D12TranslationLayer::TranslationLayerCallbacks Callbacks = {};
    Callbacks.m_pfnPostSubmit = []() {};
    return Callbacks;
}

D3DDevice::D3DDevice(Device &parent, ID3D12Device *pDevice, ID3D12CommandQueue *pQueue,
                     D3D12_FEATURE_DATA_D3D12_OPTIONS &options, bool IsImportedDevice)
    : m_IsImportedDevice(IsImportedDevice)
    , m_Parent(parent)
    , m_spDevice(pDevice)
    , m_Callbacks(GetImmCtxCallbacks())
    , m_ImmCtx(0, options, pDevice, pQueue, m_Callbacks, 0, GetImmCtxCreationArgs())
    , m_RecordingSubmission(new Submission)
    , m_ShaderCache(pDevice)
{
    BackgroundTaskScheduler::SchedulingMode mode{ 1u, BackgroundTaskScheduler::Priority::Normal };
    m_CompletionScheduler.SetSchedulingMode(mode);

    auto commandQueue = m_ImmCtx.GetCommandQueue(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS);
    (void)commandQueue->GetTimestampFrequency(&m_TimestampFrequency);

    UINT64 CPUTimestamp = 0, GPUTimestamp = 0;
    (void)commandQueue->GetClockCalibration(&GPUTimestamp, &CPUTimestamp);
    LARGE_INTEGER QPCFrequency = {};
    QueryPerformanceFrequency(&QPCFrequency);
    m_GPUToQPCTimestampOffset =
        (INT64)Task::TimestampToNanoseconds(CPUTimestamp, QPCFrequency.QuadPart) -
        (INT64)Task::TimestampToNanoseconds(GPUTimestamp, m_TimestampFrequency);
}

D3DDevice &Device::InitD3D(ID3D12Device *pDevice, ID3D12CommandQueue *pQueue)
{
    std::lock_guard Lock(m_InitLock);
    for (auto &dev : m_D3DDevices)
    {
        bool deviceAndQueueMatches = pDevice == dev->GetDevice() &&
            (!pQueue || pQueue == dev->ImmCtx().GetCommandQueue(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS));
        if ((pDevice && deviceAndQueueMatches) ||
            (!pDevice && !dev->m_IsImportedDevice))
        {
            ++dev->m_ContextCount;
            return *dev;
        }
    }

    ComPtr<ID3D12Device> spD3D12Device = pDevice;
    if (!pDevice)
    {
        THROW_IF_FAILED(D3D12CreateDevice(m_spAdapter.Get(), D3D_FEATURE_LEVEL_1_0_CORE, IID_PPV_ARGS(&spD3D12Device)));
    }
    CacheCaps(Lock, spD3D12Device);
    m_D3DDevices.emplace_back(nullptr);
    try
    {
        m_D3DDevices.back() = new D3DDevice(*this, spD3D12Device.Get(),
                                            pQueue, m_D3D12Options, pDevice != nullptr);
    }
    catch (...) { m_D3DDevices.pop_back(); throw; }

    g_Platform->DeviceInit();

    return *m_D3DDevices.back();
}

void Device::ReleaseD3D(D3DDevice &device)
{
    std::lock_guard Lock(m_InitLock);
    if (--device.m_ContextCount != 0)
        return;

    g_Platform->DeviceUninit();

    auto newEnd = std::remove_if(m_D3DDevices.begin(), m_D3DDevices.end(),
                                 [&device](D3DDevice *found) { return found == &device; });
    assert(std::distance(newEnd, m_D3DDevices.end()) == 1);
    delete m_D3DDevices.back();
    m_D3DDevices.pop_back();
}

cl_bool Device::IsAvailable() const noexcept
{
    bool driverUpdateInProgress = true;
    return SUCCEEDED(m_spAdapter->QueryState(DXCoreAdapterState::IsDriverUpdateInProgress,
        0, nullptr, sizeof(driverUpdateInProgress), &driverUpdateInProgress))
        && !driverUpdateInProgress;
}

cl_ulong Device::GetGlobalMemSize()
{
    // Just report one segment's worth of memory, depending on whether we're UMA or not.
    if (IsUMA())
    {
        uint64_t sharedMemory = 0;
        m_spAdapter->GetProperty(DXCoreAdapterProperty::SharedSystemMemory, sizeof(sharedMemory), &sharedMemory);
        return sharedMemory;
    }
    else
    {
        uint64_t localMemory = 0;
        m_spAdapter->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, sizeof(localMemory), &localMemory);
        return localMemory;
    }
}

DXCoreHardwareID const& Device::GetHardwareIds() const noexcept
{
    return m_HWIDs;
}

cl_device_type Device::GetType() const noexcept
{
    cl_device_type Default = m_DefaultDevice ? CL_DEVICE_TYPE_DEFAULT : 0;
    if (IsMCDM())
    {
        return CL_DEVICE_TYPE_ACCELERATOR | Default;
    }
    if (m_HWIDs.deviceID == 0x8c && m_HWIDs.vendorID == 0x1414)
    {
        return CL_DEVICE_TYPE_CPU | Default;
    }
    return CL_DEVICE_TYPE_GPU | Default;
}

bool Device::IsMCDM() const noexcept
{
    return !m_spAdapter->IsAttributeSupported(DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS);
}

bool Device::IsUMA()
{
    {
        std::lock_guard Lock(m_InitLock);
        CacheCaps(Lock);
    }
    return m_Architecture.UMA;
}

bool Device::SupportsInt16()
{
    {
        std::lock_guard Lock(m_InitLock);
        CacheCaps(Lock);
    }
    return m_D3D12Options4.Native16BitShaderOpsSupported;
}

bool Device::SupportsTypedUAVLoad()
{
    {
        std::lock_guard Lock(m_InitLock);
        CacheCaps(Lock);
    }
    return m_D3D12Options.TypedUAVLoadAdditionalFormats;
}

std::string Device::GetDeviceName() const
{
    std::string name;
    size_t nameSize = 0;
    if (SUCCEEDED(m_spAdapter->GetPropertySize(DXCoreAdapterProperty::DriverDescription, &nameSize)))
    {
        name.resize(nameSize);
        m_spAdapter->GetProperty(DXCoreAdapterProperty::DriverDescription, nameSize, name.data());
    }
    return name;
}

LUID Device::GetAdapterLuid() const
{
    LUID ret = {};
    m_spAdapter->GetProperty(DXCoreAdapterProperty::InstanceLuid, &ret);
    return ret;
}

void D3DDevice::SubmitTask(Task* task, TaskPoolLock const& lock)
{
    assert(task->m_CommandType != CL_COMMAND_USER);
    // User commands are treated as 'submitted' when they're created
    task->Submit();

    if (task->m_TasksToWaitOn.empty())
    {
        ReadyTask(task, lock);
    }
    else
    {
        for (auto& dependency : task->m_TasksToWaitOn)
        {
            if (dependency->GetState() == Task::State::Queued)
            {
                // It's impossible to have a task with a dependency on a task later on in the same queue.
                assert(dependency->m_CommandQueue != task->m_CommandQueue);

                // Ensure that any dependencies are also submitted. Notes:
                // - For recursive flushes, don't flush the overall device, we'll do it when we're done with all queues
                // - This might recurse back to the same queue... this is safe, because this task has already been removed
                //   from its own queue and had its state updated, so recursive flushes will pick up where we left off,
                //   and unwinding back will see that the flush has already been finished.
                dependency->m_CommandQueue->Flush(lock, /* flushDevice */ false);
            }
        }
    }
}

void D3DDevice::ReadyTask(Task* task, TaskPoolLock const& lock)
{
    assert(task->m_TasksToWaitOn.empty());

    task->MigrateResources();
    if (!task->m_TasksToWaitOn.empty() ||
        task->GetState() != Task::State::Submitted)
    {
        // Need to wait for resources to migrate.
        // Once the migration is done, this task will be readied for real
        return;
    }

    m_RecordingSubmission->push_back(task);
    task->Ready(lock);
}

void D3DDevice::Flush(TaskPoolLock const&)
{
    if (m_RecordingSubmission->empty())
    {
        return;
    }

    struct ExecutionHandler
    {
        D3DDevice& m_Device;
        std::unique_ptr<Submission> m_Tasks;
    };
    std::unique_ptr<ExecutionHandler> spHandler(new ExecutionHandler{ *this, std::move(m_RecordingSubmission) });

    m_CompletionScheduler.QueueTask({
        [](void* pContext)
        {
            std::unique_ptr<ExecutionHandler> spHandler(static_cast<ExecutionHandler*>(pContext));
            spHandler->m_Device.ExecuteTasks(*spHandler->m_Tasks);
        },
        [](void* pContext)
        {
            std::unique_ptr<ExecutionHandler> spHandler(static_cast<ExecutionHandler*>(pContext));
        },
        spHandler.get()
    });
    spHandler.release();

    m_RecordingSubmission.reset(new Submission);
}

void Device::FlushAllDevices(TaskPoolLock const& Lock)
{
    std::lock_guard InitLock(m_InitLock);
    for (auto &d3dDevice : m_D3DDevices)
    {
        d3dDevice->Flush(Lock);
    }
}

std::unique_ptr<D3D12TranslationLayer::PipelineState> D3DDevice::CreatePSO(D3D12TranslationLayer::COMPUTE_PIPELINE_STATE_DESC const& Desc)
{
    std::lock_guard PSOCreateLock(m_PSOCreateLock);
    return std::make_unique<D3D12TranslationLayer::PipelineState>(&ImmCtx(), Desc);
}

void D3DDevice::ExecuteTasks(Submission& tasks)
{
    for (cl_uint i = 0; i < tasks.size(); ++i)
    {
        try
        {
            auto& task = tasks[i];
            task->Record();
            auto Lock = g_Platform->GetTaskPoolLock();
            task->Started(Lock);
        }
        catch (...)
        {
            auto Lock = g_Platform->GetTaskPoolLock();
            if ((cl_int)tasks[i]->GetState() > 0)
            {
                tasks[i]->Complete(CL_OUT_OF_RESOURCES, Lock);
            }
            for (size_t j = i + 1; j < tasks.size(); ++j)
            {
                auto& task = tasks[j];
                task->Complete(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST, Lock);
            }
            tasks.erase(tasks.begin() + i, tasks.end());
        }
    }

    ImmCtx().WaitForCompletion(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS);

    {
        auto Lock = g_Platform->GetTaskPoolLock();
        for (auto& task : tasks)
        {
            task->Complete(CL_SUCCESS, Lock);
        }

        // Enqueue another execution task if there's new items ready to go
        g_Platform->FlushAllDevices(Lock);
    }
}

void Device::CacheCaps(std::lock_guard<std::mutex> const&, ComPtr<ID3D12Device> spDevice)
{
    if (m_CapsValid)
        return;

    if (!spDevice)
    {
        THROW_IF_FAILED(D3D12CreateDevice(m_spAdapter.Get(), D3D_FEATURE_LEVEL_1_0_CORE, IID_PPV_ARGS(&spDevice)));
    }
    spDevice->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &m_Architecture, sizeof(m_Architecture));
    spDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &m_D3D12Options, sizeof(m_D3D12Options));
    spDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &m_D3D12Options1, sizeof(m_D3D12Options1));
    spDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &m_D3D12Options4, sizeof(m_D3D12Options4));

    D3D_SHADER_MODEL SMTests[] = {
        D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0,
    };
    for (auto SM : SMTests)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL feature = { SM };
        if (SUCCEEDED(spDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &feature, sizeof(feature))))
        {
            m_ShaderModel = feature.HighestShaderModel;
            break;
        }
    }

    m_CapsValid = true;
}

void Device::CloseCaches()
{
    for (auto &dev : m_D3DDevices)
    {
        dev->GetShaderCache().Close();
    }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clRetainDevice(cl_device_id device) CL_API_SUFFIX__VERSION_1_2
{
    if (!device)
        return CL_INVALID_DEVICE;
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clReleaseDevice(cl_device_id device) CL_API_SUFFIX__VERSION_1_2
{
    if (!device)
        return CL_INVALID_DEVICE;
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetDeviceAndHostTimer(cl_device_id device_,
    cl_ulong*       device_timestamp,
    cl_ulong*       host_timestamp) CL_API_SUFFIX__VERSION_2_1
{
    if (!device_)
    {
        return CL_INVALID_DEVICE;
    }
    if (!device_timestamp || !host_timestamp)
    {
        return CL_INVALID_VALUE;
    }

    Device& device = *static_cast<Device*>(device_);
    try
    {
        // Should I just return 0 here if they haven't created a context on this device?
        auto& d3dDevice = device.InitD3D();
        auto cleanup = wil::scope_exit([&]() { device.ReleaseD3D(d3dDevice); });

        auto pQueue = d3dDevice.ImmCtx().GetCommandQueue(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS);
        D3D12TranslationLayer::ThrowFailure(pQueue->GetClockCalibration(device_timestamp, host_timestamp));
        return CL_SUCCESS;
    }
    catch (_com_error&) { return CL_OUT_OF_RESOURCES; }
    catch (std::bad_alloc&) { return CL_OUT_OF_HOST_MEMORY; }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetHostTimer(cl_device_id device,
    cl_ulong *   host_timestamp) CL_API_SUFFIX__VERSION_2_1
{
    if (!device)
    {
        return CL_INVALID_DEVICE;
    }
    if (!host_timestamp)
    {
        return CL_INVALID_VALUE;
    }
    LARGE_INTEGER QPC;
    QueryPerformanceCounter(&QPC);
    *host_timestamp = QPC.QuadPart;
    return CL_SUCCESS;
}
