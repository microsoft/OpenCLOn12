// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "device.hpp"
#include "task.hpp"

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
        cl_uint NumDevices = 0;
        if ((device_type & CL_DEVICE_TYPE_GPU) ||
            device_type == CL_DEVICE_TYPE_DEFAULT)
        {
            NumDevices += pPlatform->GetNumDevices();
        }

        if (num_devices)
        {
            *num_devices = NumDevices;
        }
        for (cl_uint i = 0; i < num_entries && i < NumDevices; ++i)
        {
            devices[i] = pPlatform->GetDevice(i);
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
        case CL_DEVICE_TYPE: return RetValue((cl_device_type)CL_DEVICE_TYPE_GPU);
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
        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_SHORT: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_SHORT: return RetValue((cl_uint)8);

        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_INT: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_INT: // Fallthrough
        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_LONG: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_LONG: // Fallthrough
        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_FLOAT: return RetValue((cl_uint)4);

        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_DOUBLE: return RetValue((cl_uint)2);
        case CL_DEVICE_PREFERRED_VECTOR_WIDTH_HALF: // Fallthrough
        case CL_DEVICE_NATIVE_VECTOR_WIDTH_HALF: return RetValue((cl_uint)8);

        case CL_DEVICE_MAX_CLOCK_FREQUENCY: return RetValue((cl_uint)12);
        case CL_DEVICE_ADDRESS_BITS: return RetValue(64u);
        case CL_DEVICE_MAX_MEM_ALLOC_SIZE: return RetValue(min((size_t)pDevice->GetGlobalMemSize() / 4, (size_t)1024 * 1024 * 1024));

        case CL_DEVICE_IMAGE_SUPPORT: return ImageRetValue((cl_bool)CL_TRUE, (cl_bool)CL_FALSE);
        case CL_DEVICE_MAX_READ_IMAGE_ARGS: /*SRVs*/ return ImageRetValueOrZero((cl_uint)128);
        case CL_DEVICE_MAX_WRITE_IMAGE_ARGS: // Fallthrough
        case CL_DEVICE_MAX_READ_WRITE_IMAGE_ARGS: /*UAVs*/ return ImageRetValueOrZero((cl_uint)64);

        case CL_DEVICE_IL_VERSION: return RetValue("");

        case CL_DEVICE_IMAGE2D_MAX_WIDTH: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        case CL_DEVICE_IMAGE2D_MAX_HEIGHT: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        case CL_DEVICE_IMAGE3D_MAX_WIDTH: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION);
        case CL_DEVICE_IMAGE3D_MAX_HEIGHT: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION);
        case CL_DEVICE_IMAGE3D_MAX_DEPTH: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION);
        case CL_DEVICE_IMAGE_MAX_BUFFER_SIZE: return ImageRetValueOrZero((size_t)(2 << D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP));
        case CL_DEVICE_IMAGE_MAX_ARRAY_SIZE: return ImageRetValueOrZero((size_t)D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
        case CL_DEVICE_MAX_SAMPLERS: return ImageRetValueOrZero((cl_uint)D3D12_COMMONSHADER_SAMPLER_SLOT_COUNT);
        case CL_DEVICE_IMAGE_PITCH_ALIGNMENT: return ImageRetValueOrZero((size_t)D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        case CL_DEVICE_IMAGE_BASE_ADDRESS_ALIGNMENT: return RetValue((cl_uint)D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

        case CL_DEVICE_MAX_PARAMETER_SIZE: return RetValue((size_t)1024);
        case CL_DEVICE_MEM_BASE_ADDR_ALIGN: return RetValue((cl_uint)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        case CL_DEVICE_SINGLE_FP_CONFIG: // Fallthrough
        case CL_DEVICE_DOUBLE_FP_CONFIG:
        {
            constexpr cl_device_fp_config fp_config =
                CL_FP_FMA | CL_FP_ROUND_TO_NEAREST | CL_FP_INF_NAN | CL_FP_DENORM;
            return RetValue(fp_config);
        }

        case CL_DEVICE_GLOBAL_MEM_CACHE_TYPE: return RetValue((cl_device_mem_cache_type)CL_NONE);
        case CL_DEVICE_GLOBAL_MEM_CACHE_SIZE: return RetValue((cl_ulong)0);
        case CL_DEVICE_GLOBAL_MEM_CACHELINE_SIZE: return RetValue((cl_uint)0);

        case CL_DEVICE_GLOBAL_MEM_SIZE: return RetValue(pDevice->GetGlobalMemSize());

        case CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE: return RetValue((cl_ulong)(D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16));
        case CL_DEVICE_MAX_CONSTANT_ARGS: return RetValue((cl_uint)(D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4));

        case CL_DEVICE_MAX_GLOBAL_VARIABLE_SIZE: return RetValue((size_t)65536);
        case CL_DEVICE_GLOBAL_VARIABLE_PREFERRED_TOTAL_SIZE: return RetValue((size_t)0);

        case CL_DEVICE_LOCAL_MEM_TYPE: return RetValue((cl_device_local_mem_type)CL_LOCAL);
        case CL_DEVICE_LOCAL_MEM_SIZE: return RetValue((cl_ulong)(D3D12_CS_TGSM_REGISTER_COUNT * sizeof(UINT)));

        case CL_DEVICE_ERROR_CORRECTION_SUPPORT: return RetValue((cl_bool)CL_FALSE);
        case CL_DEVICE_PROFILING_TIMER_RESOLUTION: return RetValue((cl_bool)80);
        case CL_DEVICE_ENDIAN_LITTLE: return RetValue((cl_bool)CL_TRUE);

        case CL_DEVICE_AVAILABLE: return RetValue(pDevice->IsAvailable());
        case CL_DEVICE_COMPILER_AVAILABLE: return RetValue((cl_bool)CL_TRUE);
        case CL_DEVICE_LINKER_AVAILABLE: return RetValue((cl_bool)CL_TRUE);
        case CL_DEVICE_EXECUTION_CAPABILITIES: return RetValue((cl_device_exec_capabilities)CL_EXEC_KERNEL);

        case CL_DEVICE_QUEUE_ON_HOST_PROPERTIES: return RetValue((cl_command_queue_properties)CL_QUEUE_PROFILING_ENABLE);
        case CL_DEVICE_QUEUE_ON_DEVICE_PROPERTIES: return RetValue(
            (cl_command_queue_properties)(CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE));
        case CL_DEVICE_QUEUE_ON_DEVICE_PREFERRED_SIZE: return RetValue((cl_uint)(16 * 1024));
        case CL_DEVICE_QUEUE_ON_DEVICE_MAX_SIZE: return RetValue((cl_uint)(256 * 1024));
        case CL_DEVICE_MAX_ON_DEVICE_QUEUES: return RetValue((cl_uint)1);
        case CL_DEVICE_MAX_ON_DEVICE_EVENTS: return RetValue(UINT_MAX);

        case CL_DEVICE_BUILT_IN_KERNELS: return RetValue("");
        case CL_DEVICE_PLATFORM: return RetValue(static_cast<cl_platform_id>(&pDevice->m_Parent.get()));
        case CL_DEVICE_NAME: return RetValue(pDevice->GetDeviceName().c_str());
        case CL_DEVICE_VENDOR: return RetValue(pDevice->m_Parent->Vendor);
        case CL_DRIVER_VERSION: return RetValue("0.0.1");
        case CL_DEVICE_PROFILE: return RetValue(pDevice->m_Parent->Profile);
        case CL_DEVICE_VERSION: return RetValue(pDevice->m_Parent->Version);
        case CL_DEVICE_OPENCL_C_VERSION: return RetValue("OpenCL C 1.2");

        case CL_DEVICE_EXTENSIONS: return RetValue("");

        case CL_DEVICE_PRINTF_BUFFER_SIZE: return RetValue((size_t)1024 * 1024);
        case CL_DEVICE_PREFERRED_INTEROP_USER_SYNC: return RetValue((cl_bool)CL_TRUE);

        case CL_DEVICE_PARENT_DEVICE: return RetValue((cl_device_id)nullptr);
        case CL_DEVICE_PARTITION_MAX_SUB_DEVICES: return RetValue((cl_uint)0);
        case CL_DEVICE_PARTITION_PROPERTIES: return RetValue((cl_uint)0);
        case CL_DEVICE_PARTITION_AFFINITY_DOMAIN: return RetValue((cl_uint)0);
        case CL_DEVICE_PARTITION_TYPE: return CL_INVALID_VALUE;

        case CL_DEVICE_REFERENCE_COUNT: return RetValue((cl_uint)1);

        case CL_DEVICE_SVM_CAPABILITIES: return RetValue((cl_device_svm_capabilities)0);
        case CL_DEVICE_PREFERRED_PLATFORM_ATOMIC_ALIGNMENT: return RetValue((cl_uint)0);
        case CL_DEVICE_PREFERRED_GLOBAL_ATOMIC_ALIGNMENT: return RetValue((cl_uint)0);
        case CL_DEVICE_PREFERRED_LOCAL_ATOMIC_ALIGNMENT: return RetValue((cl_uint)0);

        case CL_DEVICE_MAX_NUM_SUB_GROUPS: return RetValue((cl_uint)1);
        case CL_DEVICE_SUB_GROUP_INDEPENDENT_FORWARD_PROGRESS: return RetValue((cl_bool)CL_FALSE);

        case CL_DEVICE_HOST_UNIFIED_MEMORY: return RetValue((cl_bool)pDevice->IsUMA());
        }

        return CL_INVALID_VALUE;
    }
    catch (std::bad_alloc&) { return CL_OUT_OF_HOST_MEMORY; }
}

Device::Device(Platform& parent, IDXCoreAdapter* pAdapter)
    : CLChildBase(parent)
    , m_spAdapter(pAdapter)
{
    pAdapter->GetProperty(DXCoreAdapterProperty::HardwareID, sizeof(m_HWIDs), &m_HWIDs);
}

Device::~Device() = default;

void Device::InitD3D()
{
    std::lock_guard Lock(m_InitLock);
    ++m_ContextCount;
    if (m_ImmCtx)
    {
        return;
    }

    THROW_IF_FAILED(D3D12CreateDevice(m_spAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_spDevice)));
    //THROW_IF_FAILED(D3D12CreateDevice(m_spAdapter.Get(), D3D_FEATURE_LEVEL_1_0_CORE, IID_PPV_ARGS(&m_spDevice)));
    THROW_IF_FAILED(m_spDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &m_D3D12Options, sizeof(m_D3D12Options)));

    m_Callbacks.m_pfnPostSubmit = []() {};

    ImmCtx::CreationArgs Args = {};
    Args.CreatesAndDestroysAreMultithreaded = true;
    Args.RenamingIsMultithreaded = true;
    Args.UseResidencyManagement = true;
    Args.UseThreadpoolForPSOCreates = true;
    m_ImmCtx.emplace(0, m_D3D12Options, m_spDevice.Get(), nullptr, m_Callbacks, 0, Args);

    BackgroundTaskScheduler::SchedulingMode mode{ 1u, BackgroundTaskScheduler::Priority::Normal };
    m_CallbackScheduler.SetSchedulingMode(mode);
    m_CompletionScheduler.SetSchedulingMode(mode);

    mode.NumThreads = std::thread::hardware_concurrency();
    m_CompileAndLinkScheduler.SetSchedulingMode(mode);

    (void)m_ImmCtx->GetCommandQueue(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS)->GetTimestampFrequency(&m_TimestampFrequency);

    m_RecordingSubmission.reset(new Submission);
}

