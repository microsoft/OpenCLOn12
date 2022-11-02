// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "context.hpp"
#include "task.hpp"
#include "resources.hpp"
#include "queue.hpp"

#include <mesa_glinterop.h>
#include <d3d12_interop_public.h>

#include "gl_tokens.hpp"

class AcquireFromGLTask : public Task
{
public:
    AcquireFromGLTask(Context &Parent, cl_command_type command, cl_command_queue command_queue, std::vector<Resource::ref_ptr_int> resources, GLsync sync)
        : Task(Parent, command, command_queue)
        , m_Sync(sync)
        , m_Resources(std::move(resources))
    {
        if (!command_queue)
        {
            Submit();
            std::thread([ref_this = AcquireFromGLTask::ref_ptr_int(this)]()
                       {
                           ref_this->Record();
                           auto TaskPoolLock = g_Platform->GetTaskPoolLock();
                           static_cast<AcquireFromGLTask*>(ref_this.Get())->Complete(CL_SUCCESS, TaskPoolLock);
                       }).detach();
        }
    }

private:
    void RecordImpl() final
    {
        m_Parent->GetGLManager()->SyncWait(m_Sync, m_CommandType == CL_COMMAND_ACQUIRE_GL_OBJECTS);
    }
    void MigrateResources() final
    {
        for (auto &res : m_Resources)
        {
            res->EnqueueMigrateResource(&m_Parent->GetD3DDevice(0), this, 0);
        }
    }
    GLsync m_Sync;
    std::vector<Resource::ref_ptr_int> m_Resources;
};

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueAcquireGLObjects(cl_command_queue      command_queue,
                          cl_uint               num_objects,
                          const cl_mem *        mem_objects,
                          cl_uint               num_events_in_wait_list,
                          const cl_event *      event_wait_list,
                          cl_event *            event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (!context.GetGLManager())
    {
        return ReportError("Context was not created from a GL context", CL_INVALID_CONTEXT);
    }

    if ((event_wait_list == nullptr) != (num_events_in_wait_list == 0))
    {
        return ReportError("If event_wait_list is null, then num_events_in_wait_list must be zero, and vice versa.", CL_INVALID_EVENT_WAIT_LIST);
    }
    if ((mem_objects == nullptr) != (num_objects == 0))
    {
        return ReportError("If mem_objects is null, then num_objects must be zero, and vice versa.", CL_INVALID_VALUE);
    }

    try
    {
        std::vector<Resource::ref_ptr_int> resources;
        std::vector<mesa_glinterop_export_in> glResources;
        resources.reserve(num_objects);
        glResources.reserve(num_objects);
        for (cl_uint i = 0; i < num_objects; ++i)
        {
            if (!mem_objects[i])
            {
                return ReportError("Invalid memory object specific in mem_objects", CL_INVALID_MEM_OBJECT);
            }
            Resource &res = *static_cast<Resource *>(mem_objects[i]);
            if (!res.m_GLInfo)
            {
                return ReportError("A memory object was not created from a GL object", CL_INVALID_GL_OBJECT);
            }
            resources.emplace_back(&res);

            mesa_glinterop_export_in glResource = {};
            glResource.version = 1;
            glResource.target = res.m_GLInfo->TextureTarget;
            glResource.obj = res.m_GLInfo->ObjectName;
            glResources.push_back(glResource);
        }

        // The GL context must either be idle (glFinish), or be bound to this current thread as per cl_khr_gl_event
        // Either way, this will trigger a flush and return a sync object such that this CL event won't be satisfied
        // until after the GL commands from this context are complete.
        GLsync sync = {};
        context.GetGLManager()->AcquireResources(glResources, &sync);

        std::unique_ptr<Task> task(new AcquireFromGLTask(context, CL_COMMAND_ACQUIRE_GL_OBJECTS, command_queue, std::move(resources), sync));

        auto Lock = g_Platform->GetTaskPoolLock();
        if (num_events_in_wait_list)
        {
            task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        }

        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }

    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_event CL_API_CALL
clCreateEventFromGLsyncKHR(cl_context context_,
                           cl_GLsync  sync,
                           cl_int *   errcode_ret) CL_API_SUFFIX__VERSION_1_1
{
    if (!context_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return nullptr;
    }
    Context &context = *static_cast<Context *>(context_);
    auto ReportError = context.GetErrorReporter(errcode_ret);
    if (!context.GetGLManager())
    {
        return ReportError("Context was not created from a GL context", CL_INVALID_CONTEXT);
    }

    if (!sync)
    {
        return ReportError("Invalid sync", CL_INVALID_GL_OBJECT);
    }

    try
    {
        return new AcquireFromGLTask(context, CL_COMMAND_GL_FENCE_SYNC_OBJECT_KHR, nullptr, {}, sync);
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

class ReleaseToGLTask : public Task
{
public:
    ReleaseToGLTask(Context &Parent, cl_command_queue command_queue, std::vector<Resource::ref_ptr_int> resources)
        : Task(Parent, CL_COMMAND_RELEASE_GL_OBJECTS, command_queue)
        , m_Resources(std::move(resources))
    {
        auto glInterop = Parent.GetGLManager();
        if (glInterop->IsAppContextBoundToThread())
        {
            D3D12TranslationLayer::ThrowFailure(m_CommandQueue->GetD3DDevice().GetDevice()->
                                                CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_Fence.ReleaseAndGetAddressOf())));
            Parent.InsertGLWait(m_Fence.Get(), 1);
        }
    }

private:
    void RecordImpl() final
    {
        auto &ImmCtx = m_CommandQueue->GetD3DDevice().ImmCtx();
        for (auto &res : m_Resources)
        {
            ImmCtx.GetResourceStateManager().
                TransitionResource(res->GetActiveUnderlyingResource(),
                                   D3D12_RESOURCE_STATE_COMMON,
                                   D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS,
                                   D3D12TranslationLayer::SubresourceTransitionFlags::StateMatchExact |
                                       D3D12TranslationLayer::SubresourceTransitionFlags::ForceExclusiveState |
                                       D3D12TranslationLayer::SubresourceTransitionFlags::NotUsedInCommandListIfNoStateChange);
        }
        ImmCtx.GetResourceStateManager().ApplyAllResourceTransitions();
    }
    void MigrateResources() final
    {
        for (auto &res : m_Resources)
        {
            res->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
        }
    }
    void OnComplete() final
    {
        if (m_Fence)
        {
            m_Fence->Signal(1);
        }
    }
    std::vector<Resource::ref_ptr_int> m_Resources;
    ComPtr<ID3D12Fence> m_Fence;
};

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReleaseGLObjects(cl_command_queue      command_queue,
                          cl_uint               num_objects,
                          const cl_mem *        mem_objects,
                          cl_uint               num_events_in_wait_list,
                          const cl_event *      event_wait_list,
                          cl_event *            event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (!context.GetGLManager())
    {
        return ReportError("Context was not created from a GL context", CL_INVALID_CONTEXT);
    }

    if ((event_wait_list == nullptr) != (num_events_in_wait_list == 0))
    {
        return ReportError("If event_wait_list is null, then num_events_in_wait_list must be zero, and vice versa.", CL_INVALID_EVENT_WAIT_LIST);
    }
    if ((mem_objects == nullptr) != (num_objects == 0))
    {
        return ReportError("If mem_objects is null, then num_objects must be zero, and vice versa.", CL_INVALID_VALUE);
    }

    try
    {
        std::vector<Resource::ref_ptr_int> resources;
        resources.reserve(num_objects);
        for (cl_uint i = 0; i < num_objects; ++i)
        {
            if (!mem_objects[i])
            {
                return ReportError("Invalid memory object specific in mem_objects", CL_INVALID_MEM_OBJECT);
            }
            Resource &res = *static_cast<Resource *>(mem_objects[i]);
            if (!res.m_GLInfo)
            {
                return ReportError("A memory object was not created from a GL object", CL_INVALID_GL_OBJECT);
            }
            resources.emplace_back(&res);
        }

        std::unique_ptr<Task> task(new ReleaseToGLTask(context, command_queue, std::move(resources)));

        auto Lock = g_Platform->GetTaskPoolLock();
        if (num_events_in_wait_list)
        {
            task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        }

        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }

    return CL_SUCCESS;
}
