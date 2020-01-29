#pragma once
#include "platform.hpp"
#include <string>
#include <optional>
#include <mutex>

#include <Scheduler.hpp>

using ImmCtx = D3D12TranslationLayer::ImmediateContext;

struct TaskPoolLock
{
    std::unique_lock<std::recursive_mutex> m_Lock;
};

class Task;

using Submission = std::vector<Task*>;

class Device : public CLChildBase<Device, Platform, cl_device_id>
{
public:
    Device(Platform& parent, IDXCoreAdapter* pAdapter);
    ~Device();

    cl_bool IsAvailable() const noexcept;
    cl_ulong GetGlobalMemSize() const noexcept;
    DXCoreHardwareID const& GetHardwareIds() const noexcept;
    bool IsMCDM() const noexcept;

    std::string GetDeviceName() const;

    void InitD3D();
    ID3D12Device* GetDevice() const { return m_spDevice.Get(); }
    ImmCtx& ImmCtx() { return m_ImmCtx.value(); }
    UINT64 GetTimestampFrequency() const { return m_TimestampFrequency; }

    TaskPoolLock GetTaskPoolLock();
    void SubmitTask(Task*, TaskPoolLock const&);
    void ReadyTask(Task*, TaskPoolLock const&);
    void Flush(TaskPoolLock const&);

    template <typename Fn> void QueueCallback(Fn&& fn)
    {
        struct Context { Fn m_fn; };
        std::unique_ptr<Context> context(new Context{ std::forward<Fn>(fn) });
        m_CallbackScheduler.QueueTask({
            [](void* pContext)
            {
                std::unique_ptr<Context> context(static_cast<Context*>(pContext));
                context->m_fn();
            },
            [](void* pContext) { delete static_cast<Context*>(pContext); },
            context.get() });
        context.release();
    }
    template <typename Fn> void QueueProgramOp(Fn&& fn)
    {
        struct Context { Fn m_fn; };
        std::unique_ptr<Context> context(new Context{ std::forward<Fn>(fn) });
        m_CompileAndLinkScheduler.QueueTask({
            [](void* pContext)
            {
                std::unique_ptr<Context> context(static_cast<Context*>(pContext));
                context->m_fn();
            },
            [](void* pContext) { delete static_cast<Context*>(pContext); },
            context.get() });
        context.release();
    }

protected:
    void ExecuteTasks(Submission& tasks);

    ComPtr<IDXCoreAdapter> m_spAdapter;
    ComPtr<ID3D12Device> m_spDevice;
    DXCoreHardwareID m_HWIDs;

    // Lazy-initialized
    D3D12_FEATURE_DATA_D3D12_OPTIONS m_D3D12Options;
    D3D12TranslationLayer::TranslationLayerCallbacks m_Callbacks;
    std::optional<::ImmCtx> m_ImmCtx;

    std::recursive_mutex m_TaskLock;
    std::vector<::ref_ptr_int<Task>> m_TaskGraphScratch;

    std::unique_ptr<Submission> m_RecordingSubmission;

    BackgroundTaskScheduler::Scheduler m_CallbackScheduler;
    BackgroundTaskScheduler::Scheduler m_CompletionScheduler;
    BackgroundTaskScheduler::Scheduler m_CompileAndLinkScheduler;

    UINT64 m_TimestampFrequency = 0;
};