void Device::ReleaseD3D()
{
    std::lock_guard Lock(m_InitLock);
    if (--m_ContextCount != 0)
        return;

    m_ImmCtx.reset();
    BackgroundTaskScheduler::SchedulingMode mode{ 0u, BackgroundTaskScheduler::Priority::Normal };
    m_CallbackScheduler.SetSchedulingMode(mode);
    m_CompletionScheduler.SetSchedulingMode(mode);
    m_CompileAndLinkScheduler.SetSchedulingMode(mode);
    m_RecordingSubmission.reset();
    m_spDevice.Reset();
}

cl_bool Device::IsAvailable() const noexcept
{
    bool driverUpdateInProgress = true;
    return SUCCEEDED(m_spAdapter->QueryState(DXCoreAdapterState::IsDriverUpdateInProgress,
        0, nullptr, sizeof(driverUpdateInProgress), &driverUpdateInProgress))
        && !driverUpdateInProgress;
}

cl_ulong Device::GetGlobalMemSize() const noexcept
{
    size_t localMemory = 0;
    m_spAdapter->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, sizeof(localMemory), &localMemory);
    size_t nonlocalMemory = 0;
    m_spAdapter->GetProperty(DXCoreAdapterProperty::DedicatedSystemMemory, sizeof(nonlocalMemory), &nonlocalMemory);
    size_t sharedMemory = 0;
    m_spAdapter->GetProperty(DXCoreAdapterProperty::SharedSystemMemory, sizeof(sharedMemory), &sharedMemory);
    return ((cl_ulong)localMemory + nonlocalMemory + sharedMemory);
}

DXCoreHardwareID const& Device::GetHardwareIds() const noexcept
{
    return m_HWIDs;
}

bool Device::IsMCDM() const noexcept
{
    return !m_spAdapter->IsAttributeSupported(DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS);
}

bool Device::IsUMA() const noexcept
{
    if (!m_ImmCtx)
        return false;
    D3D12_FEATURE_DATA_ARCHITECTURE ArchCaps = {};
    GetDevice()->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &ArchCaps, sizeof(ArchCaps));
    return ArchCaps.UMA;
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

TaskPoolLock Device::GetTaskPoolLock()
{
    TaskPoolLock lock;
    lock.m_Lock = std::unique_lock<std::recursive_mutex>{m_TaskLock};
    return lock;
}

void Device::SubmitTask(Task* task, TaskPoolLock const& lock)
{
    if (task->m_CommandType != CL_COMMAND_USER)
    {
        // User commands are treated as 'submitted' when they're created
        task->Submit();
    }

    if (task->m_TasksToWaitOn.empty())
    {
        if (c_RecordCommandListsOnAppThreads)
        {
            m_TaskGraphScratch.emplace_back(task);
            for (cl_uint i = 0; i < m_TaskGraphScratch.size(); ++i)
            {
                task = m_TaskGraphScratch[i].Get();
                try
                {
                    ReadyTask(task, lock);
                    task->Record();
                }
                catch (...)
                {
                    // Everything else is an error now.
                    for (; i < m_TaskGraphScratch.size(); ++i)
                    {
                        m_TaskGraphScratch[i]->Complete(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST, lock);
                    }
                    break;
                }
            }
        }
        else
        {
            ReadyTask(task, lock);
        }
    }

    m_TaskGraphScratch.clear();
}

void Device::ReadyTask(Task* task, TaskPoolLock const& lock)
{
    m_RecordingSubmission->push_back(task);
    task->Ready(m_TaskGraphScratch, lock);
}

void Device::Flush(TaskPoolLock const& lock)
{
    if (m_RecordingSubmission->empty())
    {
        return;
    }

    if (c_RecordCommandListsOnAppThreads)
    {
        m_ImmCtx->Flush(D3D12TranslationLayer::COMMAND_LIST_TYPE_GRAPHICS_MASK);
        for (auto &task : *m_RecordingSubmission)
        {
            task->Started(lock);
        }

        struct CompletionHandler
        {
            Device &m_Device;
            UINT64 m_FenceValue;
            std::unique_ptr<Submission> m_Tasks;
        };
        UINT64 FenceValue = m_ImmCtx->GetCommandListID(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS) - 1;
        std::unique_ptr<CompletionHandler> spHandler(new CompletionHandler{ *this, FenceValue, std::move(m_RecordingSubmission) });

        m_CompletionScheduler.QueueTask({
            [](void *pContext)
            {
                std::unique_ptr<CompletionHandler> spHandler(static_cast<CompletionHandler *>(pContext));
                auto &CmdListManager = *spHandler->m_Device.m_ImmCtx->GetCommandListManager(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS);
                CmdListManager.WaitForFenceValue(spHandler->m_FenceValue);

                auto Lock = spHandler->m_Device.GetTaskPoolLock();
                for (auto &task : *spHandler->m_Tasks)
                {
                    task->Complete(CL_SUCCESS, Lock);
                }
            },
            [](void *pContext)
            {
                std::unique_ptr<CompletionHandler> spHandler(static_cast<CompletionHandler *>(pContext));
            },
            spHandler.get()
        });
        spHandler.release();
    }
    else
    {
        struct ExecutionHandler
        {
            Device& m_Device;
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
    }

    m_RecordingSubmission.reset(new Submission);
}

void Device::ExecuteTasks(Submission& tasks)
{
    {
        auto Lock = GetTaskPoolLock();
        for (cl_uint i = 0; i < tasks.size(); ++i)
        {
            try
            {
                auto& task = tasks[i];
                task->Record();
                task->Started(Lock);
            }
            catch (...)
            {
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

        ImmCtx().Flush(D3D12TranslationLayer::COMMAND_LIST_TYPE_GRAPHICS_MASK);
    }

    ImmCtx().WaitForCompletion(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS);

    {
        auto Lock = GetTaskPoolLock();
        for (auto& task : tasks)
        {
            task->Complete(CL_SUCCESS, Lock);
        }

        // Enqueue another execution task if there's new items ready to go
        Flush(Lock);
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
