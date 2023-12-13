// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "task.hpp"
#include "queue.hpp"
#include "resources.hpp"
#include "formats.hpp"
#include <variant>
#include <wil/resource.h>

#include "FormatDesc.hpp"
#include "ImmediateContext.inl"

using D3D12TranslationLayer::ImmediateContext;
using UpdateSubresourcesFlags = ImmediateContext::UpdateSubresourcesFlags;
using CPrepareUpdateSubresourcesHelper = ImmediateContext::CPrepareUpdateSubresourcesHelper;

template <typename ReportErrorT>
static cl_int ProcessImageDimensions(
    ReportErrorT&& ReportError,
    size_t const* origin,
    size_t const* region,
    Resource& resource,
    cl_ushort& FirstArraySlice,
    cl_ushort& NumArraySlices,
    cl_uchar& FirstMipLevel,
    cl_uint& Height, cl_uint& Depth,
    cl_uint& Y, cl_uint& Z)
{
    switch (resource.m_Desc.image_type)
    {
    default:
    case CL_MEM_OBJECT_BUFFER:
        return ReportError("image must be an image object.", CL_INVALID_MEM_OBJECT);

    case CL_MEM_OBJECT_IMAGE1D:
    case CL_MEM_OBJECT_IMAGE1D_BUFFER:
        if (origin[1] != 0 || origin[2] != 0 ||
            region[1] != 1 || region[2] != 1)
        {
            return ReportError("For 1D images, origin/region dimensions beyond the first must be 0/1 respectively.", CL_INVALID_VALUE);
        }
        break;

    case CL_MEM_OBJECT_IMAGE1D_ARRAY:
        if (origin[1] > resource.m_Desc.image_array_size ||
            region[1] > resource.m_Desc.image_array_size ||
            origin[1] + region[1] > resource.m_Desc.image_array_size)
        {
            return ReportError("For 1D image arrays, origin[1] and region[1] must be less than the image_array_size.", CL_INVALID_VALUE);
        }
        FirstArraySlice = (cl_ushort)origin[1];
        NumArraySlices = (cl_ushort)region[1];

        if (origin[2] != 0 || region[2] != 1)
        {
            return ReportError("For 1D image arrays, origin[2] must be 0 and region[2] must be 1.", CL_INVALID_VALUE);
        }
        break;

    case CL_MEM_OBJECT_IMAGE2D:
    case CL_MEM_OBJECT_IMAGE2D_ARRAY:
    case CL_MEM_OBJECT_IMAGE3D:
        if (origin[1] > resource.m_Desc.image_height ||
            region[1] > resource.m_Desc.image_height ||
            origin[1] + region[1] > resource.m_Desc.image_height)
        {
            return ReportError("For 2D and 3D images, origin[1] and region[1] must be less than the image_height.", CL_INVALID_VALUE);
        }
        Y = (cl_uint)origin[1];
        Height = (cl_uint)region[1];

        switch (resource.m_Desc.image_type)
        {
        case CL_MEM_OBJECT_IMAGE2D:
            if (origin[2] != 0 || region[2] != 1)
            {
                return ReportError("For 2D images, origin[2] must be 0 and region[2] must be 1.", CL_INVALID_VALUE);
            }
            break;
        case CL_MEM_OBJECT_IMAGE2D_ARRAY:
            if (origin[2] > resource.m_Desc.image_array_size ||
                region[2] > resource.m_Desc.image_array_size ||
                origin[2] + region[2] > resource.m_Desc.image_array_size)
            {
                return ReportError("For 2D image arrays, origin[2] and region[2] must be less than the image_array_size.", CL_INVALID_VALUE);
            }
            FirstArraySlice = (cl_ushort)origin[2];
            NumArraySlices = (cl_ushort)region[2];
            break;
        case CL_MEM_OBJECT_IMAGE3D:
            if (origin[2] > resource.m_Desc.image_depth ||
                region[2] > resource.m_Desc.image_depth ||
                origin[2] + region[2] > resource.m_Desc.image_depth)
            {
                return ReportError("For 3D images, origin[2] and region[2] must be less than the image_depth.", CL_INVALID_VALUE);
            }
            Z = (cl_uint)origin[2];
            Depth = (cl_uint)region[2];
            break;
        }
        break;
    }
    if (resource.m_GLInfo)
    {
        FirstArraySlice += (cl_ushort)resource.m_GLInfo->BaseArray;
        FirstMipLevel = (cl_uchar)resource.m_GLInfo->MipLevel;
    }
    return CL_SUCCESS;
}

class MemWriteFillTask : public Task
{
public:
    struct FillData
    {
        char Pattern[128];
        cl_uint PatternSize;
    };
    struct WriteData
    {
        const void *pData;
        cl_uint RowPitch;
        cl_uint SlicePitch;
    };
    struct Args
    {
        cl_uint DstX;
        cl_uint DstY;
        cl_uint DstZ;
        cl_uint Width;
        cl_uint Height;
        cl_uint Depth;
        cl_ushort FirstArraySlice;
        cl_ushort NumArraySlices;
        cl_uchar FirstMipLevel;
        std::variant<WriteData, FillData> Data;
        cl_uint SrcX;
        cl_uint SrcY;
        cl_uint SrcZ;
        cl_uint DstBufferRowPitch;
        cl_uint DstBufferSlicePitch;
    };


    MemWriteFillTask(Context& Parent, Resource& Target, cl_command_type CommandType,
        cl_command_queue CommandQueue, Args const& args, bool DeferCopy);

private:
    Resource::ref_ptr_int m_Target;
    const Args m_Args;

    void CopyFromHostPtr(UpdateSubresourcesFlags);
    std::vector<CPrepareUpdateSubresourcesHelper> m_Helpers;

    void MigrateResources() final
    {
        m_Target->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
    }
    void RecordImpl() final;
    void OnComplete() final
    {
        m_Target.Release();
    }
};

MemWriteFillTask::MemWriteFillTask(Context &Parent, Resource &Target,
    cl_command_type CommandType, cl_command_queue CommandQueue,
    Args const& args, bool DeferCopy)
    : Task(Parent, CommandType, CommandQueue)
    , m_Target(&Target)
    , m_Args(args)
{
    if (!DeferCopy)
    {
        CopyFromHostPtr(UpdateSubresourcesFlags::ScenarioBatchedContext);
    }
}

void MemWriteFillTask::CopyFromHostPtr(UpdateSubresourcesFlags flags)
{
    // For buffer rects, have to use row-by-row copies if the pitches don't align to
    // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT.
    // TODO: Add a path that uses CopyTextureRegion if it does align.
    
    const bool bIsRowByRowCopy = m_Target->m_Desc.image_type == CL_MEM_OBJECT_BUFFER;
    UINT NumRowCopies = bIsRowByRowCopy ? m_Args.Height : 1;
    UINT NumSliceCopies = bIsRowByRowCopy ? m_Args.Depth : 1;

    D3D12TranslationLayer::CSubresourceSubset subresources =
        m_Target->GetUnderlyingResource(&m_CommandQueue->GetD3DDevice())->GetFullSubresourceSubset();
    const cl_uint FormatBytes = GetFormatSizeBytes(m_Target->m_Format);
    for (UINT16 i = 0; i < m_Args.NumArraySlices; ++i)
    {
        subresources.m_BeginArray = (UINT16)
            ((m_Args.FirstArraySlice + i) * m_Target->m_CreationArgs.m_desc12.MipLevels +
              m_Args.FirstMipLevel);
        subresources.m_EndArray = subresources.m_BeginArray + 1;

        for (UINT z = 0; z < NumSliceCopies; ++z)
        {
            for (UINT y = 0; y < NumRowCopies; ++y)
            {
                D3D11_SUBRESOURCE_DATA UploadData;
                auto pData = &UploadData;
                const void *pPattern = nullptr;
                UINT PatternSize = 0;

                if (m_Args.Data.index() == 0)
                {
                    WriteData const &WriteArgs = std::get<0>(m_Args.Data);

                    const char* pSubresourceData = reinterpret_cast<const char*>(WriteArgs.pData);
                    pSubresourceData += (i + z + m_Args.SrcZ) * WriteArgs.SlicePitch;
                    pSubresourceData += (y + m_Args.SrcY) * WriteArgs.RowPitch;
                    pSubresourceData += FormatBytes * m_Args.SrcX;

                    UploadData.pSysMem = pSubresourceData;
                    UploadData.SysMemPitch = WriteArgs.RowPitch;
                    UploadData.SysMemSlicePitch = WriteArgs.SlicePitch;
                }
                else
                {
                    FillData const& FillArgs = std::get<1>(m_Args.Data);
                    pData = nullptr;
                    pPattern = FillArgs.Pattern;
                    PatternSize = FillArgs.PatternSize;
                }

                D3D12_BOX DstBox =
                {
                    m_Args.DstX, m_Args.DstY, m_Args.DstZ,
                    m_Args.DstX + m_Args.Width,
                    m_Args.DstY + m_Args.Height,
                    m_Args.DstZ + m_Args.Depth
                };
                if (bIsRowByRowCopy)
                {
                    DstBox = { 0, 0, 0, 1, 1, 1 };
                    DstBox.left = (UINT)(m_Target->m_Offset + // Buffer suballocation offset
                        ((z + m_Args.DstZ) * m_Args.DstBufferSlicePitch) + // Slice offset
                        ((y + m_Args.DstY) * m_Args.DstBufferRowPitch) + // Row offset
                        m_Args.DstX); // Offset within row
                    DstBox.right = DstBox.left + m_Args.Width;
                }
                m_Helpers.emplace_back(
                    *m_Target->GetUnderlyingResource(&m_CommandQueue->GetD3DDevice()),
                    subresources,
                    pData,
                    &DstBox,
                    flags,
                    pPattern,
                    PatternSize,
                    m_CommandQueue->GetD3DDevice().ImmCtx());
            }
        }

    }
}

void MemWriteFillTask::RecordImpl()
{
    if (m_Helpers.empty())
    {
        CopyFromHostPtr(UpdateSubresourcesFlags::ScenarioImmediateContext);
    }

    for (auto& Helper : m_Helpers)
    {
        if (Helper.FinalizeNeeded)
        {
            m_CommandQueue->GetD3DDevice().ImmCtx().FinalizeUpdateSubresources(
                &Helper.Dst, Helper.PreparedStorage.Base, Helper.PreparedStorage.LocalPlacementDescs);
        }
    }
}

cl_int clEnqueueWriteBufferRectImpl(cl_command_queue    command_queue,
    cl_mem              buffer,
    cl_bool             blocking_write,
    const size_t* buffer_offset,
    const size_t* host_offset,
    const size_t* region,
    size_t              buffer_row_pitch,
    size_t              buffer_slice_pitch,
    size_t              host_row_pitch,
    size_t              host_slice_pitch,
    const void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event,
    cl_command_type command_type
)
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!buffer)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(buffer);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (resource.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("buffer must be a buffer object.", CL_INVALID_MEM_OBJECT);
    }

    if (buffer_offset[0] > resource.m_Desc.image_width ||
        region[0] > resource.m_Desc.image_width ||
        buffer_offset[0] + region[0] > resource.m_Desc.image_width)
    {
        return ReportError("Offsets/regions too large.", CL_INVALID_VALUE);
    }

    if (buffer_row_pitch == 0)
    {
        buffer_row_pitch = region[0];
    }
    else if (buffer_row_pitch > resource.m_Desc.image_width ||
             buffer_row_pitch < region[0])
    {
        return ReportError("buffer_row_pitch must be 0 or between region[0] and the buffer size.", CL_INVALID_VALUE);
    }
    
    if (host_row_pitch == 0)
    {
        host_row_pitch = region[0];
    }
    else if (host_row_pitch < region[0])
    {
        return ReportError("host_row_pitch must be 0 or greater than region[0].", CL_INVALID_VALUE);
    }

    size_t SliceSizeInBytes = (buffer_offset[1] + region[1] - 1) * buffer_row_pitch + buffer_offset[0] + region[0];
    if (SliceSizeInBytes > resource.m_Desc.image_width)
    {
        return ReportError("Offsets/regions too large.", CL_INVALID_VALUE);
    }

    size_t ReqBufferSlicePitch = buffer_row_pitch * region[1];
    size_t ReqHostSlicePitch = host_row_pitch * region[1];
    if (buffer_slice_pitch == 0)
    {
        buffer_slice_pitch = ReqBufferSlicePitch;
    }
    else if (buffer_slice_pitch > resource.m_Desc.image_width ||
             buffer_slice_pitch < ReqBufferSlicePitch)
    {
        return ReportError("buffer_slice_pitch must be 0 or between (region[0] * buffer_row_pitch) and the buffer size.", CL_INVALID_VALUE);
    }

    if (host_slice_pitch == 0)
    {
        host_slice_pitch = ReqHostSlicePitch;
    }
    else if (host_slice_pitch < ReqHostSlicePitch)
    {
        return ReportError("host_slice_pitch must be 0 or greater than (region[0] * buffer_row_pitch).", CL_INVALID_VALUE);
    }

    size_t ResourceSizeInBytes = (buffer_offset[2] + region[2] - 1) * buffer_slice_pitch + SliceSizeInBytes;
    if (ResourceSizeInBytes > resource.m_Desc.image_width)
    {
        return ReportError("Offsets/regions too large.", CL_INVALID_VALUE);
    }

    if (resource.m_Flags & (CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS))
    {
        return ReportError("Buffer is not writable from the host.", CL_INVALID_OPERATION);
    }

    if (!ptr)
    {
        return ReportError("ptr must not be null.", CL_INVALID_VALUE);
    }

    MemWriteFillTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)buffer_offset[0];
    CmdArgs.DstY = (cl_uint)buffer_offset[1];
    CmdArgs.DstZ = (cl_uint)buffer_offset[2];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = (cl_uint)region[1];
    CmdArgs.Depth = (cl_uint)region[2];
    CmdArgs.SrcX = (cl_uint)host_offset[0];
    CmdArgs.SrcY = (cl_uint)host_offset[1];
    CmdArgs.SrcZ = (cl_uint)host_offset[2];
    CmdArgs.NumArraySlices = 1;
    CmdArgs.DstBufferRowPitch = (cl_uint)buffer_row_pitch;
    CmdArgs.DstBufferSlicePitch = (cl_uint)buffer_slice_pitch;
    CmdArgs.Data = MemWriteFillTask::WriteData
    {
        ptr, (cl_uint)host_row_pitch, (cl_uint)host_slice_pitch
    };

    try
    {
        std::unique_ptr<Task> task(new MemWriteFillTask(context, resource, command_type, command_queue, CmdArgs, blocking_write == CL_FALSE));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
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

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWriteBuffer(cl_command_queue   command_queue,
    cl_mem             buffer,
    cl_bool            blocking_write,
    size_t             offset,
    size_t             size,
    const void* ptr,
    cl_uint            num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    size_t buffer_offset[3] = { offset, 0, 0 };
    size_t host_offset[3] = { 0, 0, 0 };
    size_t region[3] = { size, 1, 1 };
    return clEnqueueWriteBufferRectImpl(
        command_queue,
        buffer,
        blocking_write,
        buffer_offset,
        host_offset,
        region,
        0,
        0,
        0,
        0,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
        CL_COMMAND_COPY_BUFFER
    );
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWriteBufferRect(cl_command_queue    command_queue,
    cl_mem              buffer,
    cl_bool             blocking_write,
    const size_t* buffer_offset,
    const size_t* host_offset,
    const size_t* region,
    size_t              buffer_row_pitch,
    size_t              buffer_slice_pitch,
    size_t              host_row_pitch,
    size_t              host_slice_pitch,
    const void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_1
{
    return clEnqueueWriteBufferRectImpl(
        command_queue,
        buffer,
        blocking_write,
        buffer_offset,
        host_offset,
        region,
        buffer_row_pitch,
        buffer_slice_pitch,
        host_row_pitch,
        host_slice_pitch,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
        CL_COMMAND_COPY_BUFFER_RECT
    );
}


extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueFillBuffer(cl_command_queue   command_queue,
    cl_mem             buffer,
    const void* pattern,
    size_t             pattern_size,
    size_t             offset,
    size_t             size,
    cl_uint            num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_2
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!buffer)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(buffer);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (resource.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("buffer must be a buffer object.", CL_INVALID_MEM_OBJECT);
    }

    if (offset > resource.m_Desc.image_width ||
        size > resource.m_Desc.image_width ||
        offset + size > resource.m_Desc.image_width)
    {
        return ReportError("offset/size too large.", CL_INVALID_VALUE);
    }

    switch (pattern_size)
    {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
    case 32:
    case 64:
    case 128:
        break;
    default:
        return ReportError("Invalid pattern_size. Valid values are {1, 2, 4, 8, 16, 32, 64, 128} for this device.", CL_INVALID_VALUE);
    }

    if (!pattern)
    {
        return ReportError("pattern must not be null.", CL_INVALID_VALUE);
    }

    if (size % pattern_size != 0 || offset % pattern_size != 0)
    {
        return ReportError("offset and size must be a multiple of pattern_size.", CL_INVALID_VALUE);
    }

    MemWriteFillTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)(offset + resource.m_Offset);
    CmdArgs.Width = (cl_uint)size;
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    MemWriteFillTask::FillData FillData;
    memcpy(FillData.Pattern, pattern, pattern_size);
    FillData.PatternSize = (cl_uint)pattern_size;
    CmdArgs.Data = FillData;

    try
    {
        std::unique_ptr<Task> task(new MemWriteFillTask(context, resource, CL_COMMAND_FILL_BUFFER, command_queue, CmdArgs, false));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
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

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWriteImage(cl_command_queue    command_queue,
    cl_mem              image,
    cl_bool             blocking_write,
    const size_t* origin,
    const size_t* region,
    size_t              input_row_pitch,
    size_t              input_slice_pitch,
    const void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!image)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(image);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (resource.m_Flags & (CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS))
    {
        return ReportError("Image is not writable from the host.", CL_INVALID_OPERATION);
    }

    if (!ptr)
    {
        return ReportError("ptr must not be null.", CL_INVALID_VALUE);
    }

    MemWriteFillTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)origin[0];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    auto imageResult = ProcessImageDimensions(ReportError, origin, region,
                                              resource, CmdArgs.FirstArraySlice, CmdArgs.NumArraySlices, CmdArgs.FirstMipLevel,
                                              CmdArgs.Height, CmdArgs.Depth, CmdArgs.DstY, CmdArgs.DstZ);
    if (imageResult != CL_SUCCESS)
    {
        return imageResult;
    }

    size_t ReqRowPitch = CD3D11FormatHelper::GetByteAlignment(GetDXGIFormatForCLImageFormat(resource.m_Format)) * region[0];
    if (input_row_pitch == 0)
    {
        input_row_pitch = ReqRowPitch;
    }
    else if (input_row_pitch < ReqRowPitch)
    {
        return ReportError("input_row_pitch must be 0 or at least large enough for a single row.", CL_INVALID_VALUE);
    }

    size_t ReqSlicePitch = input_row_pitch * CmdArgs.Height;
    if (input_slice_pitch == 0)
    {
        input_slice_pitch = ReqSlicePitch;
    }
    else if (input_slice_pitch < ReqSlicePitch)
    {
        return ReportError("input_slice_pitch must be 0 or at least input_row_pitch * image_height.", CL_INVALID_VALUE);
    }
    CmdArgs.Data = MemWriteFillTask::WriteData
    {
        ptr, (cl_uint)input_row_pitch, (cl_uint)input_slice_pitch
    };

    try
    {
        std::unique_ptr<Task> task(new MemWriteFillTask(context, resource, CL_COMMAND_WRITE_IMAGE, command_queue, CmdArgs, blocking_write == CL_FALSE));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
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


class FillImageTask : public Task
{
public:
    struct Args
    {
        cl_uint DstX;
        cl_uint DstY;
        cl_uint DstZ;
        cl_uint Width;
        cl_uint Height;
        cl_uint Depth;
        cl_ushort FirstArraySlice;
        cl_ushort NumArraySlices;
        cl_uchar FirstMipLevel;
        char Pattern[16];
    };

    FillImageTask(Context& Parent, Resource& Target, cl_command_queue CommandQueue, Args const& args);

private:
    Resource::ref_ptr_int m_Target;
    const Args m_Args;

    void MigrateResources() final
    {
        m_Target->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
    }
    void RecordImpl() final;
    void OnComplete() final
    {
        m_Target.Release();
    }
};

FillImageTask::FillImageTask(Context &Parent, Resource &Target, cl_command_queue CommandQueue, Args const& args)
    : Task(Parent, CL_COMMAND_FILL_IMAGE, CommandQueue)
    , m_Target(&Target)
    , m_Args(args)
{
}

void FillImageTask::RecordImpl()
{
    auto& ImmCtx = m_CommandQueue->GetD3DDevice().ImmCtx();
    for (cl_uint i = 0; i < m_Args.NumArraySlices; ++i)
    {
        D3D12TranslationLayer::CSubresourceSubset Subset(1, 1, 1,
                                                         (UINT8)m_Args.FirstMipLevel,
                                                         (UINT16)(m_Args.FirstArraySlice + i), 0);
        D3D12_BOX Box =
        {
            m_Args.DstX,
            m_Args.DstY,
            m_Args.DstZ,
            m_Args.DstX + m_Args.Width,
            m_Args.DstY + m_Args.Height,
            m_Args.DstZ + m_Args.Depth
        };
        ImmCtx.UpdateSubresources(
            m_Target->GetActiveUnderlyingResource(),
            Subset, nullptr, &Box,
            D3D12TranslationLayer::ImmediateContext::UpdateSubresourcesFlags::ScenarioImmediateContext,
            m_Args.Pattern);
    }
}

template <typename T> T FloatToNormalized(float x)
{
    constexpr auto max = std::numeric_limits<T>::max();
    constexpr auto min = std::is_signed_v<T> ? -max : 0;
    constexpr float min_float = std::is_signed_v<T> ? -1.0f : 0.0f;
    if (x != x) return (T)0;
    if (x >= 1.0f) return max;
    if (x <= min_float) return min;
    constexpr auto scale = std::is_signed_v<T> ?
        (1 << (sizeof(T) * 8 - 1)) - 1 :
        (1 << (sizeof(T) * 8)) - 1;
    float scaled = x * (float)scale;
    return (T)scaled;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueFillImage(cl_command_queue   command_queue,
    cl_mem             image,
    const void* fill_color,
    const size_t* origin,
    const size_t* region,
    cl_uint            num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_2
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!image)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(image);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (origin[0] > resource.m_Desc.image_width ||
        region[0] > resource.m_Desc.image_width ||
        origin[0] + region[0] > resource.m_Desc.image_width)
    {
        return ReportError("origin/region is too large.", CL_INVALID_VALUE);
    }

    if (!fill_color)
    {
        return ReportError("ptr must not be null.", CL_INVALID_VALUE);
    }

    FillImageTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)origin[0];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    // Compact the fill color into the pattern
    cl_uint PixelDataSize = GetChannelSizeBits(resource.m_Format.image_channel_data_type) / 8;
    for (cl_uint i = 0; i < GetNumChannelsInOrder(resource.m_Format.image_channel_order); ++i)
    {
        cl_uint dest_i = resource.m_Format.image_channel_order != CL_BGRA ? i :
            (i == 3 ? 3 : 2 - i);
        cl_uint src_i = resource.m_Format.image_channel_order == CL_A ? 3 : i;
        auto fill_floats = reinterpret_cast<const float*>(fill_color);
        switch (resource.m_Format.image_channel_data_type)
        {
        default:
            memcpy(CmdArgs.Pattern + dest_i * PixelDataSize,
                    &fill_floats[src_i],
                    PixelDataSize);
            break;
        case CL_HALF_FLOAT:
            reinterpret_cast<cl_ushort*>(CmdArgs.Pattern)[dest_i] = ConvertFloatToHalf(fill_floats[src_i]);
            break;
        case CL_UNORM_INT8:
            reinterpret_cast<cl_uchar*>(CmdArgs.Pattern)[dest_i] = FloatToNormalized<cl_uchar>(fill_floats[src_i]);
            break;
        case CL_UNORM_INT16:
            reinterpret_cast<cl_ushort*>(CmdArgs.Pattern)[dest_i] = FloatToNormalized<cl_ushort>(fill_floats[src_i]);
            break;
        case CL_SNORM_INT8:
            reinterpret_cast<cl_char*>(CmdArgs.Pattern)[dest_i] = FloatToNormalized<cl_char>(fill_floats[src_i]);
            break;
        case CL_SNORM_INT16:
            reinterpret_cast<cl_short*>(CmdArgs.Pattern)[dest_i] = FloatToNormalized<cl_short>(fill_floats[src_i]);
            break;
        }
    }

    auto imageResult = ProcessImageDimensions(ReportError, origin, region, resource,
                                              CmdArgs.FirstArraySlice, CmdArgs.NumArraySlices, CmdArgs.FirstMipLevel,
                                              CmdArgs.Height, CmdArgs.Depth, CmdArgs.DstY, CmdArgs.DstZ);
    if (imageResult != CL_SUCCESS)
    {
        return imageResult;
    }

    try
    {
        std::unique_ptr<Task> task(new FillImageTask(context, resource, command_queue, CmdArgs));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
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

class MemReadTask : public Task
{
public:
    struct Args
    {
        cl_uint SrcX;
        cl_uint SrcY;
        cl_uint SrcZ;
        cl_uint Width;
        cl_uint Height;
        cl_uint Depth;
        cl_ushort FirstArraySlice;
        cl_ushort NumArraySlices;
        cl_uchar FirstMipLevel;
        cl_uint DstX;
        cl_uint DstY;
        cl_uint DstZ;
        cl_uint SrcBufferRowPitch;
        cl_uint SrcBufferSlicePitch;
        void* pData;
        cl_uint DstRowPitch;
        cl_uint DstSlicePitch;
    };
    MemReadTask(Context& Parent, Resource& Source, cl_command_type CommandType,
        cl_command_queue CommandQueue, Args const& args)
        : Task(Parent, CommandType, CommandQueue)
        , m_Source(&Source)
        , m_Args(args)
    {
    }

    void CopyBits(void* pData, int Subresource, size_t SrcRowPitch, size_t SrcSlicePitch);

private:
    Resource::ref_ptr_int m_Source;
    const Args m_Args;
    
    void RecordViaCopy();

    void MigrateResources() final
    {
        m_Source->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
    }
    void RecordImpl() final;
    void OnComplete() final
    {
        m_Source.Release();
    }
};

void MemReadTask::CopyBits(void* pData, int Subresource, size_t SrcRowPitch, size_t SrcSlicePitch)
{
    const char *pSrc = reinterpret_cast<char*>(pData) + Subresource * SrcSlicePitch;
    const cl_uint FormatBytes = GetFormatSizeBytes(m_Source->m_Format);
    if (m_Args.DstZ != 0 || m_Args.DstY != 0 || m_Args.DstX != 0)
    {
        for (cl_uint z = 0; z < m_Args.Depth; ++z)
        {
            for (cl_uint y = 0; y < m_Args.Height; ++y)
            {
                char* pDest = reinterpret_cast<char*>(m_Args.pData) +
                    (z + Subresource + m_Args.DstZ) * m_Args.DstSlicePitch +
                    (y + m_Args.DstY) * m_Args.DstRowPitch +
                    m_Args.DstX * FormatBytes;
                const char *pRowSrc = pSrc +
                    (z + m_Args.SrcZ) * SrcSlicePitch +
                    (y + m_Args.SrcY) * SrcRowPitch +
                    m_Args.SrcX * FormatBytes;
                memcpy(pDest, pRowSrc, m_Args.Width * FormatBytes);
            }
        }
    }
    else
    {
        char* pDest = reinterpret_cast<char*>(m_Args.pData) +
            (Subresource + m_Args.DstZ) * m_Args.DstSlicePitch;
        D3D12_MEMCPY_DEST Dest = { pDest, m_Args.DstRowPitch, m_Args.DstSlicePitch };
        D3D12_SUBRESOURCE_DATA Src = { pSrc, (LONG_PTR)SrcRowPitch, (LONG_PTR)SrcSlicePitch };
        MemcpySubresource(&Dest, &Src, FormatBytes * m_Args.Width, m_Args.Height, m_Args.Depth);
    }
}

void MemReadTask::RecordImpl()
{
    if (!(m_Source->m_Flags & CL_MEM_ALLOC_HOST_PTR))
    {
        RecordViaCopy();
        return;
    }

    auto& ImmCtx = m_CommandQueue->GetD3DDevice().ImmCtx();
    for (UINT16 i = 0; i < m_Args.NumArraySlices; ++i)
    {
        D3D12TranslationLayer::MappedSubresource MapRet = {};
        D3D12_BOX SrcBox =
        {
            m_Args.SrcX, m_Args.SrcY, m_Args.SrcZ,
            m_Args.SrcX + m_Args.Width,
            m_Args.SrcY + m_Args.Height,
            m_Args.SrcZ + m_Args.Depth
        };

        // Unlike for writing, we don't need to be super picky about what
        // we read - we can ask the GPU to read data that we're not going to write
        // out into the user buffer
        if (m_Source->m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
        {
            SrcBox = {};
            SrcBox.left = (UINT)(m_Source->m_Offset + // Buffer suballocation offset
                m_Args.SrcX); // Offset within row
            SrcBox.right = SrcBox.left + m_Args.Width +
                ((m_Args.Height - 1) * m_Args.SrcBufferRowPitch) +
                ((m_Args.Depth - 1) * m_Args.SrcBufferSlicePitch);
        }
        ImmCtx.Map(m_Source->GetActiveUnderlyingResource(), i,
            D3D12TranslationLayer::MAP_TYPE_READ, false,
            nullptr, &MapRet);

        size_t SrcRowPitch = m_Args.SrcBufferRowPitch;
        size_t SrcSlicePitch = m_Args.SrcBufferSlicePitch;
        if (m_Source->m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
        {
            SrcRowPitch = MapRet.RowPitch;
            SrcSlicePitch = MapRet.DepthPitch;
        }

        if (MapRet.pData)
        {
            CopyBits(MapRet.pData, i, SrcRowPitch, SrcSlicePitch);
        }
        else
        {
            assert(m_Source->m_CreationArgs.m_desc12.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
            assert(m_Args.DstX == 0 && m_Args.DstY == 0 && m_Args.DstZ == 0);
            auto pResource12 = m_Source->GetActiveUnderlyingResource()->GetUnderlyingResource();
            D3D12TranslationLayer::ThrowFailure(pResource12->ReadFromSubresource(
                m_Args.pData, m_Args.DstRowPitch, m_Args.DstSlicePitch, i, &SrcBox));
        }

        ImmCtx.Unmap(m_Source->GetActiveUnderlyingResource(), i, D3D12TranslationLayer::MAP_TYPE_READ, nullptr);
    }
}

cl_int clEnqueueReadBufferRectImpl(cl_command_queue    command_queue,
    cl_mem              buffer,
    cl_bool             blocking_read,
    const size_t* buffer_offset,
    const size_t* host_offset,
    const size_t* region,
    size_t              buffer_row_pitch,
    size_t              buffer_slice_pitch,
    size_t              host_row_pitch,
    size_t              host_slice_pitch,
    void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event,
    cl_command_type command_type)
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!buffer)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(buffer);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (resource.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("buffer must be a buffer object.", CL_INVALID_MEM_OBJECT);
    }

    if (buffer_offset[0] > resource.m_Desc.image_width ||
        region[0] > resource.m_Desc.image_width ||
        buffer_offset[0] + region[0] > resource.m_Desc.image_width)
    {
        return ReportError("Offsets/regions too large.", CL_INVALID_VALUE);
    }

    if (buffer_row_pitch == 0)
    {
        buffer_row_pitch = region[0];
    }
    else if (buffer_row_pitch > resource.m_Desc.image_width ||
        buffer_row_pitch < region[0])
    {
        return ReportError("buffer_row_pitch must be 0 or between region[0] and the buffer size.", CL_INVALID_VALUE);
    }

    if (host_row_pitch == 0)
    {
        host_row_pitch = region[0];
    }
    else if (host_row_pitch > resource.m_Desc.image_width ||
        host_row_pitch < region[0])
    {
        return ReportError("host_row_pitch must be 0 or between region[0] and the buffer size.", CL_INVALID_VALUE);
    }

    size_t SliceSizeInBytes = (buffer_offset[1] + region[1] - 1) * buffer_row_pitch + buffer_offset[0] + region[0];
    if (SliceSizeInBytes > resource.m_Desc.image_width)
    {
        return ReportError("Offsets/regions too large.", CL_INVALID_VALUE);
    }

    size_t ReqBufferSlicePitch = buffer_row_pitch * region[1];
    size_t ReqHostSlicePitch = host_row_pitch * region[1];
    if (buffer_slice_pitch == 0)
    {
        buffer_slice_pitch = ReqBufferSlicePitch;
    }
    else if (buffer_slice_pitch > resource.m_Desc.image_width ||
        buffer_slice_pitch < ReqBufferSlicePitch)
    {
        return ReportError("buffer_slice_pitch must be 0 or between (region[0] * buffer_row_pitch) and the buffer size.", CL_INVALID_VALUE);
    }

    if (host_slice_pitch == 0)
    {
        host_slice_pitch = ReqHostSlicePitch;
    }
    else if (host_slice_pitch > resource.m_Desc.image_width ||
        host_slice_pitch < ReqHostSlicePitch)
    {
        return ReportError("host_slice_pitch must be 0 or between (region[0] * buffer_row_pitch) and the buffer size.", CL_INVALID_VALUE);
    }

    size_t ResourceSizeInBytes = (buffer_offset[2] + region[2] - 1) * buffer_slice_pitch + SliceSizeInBytes;
    if (ResourceSizeInBytes > resource.m_Desc.image_width)
    {
        return ReportError("Offsets/regions too large.", CL_INVALID_VALUE);
    }

    if (resource.m_Flags & (CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_NO_ACCESS))
    {
        return ReportError("Buffer is not readable from the host.", CL_INVALID_OPERATION);
    }

    if (!ptr)
    {
        return ReportError("ptr must not be null.", CL_INVALID_VALUE);
    }

    MemReadTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)host_offset[0];
    CmdArgs.DstY = (cl_uint)host_offset[1];
    CmdArgs.DstZ = (cl_uint)host_offset[2];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = (cl_uint)region[1];
    CmdArgs.Depth = (cl_uint)region[2];
    CmdArgs.SrcX = (cl_uint)buffer_offset[0];
    CmdArgs.SrcY = (cl_uint)buffer_offset[1];
    CmdArgs.SrcZ = (cl_uint)buffer_offset[2];
    CmdArgs.NumArraySlices = 1;
    CmdArgs.SrcBufferRowPitch = (cl_uint)buffer_row_pitch;
    CmdArgs.SrcBufferSlicePitch = (cl_uint)buffer_slice_pitch;
    CmdArgs.pData = ptr;
    CmdArgs.DstRowPitch = (cl_uint)host_row_pitch;
    CmdArgs.DstSlicePitch = (cl_uint)host_slice_pitch;

    cl_int ret = CL_SUCCESS;
    try
    {
        std::unique_ptr<Task> task(new MemReadTask(context, resource, command_type, command_queue, CmdArgs));
        {
            auto Lock = g_Platform->GetTaskPoolLock();
            task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
            queue.QueueTask(task.get(), Lock);
            if (blocking_read)
            {
                queue.Flush(Lock, /* flushDevice */ true);
            }
        }

        if (blocking_read)
        {
            ret = task->WaitForCompletion();
        }

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
    return ret;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReadBuffer(cl_command_queue    command_queue,
    cl_mem              buffer,
    cl_bool             blocking_read,
    size_t              offset,
    size_t              size,
    void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    size_t buffer_offset[3] = { offset, 0, 0 };
    size_t host_offset[3] = { 0, 0, 0 };
    size_t region[3] = { size, 1, 1 };
    return clEnqueueReadBufferRectImpl(
        command_queue,
        buffer,
        blocking_read,
        buffer_offset,
        host_offset,
        region,
        0,
        0,
        0,
        0,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
        CL_COMMAND_READ_BUFFER);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReadBufferRect(cl_command_queue    command_queue,
    cl_mem              buffer,
    cl_bool             blocking_read,
    const size_t* buffer_offset,
    const size_t* host_offset,
    const size_t* region,
    size_t              buffer_row_pitch,
    size_t              buffer_slice_pitch,
    size_t              host_row_pitch,
    size_t              host_slice_pitch,
    void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_1
{
    return clEnqueueReadBufferRectImpl(
        command_queue,
        buffer,
        blocking_read,
        buffer_offset,
        host_offset,
        region,
        buffer_row_pitch,
        buffer_slice_pitch,
        host_row_pitch,
        host_slice_pitch,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
        CL_COMMAND_READ_BUFFER_RECT);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReadImage(cl_command_queue     command_queue,
    cl_mem               image,
    cl_bool              blocking_read,
    const size_t* origin,
    const size_t* region,
    size_t               row_pitch,
    size_t               slice_pitch,
    void* ptr,
    cl_uint              num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!image)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(image);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (resource.m_Flags & (CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_NO_ACCESS))
    {
        return ReportError("Image is not readable from the host.", CL_INVALID_OPERATION);
    }

    if (!ptr)
    {
        return ReportError("ptr must not be null.", CL_INVALID_VALUE);
    }

    MemReadTask::Args CmdArgs = {};
    CmdArgs.SrcX = (cl_uint)origin[0];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;
    CmdArgs.pData = ptr;

    auto imageResult = ProcessImageDimensions(ReportError, origin, region, resource,
                                              CmdArgs.FirstArraySlice, CmdArgs.NumArraySlices, CmdArgs.FirstMipLevel,
                                              CmdArgs.Height, CmdArgs.Depth, CmdArgs.SrcY, CmdArgs.SrcZ);
    if (imageResult != CL_SUCCESS)
    {
        return imageResult;
    }

    size_t ReqRowPitch = CD3D11FormatHelper::GetByteAlignment(GetDXGIFormatForCLImageFormat(resource.m_Format)) * region[0];
    if (row_pitch == 0)
    {
        row_pitch = ReqRowPitch;
    }
    else if (row_pitch < ReqRowPitch)
    {
        return ReportError("row_pitch must be 0 or at least large enough for a single row.", CL_INVALID_VALUE);
    }

    size_t ReqSlicePitch = row_pitch * CmdArgs.Height;
    if (slice_pitch == 0)
    {
        slice_pitch = ReqSlicePitch;
    }
    else if (slice_pitch < ReqSlicePitch)
    {
        return ReportError("slice_pitch must be 0 or at least row_pitch * image_height.", CL_INVALID_VALUE);
    }
    CmdArgs.DstRowPitch = (cl_uint)row_pitch;
    CmdArgs.DstSlicePitch = (cl_uint)slice_pitch;

    cl_int ret = CL_SUCCESS;
    try
    {
        std::unique_ptr<Task> task(new MemReadTask(context, resource, CL_COMMAND_READ_IMAGE, command_queue, CmdArgs));
        {
            auto Lock = g_Platform->GetTaskPoolLock();
            task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
            queue.QueueTask(task.get(), Lock);
            if (blocking_read)
            {
                queue.Flush(Lock, /* flushDevice */ true);
            }
        }

        if (blocking_read)
        {
            ret = task->WaitForCompletion();
        }

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
    return ret;
}

class CopyResourceTask : public Task
{
public:
    struct Args
    {
        cl_uint DstX;
        cl_uint DstY;
        cl_uint DstZ;
        cl_uint Width;
        cl_uint Height;
        cl_uint Depth;
        cl_uint SrcX;
        cl_uint SrcY;
        cl_uint SrcZ;
        cl_ushort FirstSrcArraySlice;
        cl_ushort FirstDstArraySlice;
        cl_ushort NumArraySlices;
        cl_uchar FirstSrcMipLevel;
        cl_uchar FirstDstMipLevel;
    };

    CopyResourceTask(Context& Parent, Resource& Source, Resource& Dest,
        cl_command_queue CommandQueue, Args const& args, cl_command_type type)
        : Task(Parent, type, CommandQueue)
        , m_Source(&Source)
        , m_Dest(&Dest)
        , m_Args(args)
    {
    }

private:
    Resource::ref_ptr_int m_Source;
    Resource::ref_ptr_int m_Dest;
    const Args m_Args;

    static constexpr bool ImageTypesCopyCompatible(cl_mem_object_type a, cl_mem_object_type b)
    {
        if (a == b) return true;
        switch (a)
        {
        default: return false;
        case CL_MEM_OBJECT_IMAGE1D: return b == CL_MEM_OBJECT_IMAGE1D_ARRAY || b == CL_MEM_OBJECT_IMAGE1D_BUFFER;
        case CL_MEM_OBJECT_IMAGE1D_ARRAY: return b == CL_MEM_OBJECT_IMAGE1D || b == CL_MEM_OBJECT_IMAGE1D_BUFFER;
        case CL_MEM_OBJECT_IMAGE1D_BUFFER: return b == CL_MEM_OBJECT_IMAGE1D || b == CL_MEM_OBJECT_IMAGE1D_ARRAY;
        case CL_MEM_OBJECT_IMAGE2D: return b == CL_MEM_OBJECT_IMAGE2D_ARRAY;
        case CL_MEM_OBJECT_IMAGE2D_ARRAY: return b == CL_MEM_OBJECT_IMAGE2D;
        }
    }

    void MigrateResources() final
    {
        m_Source->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
        m_Dest->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
    }
    void RecordImpl() final
    {
        auto& ImmCtx = m_CommandQueue->GetD3DDevice().ImmCtx();
        if (ImageTypesCopyCompatible(m_Source->m_Desc.image_type, m_Dest->m_Desc.image_type))
        {
            for (cl_ushort i = 0; i < m_Args.NumArraySlices; ++i)
            {
                D3D12_BOX SrcBox =
                {
                    m_Args.SrcX,
                    m_Args.SrcY,
                    m_Args.SrcZ,
                    m_Args.SrcX + m_Args.Width,
                    m_Args.SrcY + m_Args.Height,
                    m_Args.SrcZ + m_Args.Depth
                };
                UINT SrcSubresource = (m_Args.FirstSrcArraySlice + i) * m_Source->m_CreationArgs.m_desc12.MipLevels
                    + m_Args.FirstSrcMipLevel;
                UINT DstSubresource = (m_Args.FirstDstArraySlice + i) * m_Dest->m_CreationArgs.m_desc12.MipLevels
                    + m_Args.FirstDstMipLevel;
                ImmCtx.ResourceCopyRegion(
                    m_Dest->GetActiveUnderlyingResource(),
                    DstSubresource,
                    m_Args.DstX,
                    m_Args.DstY,
                    m_Args.DstZ,
                    m_Source->GetActiveUnderlyingResource(),
                    SrcSubresource,
                    &SrcBox);
            }
        }
        else
        {
            // This can also support copying one row between Tex1D[Array], Tex2D[Array], and Tex3D, 
            // or one slice between Tex2D and Tex3D.
            // It cannot support copying arrays of rows or arrays of slices
            assert(m_Args.Depth == 1);
            assert(m_Args.NumArraySlices == 1);

            // Since D3D12 can't support this, we'll allocate a temp buffer in the form of a Tex2D
            // The translation layer converts these to CopyTextureRegion ops, which don't have any
            // dimensionality on the footprint desc for the buffer.
            D3D12TranslationLayer::ResourceCreationArgs Args = {};
            Args.m_appDesc.m_Subresources = 1;
            Args.m_appDesc.m_SubresourcesPerPlane = 1;
            Args.m_appDesc.m_NonOpaquePlaneCount = 1;
            Args.m_appDesc.m_MipLevels = 1;
            Args.m_appDesc.m_ArraySize = 1;
            Args.m_appDesc.m_Depth = 1;
            Args.m_appDesc.m_Width = m_Args.Width;
            Args.m_appDesc.m_Height = m_Args.Height;
            Args.m_appDesc.m_Format = m_Source->m_CreationArgs.m_appDesc.Format();
            Args.m_appDesc.m_Samples = 1;
            Args.m_appDesc.m_Quality = 0;
            Args.m_appDesc.m_resourceDimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Args.m_appDesc.m_usage = D3D12TranslationLayer::RESOURCE_USAGE_DEFAULT;
            Args.m_appDesc.m_bindFlags = D3D12TranslationLayer::RESOURCE_BIND_NONE;
            Args.m_desc12 = CD3DX12_RESOURCE_DESC::Tex2D(
                Args.m_appDesc.m_Format,
                Args.m_appDesc.m_Width,
                Args.m_appDesc.m_Height,
                1, 1, 1, 0, D3D12_RESOURCE_FLAG_NONE, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
            Args.m_heapDesc = CD3DX12_HEAP_DESC(0, D3D12_HEAP_TYPE_DEFAULT);

            UINT64 totalSize = 0;
            ImmCtx.m_pDevice12->GetCopyableFootprints(&Args.m_desc12, 0, 1, 0, nullptr, nullptr, nullptr, &totalSize);

            Args.m_desc12 = CD3DX12_RESOURCE_DESC::Buffer(totalSize, D3D12_RESOURCE_FLAG_NONE);
            Args.m_isPlacedTexture = true;

            auto tempResource = D3D12TranslationLayer::Resource::CreateResource(&ImmCtx, Args, D3D12TranslationLayer::ResourceAllocationContext::ImmediateContextThreadTemporary);
            D3D12_BOX SrcBox =
            {
                m_Args.SrcX,
                m_Args.SrcY,
                m_Args.SrcZ,
                m_Args.SrcX + m_Args.Width,
                m_Args.SrcY + m_Args.Height,
                m_Args.SrcZ + m_Args.Depth
            };
            UINT SrcSubresource = m_Args.FirstSrcArraySlice * m_Source->m_CreationArgs.m_desc12.MipLevels + m_Args.FirstSrcMipLevel;
            UINT DstSubresource = m_Args.FirstDstArraySlice * m_Dest->m_CreationArgs.m_desc12.MipLevels + m_Args.FirstDstMipLevel;
            ImmCtx.ResourceCopyRegion(tempResource.get(), 0, 0, 0, 0,
                m_Source->GetActiveUnderlyingResource(), SrcSubresource, &SrcBox);
            ImmCtx.ResourceCopyRegion(m_Dest->GetActiveUnderlyingResource(), DstSubresource,
                m_Args.DstX, m_Args.DstY, m_Args.DstZ, tempResource.get(), 0, nullptr);
        }
    }
    void OnComplete() final
    {
        m_Source.Release();
        m_Dest.Release();
    }
};

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyBuffer(cl_command_queue    command_queue,
    cl_mem              src_buffer,
    cl_mem              dst_buffer,
    size_t              src_offset,
    size_t              dst_offset,
    size_t              size,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (!src_buffer || !dst_buffer)
    {
        return ReportError("src_buffer and dst_buffer must not be NULL.", CL_INVALID_MEM_OBJECT);
    }

    auto& source = *static_cast<Resource*>(src_buffer);
    auto& dest = *static_cast<Resource*>(dst_buffer);
    if (&source.m_Parent.get() != &context || &dest.m_Parent.get() != &context)
    {
        return ReportError("src_buffer and dst_buffer must belong to the same context as the command_queue", CL_INVALID_CONTEXT);
    }

    if (source.m_Desc.image_type != CL_MEM_OBJECT_BUFFER ||
        dest.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("src_buffer and dst_buffer must be buffers", CL_INVALID_MEM_OBJECT);
    }

    if (size == 0 ||
        size + src_offset > source.m_Desc.image_width ||
        size + dst_offset > dest.m_Desc.image_width)
    {
        return ReportError("size must be nonzero, and size and offsets must address regions within buffers", CL_INVALID_VALUE);
    }

    if (source.m_ParentBuffer.Get() == &dest ||
        dest.m_ParentBuffer.Get() == &source ||
        &source == &dest)
    {
        size_t absolute_src_offset = src_offset + source.m_Offset;
        size_t absolute_dst_offset = dst_offset + dest.m_Offset;
        if ((absolute_src_offset <= absolute_dst_offset && absolute_dst_offset <= absolute_src_offset + size - 1) ||
            (absolute_dst_offset <= absolute_src_offset && absolute_src_offset <= absolute_dst_offset + size - 1))
        {
            return ReportError("Buffer regions overlap", CL_MEM_COPY_OVERLAP);
        }
    }

    CopyResourceTask::Args CmdArgs = {};
    CmdArgs.SrcX = (cl_uint)(src_offset + source.m_Offset);
    CmdArgs.DstX = (cl_uint)(dst_offset + dest.m_Offset);
    CmdArgs.Width = (cl_uint)size;
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    try
    {
        std::unique_ptr<Task> task(new CopyResourceTask(context, source, dest, command_queue, CmdArgs, CL_COMMAND_COPY_BUFFER));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyImage(cl_command_queue     command_queue,
    cl_mem               src_image,
    cl_mem               dst_image,
    const size_t* src_origin,
    const size_t* dst_origin,
    const size_t* region,
    cl_uint              num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (!src_image || !dst_image)
    {
        return ReportError("src_image and dst_image must not be NULL.", CL_INVALID_MEM_OBJECT);
    }

    auto& source = *static_cast<Resource*>(src_image);
    auto& dest = *static_cast<Resource*>(dst_image);
    if (&source.m_Parent.get() != &context || &dest.m_Parent.get() != &context)
    {
        return ReportError("src_image and dst_image must belong to the same context as the command_queue", CL_INVALID_CONTEXT);
    }

    if (source.m_Desc.image_type == CL_MEM_OBJECT_BUFFER ||
        dest.m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("src_image and dst_image must not be buffers", CL_INVALID_MEM_OBJECT);
    }

    if (source.m_Format.image_channel_data_type != dest.m_Format.image_channel_data_type ||
        source.m_Format.image_channel_order != dest.m_Format.image_channel_order)
    {
        return ReportError("src_image and dst_image must have the same format", CL_IMAGE_FORMAT_MISMATCH);
    }

    CopyResourceTask::Args CmdArgs = {};
    CmdArgs.SrcX = (cl_uint)src_origin[0];
    CmdArgs.DstX = (cl_uint)dst_origin[0];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    auto imageResult = ProcessImageDimensions(ReportError, src_origin, region, source,
                                              CmdArgs.FirstSrcArraySlice, CmdArgs.NumArraySlices, CmdArgs.FirstSrcMipLevel,
                                              CmdArgs.Height, CmdArgs.Depth, CmdArgs.SrcY, CmdArgs.SrcZ);
    if (imageResult != CL_SUCCESS)
    {
        return imageResult;
    }
    imageResult = ProcessImageDimensions(ReportError, dst_origin, region, dest,
                                         CmdArgs.FirstDstArraySlice, CmdArgs.NumArraySlices, CmdArgs.FirstDstMipLevel,
                                         CmdArgs.Height, CmdArgs.Depth, CmdArgs.DstY, CmdArgs.DstZ);
    if (imageResult != CL_SUCCESS)
    {
        return imageResult;
    }

    if (source.m_ParentBuffer.Get() == &dest ||
        dest.m_ParentBuffer.Get() == &source ||
        &source == &dest)
    {
        cl_uint overlap = 0;
        for (cl_uint i = 0; i < 3; ++i)
        {
            if ((src_origin[i] <= dst_origin[i] && dst_origin[i] <= src_origin[i] + region[i]) ||
                (dst_origin[i] <= src_origin[i] && src_origin[i] <= dst_origin[i] + region[i]))
            {
                ++overlap;
            }
        }
        if (overlap == 3)
        {
            return ReportError("Image regions overlap", CL_MEM_COPY_OVERLAP);
        }
    }

    try
    {
        std::unique_ptr<Task> task(new CopyResourceTask(context, source, dest, command_queue, CmdArgs, CL_COMMAND_COPY_IMAGE));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
    return CL_SUCCESS;
}

class CopyBufferRectTask : public Task
{
public:
    struct Args
    {
        cl_uint DstOffset;
        cl_uint DstX;
        cl_uint DstY;
        cl_uint DstZ;
        cl_uint Width;
        cl_uint Height;
        cl_uint Depth;
        cl_uint SrcOffset;
        cl_uint SrcX;
        cl_uint SrcY;
        cl_uint SrcZ;
        cl_uint DstBufferRowPitch;
        cl_uint DstBufferSlicePitch;
        cl_uint SrcBufferRowPitch;
        cl_uint SrcBufferSlicePitch;
    };

    CopyBufferRectTask(Context& Parent, Resource& Source, Resource& Dest,
        cl_command_queue CommandQueue, Args const& args)
        : Task(Parent, CL_COMMAND_COPY_BUFFER_RECT, CommandQueue)
        , m_Source(&Source)
        , m_Dest(&Dest)
        , m_Args(args)
    {
    }

private:
    Resource::ref_ptr_int m_Source;
    Resource::ref_ptr_int m_Dest;
    const Args m_Args;

    void MigrateResources() final
    {
        m_Source->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
        m_Dest->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
    }
    void RecordImpl() final;
    void OnComplete() final
    {
        m_Source.Release();
        m_Dest.Release();
    }
};

void CopyBufferRectTask::RecordImpl()
{
    // TODO: Fast-path when pitches line up with D3D12 buffer-as-texture support, and not same-resource copy
    /*
    if (m_Source->GetUnderlyingResource() != m_Dest->GetUnderlyingResource() &&
        m_Args.DstBufferRowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0 &&
        m_Args.DstBufferSlicePitch == m_Args.DstBufferRowPitch * m_Args.Height &&
        m_Args.SrcBufferRowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0 &&
        m_Args.SrcBufferSlicePitch == m_Args.SrcBufferRowPitch * m_Args.Height &&
        m_Dest->m_Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0 &&
        m_Source->m_Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0)
    {
        auto& ImmCtx = m_Parent->GetDevice().ImmCtx();

        ImmCtx.GetResourceStateManager().TransitionResource(m_Dest->GetUnderlyingResource(), D3D12_RESOURCE_STATE_COPY_DEST);
        ImmCtx.GetResourceStateManager().TransitionResource(m_Source->GetUnderlyingResource(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        ImmCtx.GetResourceStateManager().ApplyAllResourceTransitions();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT DstFootprint = { m_Dest->m_Offset,
            {
                DXGI_FORMAT_R8_UINT,
                m_Args.Width,
                m_Args.Height,
                m_Args.Depth,
                m_Args.DstBufferRowPitch
            } };
        CD3DX12_TEXTURE_COPY_LOCATION Dst(m_Dest->GetUnderlyingResource()->GetUnderlyingResource(), DstFootprint);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT SrcFootprint = { m_Source->m_Offset,
            {
                DXGI_FORMAT_R8_UINT,
                m_Args.Width,
                m_Args.Height,
                m_Args.Depth,
                m_Args.SrcBufferRowPitch
            } };
        CD3DX12_TEXTURE_COPY_LOCATION Src(m_Source->GetUnderlyingResource()->GetUnderlyingResource(), SrcFootprint);
        ImmCtx.GetGraphicsCommandList()->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        ImmCtx.PostCopy(m_Source->GetUnderlyingResource(), 0, m_Dest->GetUnderlyingResource(), 0, 1);
    }
    else
    */
    {
        for (cl_uint z = 0; z < m_Args.Depth; ++z)
        {
            for (cl_uint y = 0; y < m_Args.Height; ++y)
            {
                D3D12_BOX SrcBox =
                {
                    (UINT)(m_Source->m_Offset + m_Args.SrcOffset +
                        (z + m_Args.SrcZ) * m_Args.SrcBufferSlicePitch +
                        (y + m_Args.SrcY) * m_Args.SrcBufferRowPitch +
                        m_Args.SrcX),
                    0, 0, 1, 1, 1
                };
                SrcBox.right = SrcBox.left + m_Args.Width;
                UINT DstOffset =
                    (UINT)(m_Dest->m_Offset + m_Args.DstOffset +
                    (z + m_Args.DstZ) * m_Args.DstBufferSlicePitch +
                    (y + m_Args.DstY) * m_Args.DstBufferRowPitch +
                    m_Args.DstX);
                m_CommandQueue->GetD3DDevice().ImmCtx().ResourceCopyRegion(
                    m_Dest->GetActiveUnderlyingResource(),
                    0, //SubresourceIndex
                    DstOffset,
                    0, 0,
                    m_Source->GetActiveUnderlyingResource(),
                    0, //SubresourceIndex,
                    &SrcBox);
            }
        }
    }
}

// Adapted from OpenCL spec, Appendix D
bool check_copy_overlap(
    const size_t src_offset,
    const size_t dst_offset,
    const size_t src_origin[],
    const size_t dst_origin[],
    const size_t region[],
    const size_t row_pitch,
    const size_t slice_pitch)
{
    const size_t slice_size = (region[1] - 1) * row_pitch + region[0];

    /* No overlap if region[0] for dst or src fits in the gap
     * between region[0] and row_pitch.
     */
    {
        const size_t src_dx = (src_origin[0] + src_offset) % row_pitch;
        const size_t dst_dx = (dst_origin[0] + dst_offset) % row_pitch;

        if (((dst_dx >= src_dx + region[0]) &&
            (dst_dx + region[0] <= src_dx + row_pitch)) ||
            ((src_dx >= dst_dx + region[0]) &&
                (src_dx + region[0] <= dst_dx + row_pitch)))
        {
            return false;
        }
    }

    /* No overlap if region[1] for dst or src fits in the gap
     * between region[1] and slice_pitch.
     */
    {
        const size_t src_dy =
            (src_origin[1] * row_pitch + src_origin[0] + src_offset) % slice_pitch;
        const size_t dst_dy =
            (dst_origin[1] * row_pitch + dst_origin[0] + dst_offset) % slice_pitch;

        if (((dst_dy >= src_dy + slice_size) &&
            (dst_dy + slice_size <= src_dy + slice_pitch)) ||
            ((src_dy >= dst_dy + slice_size) &&
                (src_dy + slice_size <= dst_dy + slice_pitch))) {
            return false;
        }
    }

    /* Otherwise src and dst overlap. */
    return true;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyBufferRect(cl_command_queue    command_queue,
    cl_mem              src_buffer,
    cl_mem              dst_buffer,
    const size_t* src_origin,
    const size_t* dst_origin,
    const size_t* region,
    size_t              src_row_pitch,
    size_t              src_slice_pitch,
    size_t              dst_row_pitch,
    size_t              dst_slice_pitch,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_1
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (!src_buffer || !dst_buffer)
    {
        return ReportError("src_buffer and dst_buffer must not be NULL.", CL_INVALID_MEM_OBJECT);
    }

    auto& source = *static_cast<Resource*>(src_buffer);
    auto& dest = *static_cast<Resource*>(dst_buffer);
    if (&source.m_Parent.get() != &context || &dest.m_Parent.get() != &context)
    {
        return ReportError("src_buffer and dst_buffer must belong to the same context as the command_queue", CL_INVALID_CONTEXT);
    }

    if (source.m_Desc.image_type != CL_MEM_OBJECT_BUFFER ||
        dest.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("src_buffer and dst_buffer must be buffers", CL_INVALID_MEM_OBJECT);
    }

    if (region[0] == 0 ||
        region[1] == 0 ||
        region[2] == 0)
    {
        return ReportError("region contains a 0", CL_INVALID_VALUE);
    }

    if (src_row_pitch == 0)
    {
        src_row_pitch = region[0];
    }
    else if (src_row_pitch < region[0])
    {
        return ReportError("src_row_pitch must be >= region[0]", CL_INVALID_VALUE);
    }

    if (src_slice_pitch == 0)
    {
        src_slice_pitch = region[1] * src_row_pitch;
    }
    else if (src_slice_pitch < region[1] * src_row_pitch)
    {
        return ReportError("src_slice_pitch must be >= (region[1] * src_row_pitch)", CL_INVALID_VALUE);
    }

    if (dst_row_pitch == 0)
    {
        dst_row_pitch = region[0];
    }
    else if (dst_row_pitch < region[0])
    {
        return ReportError("dst_row_pitch must be >= region[0]", CL_INVALID_VALUE);
    }

    if (dst_slice_pitch == 0)
    {
        dst_slice_pitch = region[1] * dst_row_pitch;
    }
    else if (dst_slice_pitch < region[1] * dst_row_pitch)
    {
        return ReportError("dst_slice_pitch must be >= (region[1] * dst_row_pitch)", CL_INVALID_VALUE);
    }

    // From OpenCL spec, Appendix D
    const size_t src_slice_size = (region[1] - 1) * src_row_pitch + region[0];
    const size_t dst_slice_size = (region[1] - 1) * dst_row_pitch + region[0];
    const size_t src_block_size = (region[2] - 1) * src_slice_pitch + src_slice_size;
    const size_t dst_block_size = (region[2] - 1) * dst_slice_pitch + dst_slice_size;
    const size_t src_start = src_origin[2] * src_slice_pitch
        + src_origin[1] * dst_row_pitch
        + src_origin[0]
        + source.m_Offset;
    const size_t src_end = src_start + src_block_size;
    const size_t dst_start = dst_origin[2] * dst_slice_pitch
        + dst_origin[1] * dst_row_pitch
        + dst_origin[0]
        + dest.m_Offset;
    const size_t dst_end = dst_start + dst_block_size;

    if (src_end - source.m_Offset > source.m_Desc.image_width ||
        dst_end - dest.m_Offset > dest.m_Desc.image_width)
    {
        return ReportError("Offsets and region would require accessing out of bounds of buffer objects", CL_INVALID_VALUE);
    }

    if (source.m_ParentBuffer.Get() == &dest ||
        dest.m_ParentBuffer.Get() == &source ||
        &source == &dest)
    {
        if ((src_start <= dst_start && dst_start <= src_end) ||
            (dst_start <= src_start && src_start <= dst_end))
        {
            if (src_row_pitch != dst_row_pitch ||
                src_slice_pitch != dst_slice_pitch ||
                check_copy_overlap(source.m_Offset, dest.m_Offset, src_origin, dst_origin, region, src_row_pitch, src_slice_pitch))
            {
                return ReportError("Buffer regions overlap", CL_MEM_COPY_OVERLAP);
            }
        }
    }

    CopyBufferRectTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)dst_origin[0];
    CmdArgs.DstY = (cl_uint)dst_origin[1];
    CmdArgs.DstZ = (cl_uint)dst_origin[2];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = (cl_uint)region[1];
    CmdArgs.Depth = (cl_uint)region[2];
    CmdArgs.SrcX = (cl_uint)src_origin[0];
    CmdArgs.SrcY = (cl_uint)src_origin[1];
    CmdArgs.SrcZ = (cl_uint)src_origin[2];
    CmdArgs.DstBufferRowPitch = (cl_uint)dst_row_pitch;
    CmdArgs.DstBufferSlicePitch = (cl_uint)dst_slice_pitch;
    CmdArgs.SrcBufferRowPitch = (cl_uint)src_row_pitch;
    CmdArgs.SrcBufferSlicePitch = (cl_uint)src_slice_pitch;

    try
    {
        std::unique_ptr<Task> task(new CopyBufferRectTask(context, source, dest, command_queue, CmdArgs));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
    return CL_SUCCESS;
}

class CopyBufferAndImageTask : public Task
{
public:
    struct Args
    {
        cl_uint ImageX;
        cl_uint ImageY;
        cl_uint ImageZ;
        cl_uint Width;
        cl_uint Height;
        cl_uint Depth;
        size_t BufferOffset;
        cl_uint BufferPitch;
        cl_ushort FirstImageArraySlice;
        cl_ushort NumArraySlices;
        cl_uchar FirstImageMipLevel;
    };

    CopyBufferAndImageTask(Context& Parent, Resource& Source, Resource& Dest,
        cl_command_queue CommandQueue, Args const& args, cl_command_type type)
        : Task(Parent, type, CommandQueue)
        , m_Source(&Source)
        , m_Dest(&Dest)
        , m_Args(args)
    {
        D3D12_RESOURCE_DESC ImageDesc = {};
        auto& image = m_Source->m_Desc.image_type == CL_MEM_OBJECT_BUFFER ? *m_Dest.Get() : *m_Source.Get();
        ImageDesc.Dimension = image.m_CreationArgs.ResourceDimension12();
        ImageDesc.SampleDesc.Count = 1;
        ImageDesc.Width = m_Args.Width;
        ImageDesc.Height = m_Args.Height;
        ImageDesc.DepthOrArraySize = max((cl_ushort)m_Args.Depth, m_Args.NumArraySlices);
        ImageDesc.MipLevels = 1;
        ImageDesc.Format = image.m_CreationArgs.m_appDesc.Format();
        UINT64 RowPitch, TotalSize;
        m_CommandQueue->GetD3DDevice().GetDevice()->GetCopyableFootprints(&ImageDesc, m_Args.FirstImageArraySlice, m_Args.NumArraySlices, 0, nullptr, nullptr, &RowPitch, &TotalSize);
        m_CommandQueue->GetD3DDevice().GetDevice()->GetCopyableFootprints(&ImageDesc, 0, 1, 0, &m_BufferFootprint, nullptr, nullptr, nullptr);
        assert(m_Args.BufferPitch == RowPitch);
        if (m_Args.BufferPitch != m_BufferFootprint.Footprint.RowPitch ||
            (m_Args.NumArraySlices > 1 &&
             (m_Args.BufferPitch * m_Args.Height) % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0))
        {
            m_Temp.Attach(static_cast<Resource*>(clCreateBuffer(&m_Parent.get(), 0, (size_t)TotalSize, nullptr, nullptr)));
        }
    }

private:
    Resource::ref_ptr_int m_Source;
    Resource::ref_ptr_int m_Dest;
    Resource::ref_ptr m_Temp;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_BufferFootprint;
    const Args m_Args;

    void FillBufferDesc(D3D12_TEXTURE_COPY_LOCATION& Buffer, size_t BufferOffset)
    {
        Buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Buffer.PlacedFootprint = m_BufferFootprint;
        Buffer.PlacedFootprint.Offset = BufferOffset;
    }
    void MoveToNextArraySlice(D3D12_TEXTURE_COPY_LOCATION& Desc, UINT MipLevels)
    {
        if (Desc.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
        {
            Desc.PlacedFootprint.Offset += 
                D3D12TranslationLayer::Align(Desc.PlacedFootprint.Footprint.RowPitch * Desc.PlacedFootprint.Footprint.Height,
                                             (UINT)D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        }
        else
        {
            Desc.SubresourceIndex += MipLevels;
        }
    }
    void MigrateResources() final
    {
        m_Source->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
        m_Dest->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
        if (m_Temp.Get())
            m_Temp->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
    }
    void RecordImpl() final
    {
        D3D12TranslationLayer::Resource *UnderlyingSrc = m_Source->GetActiveUnderlyingResource(),
            *UnderlyingDest = m_Dest->GetActiveUnderlyingResource();
        if (m_Temp.Get() && m_Source->m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
        {
            CopyBufferRectTask::Args CopyRectArgs = {};
            CopyRectArgs.SrcOffset = (cl_uint)m_Args.BufferOffset;
            CopyRectArgs.SrcBufferRowPitch = m_Args.BufferPitch;
            CopyRectArgs.SrcBufferSlicePitch = m_Args.BufferPitch * m_Args.Height;
            CopyRectArgs.Width = m_Args.Width;
            CopyRectArgs.Height = m_Args.Height;
            CopyRectArgs.Depth = m_Args.Depth;
            CopyRectArgs.DstBufferRowPitch = m_BufferFootprint.Footprint.RowPitch;
            CopyRectArgs.DstBufferSlicePitch = m_BufferFootprint.Footprint.RowPitch * m_Args.Height;
            CopyBufferRectTask(m_Parent.get(), *m_Source.Get(), *m_Temp.Get(), m_CommandQueue.Get(), CopyRectArgs).Record();

            UnderlyingSrc = m_Temp->GetActiveUnderlyingResource();
        }
        else if (m_Temp.Get())
        {
            UnderlyingDest = m_Temp->GetActiveUnderlyingResource();
        }

        D3D12_TEXTURE_COPY_LOCATION Src, Dest;
        D3D12TranslationLayer::CViewSubresourceSubset SrcSubresources, DestSubresources;
        UINT DstX = 0, DstY = 0, DstZ = 0;
        D3D12_BOX SrcBox;
        if (m_Source->m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
        {
            FillBufferDesc(Src, m_Temp.Get() ? 0 : m_Source->m_Offset);
            Src.pResource = UnderlyingSrc->GetUnderlyingResource();
            SrcSubresources = D3D12TranslationLayer::CViewSubresourceSubset(D3D12TranslationLayer::CBufferView{});
            SrcBox = { 0, 0, 0, m_Args.Width, m_Args.Height, m_Args.Depth };

            Dest = CD3DX12_TEXTURE_COPY_LOCATION(UnderlyingDest->GetUnderlyingResource(), m_Args.FirstImageArraySlice);
            DestSubresources = D3D12TranslationLayer::CViewSubresourceSubset(
                D3D12TranslationLayer::CSubresourceSubset(1, m_Args.NumArraySlices, 1, m_Args.FirstImageMipLevel, m_Args.FirstImageArraySlice, 0),
                (UINT8)m_Dest->m_CreationArgs.m_desc12.MipLevels, (UINT16)m_Dest->m_Desc.image_array_size, 1);
            DstX = m_Args.ImageX;
            DstY = m_Args.ImageY;
            DstZ = m_Args.ImageZ;
        }
        else
        {
            Src = CD3DX12_TEXTURE_COPY_LOCATION(UnderlyingSrc->GetUnderlyingResource(), m_Args.FirstImageArraySlice);
            SrcSubresources = D3D12TranslationLayer::CViewSubresourceSubset(
                D3D12TranslationLayer::CSubresourceSubset(1, m_Args.NumArraySlices, 1, m_Args.FirstImageMipLevel, m_Args.FirstImageArraySlice, 0),
                (UINT8)m_Source->m_CreationArgs.m_desc12.MipLevels, (UINT16)m_Source->m_Desc.image_array_size, 1);
            SrcBox =
            {
                m_Args.ImageX,
                m_Args.ImageY,
                m_Args.ImageZ,
                m_Args.ImageX + m_Args.Width,
                m_Args.ImageY + m_Args.Height,
                m_Args.ImageZ + m_Args.Depth
            };

            FillBufferDesc(Dest, m_Temp.Get() ? 0 : m_Dest->m_Offset);
            Dest.pResource = UnderlyingDest->GetUnderlyingResource();
            DestSubresources = D3D12TranslationLayer::CViewSubresourceSubset(D3D12TranslationLayer::CBufferView{});
        }

        auto& ImmCtx = m_CommandQueue->GetD3DDevice().ImmCtx();
        ImmCtx.GetResourceStateManager().TransitionSubresources(UnderlyingSrc, SrcSubresources, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ImmCtx.GetResourceStateManager().TransitionSubresources(UnderlyingDest, DestSubresources, D3D12_RESOURCE_STATE_COPY_DEST);
        ImmCtx.GetResourceStateManager().ApplyAllResourceTransitions();
        for (cl_ushort i = 0; i < m_Args.NumArraySlices; ++i)
        {
            ImmCtx.GetGraphicsCommandList()->CopyTextureRegion(&Dest, DstX, DstY, DstZ, &Src, &SrcBox);
            MoveToNextArraySlice(Src, m_Source->m_CreationArgs.m_desc12.MipLevels);
            MoveToNextArraySlice(Dest, m_Dest->m_CreationArgs.m_desc12.MipLevels);
        }
        ImmCtx.PostCopy(UnderlyingSrc, SrcSubresources.begin().StartSubresource(),
                        UnderlyingDest, DestSubresources.begin().StartSubresource(),
                        m_Args.NumArraySlices);

        if (m_Temp.Get() && m_Source->m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
        {
            CopyBufferRectTask::Args CopyRectArgs = {};
            CopyRectArgs.DstOffset = (cl_uint)m_Args.BufferOffset;
            CopyRectArgs.DstBufferRowPitch = m_Args.BufferPitch;
            CopyRectArgs.DstBufferSlicePitch = m_Args.BufferPitch * m_Args.Height;
            CopyRectArgs.Width = m_Args.Width;
            CopyRectArgs.Height = m_Args.Height;
            CopyRectArgs.Depth = m_Args.Depth;
            CopyRectArgs.SrcBufferRowPitch = m_BufferFootprint.Footprint.RowPitch;
            CopyRectArgs.SrcBufferSlicePitch = m_BufferFootprint.Footprint.RowPitch * m_Args.Height;
            CopyBufferRectTask(m_Parent.get(), *m_Temp.Get(), *m_Dest.Get(), m_CommandQueue.Get(), CopyRectArgs).Record();
        }
    }
    void OnComplete() final
    {
        m_Source.Release();
        m_Dest.Release();
    }
};

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyImageToBuffer(cl_command_queue command_queue,
    cl_mem           src_image,
    cl_mem           dst_buffer,
    const size_t* src_origin,
    const size_t* region,
    size_t           dst_offset,
    cl_uint          num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (!src_image || !dst_buffer)
    {
        return ReportError("src_image and dst_buffer must not be NULL.", CL_INVALID_MEM_OBJECT);
    }

    Resource& image = *static_cast<Resource*>(src_image);
    Resource& buffer = *static_cast<Resource*>(dst_buffer);
    if (image.m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("src_image must be an image.", CL_INVALID_MEM_OBJECT);
    }
    if (buffer.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("dst_buffer must be a buffer.", CL_INVALID_MEM_OBJECT);
    }

    if (&buffer.m_Parent.get() != &context ||
        &image.m_Parent.get() != &context)
    {
        return ReportError("Both the buffer and image must belong to the same context as the queue.", CL_INVALID_CONTEXT);
    }

    CopyBufferAndImageTask::Args CmdArgs = {};
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    auto imageResult = ProcessImageDimensions(ReportError, src_origin, region, image,
                                              CmdArgs.FirstImageArraySlice, CmdArgs.NumArraySlices, CmdArgs.FirstImageMipLevel,
                                              CmdArgs.Height, CmdArgs.Depth, CmdArgs.ImageY, CmdArgs.ImageZ);
    if (imageResult != CL_SUCCESS)
    {
        return imageResult;
    }

    cl_uint elementSize = GetFormatSizeBytes(image.m_Format);
    size_t rowPitch = elementSize * CmdArgs.Width;
    CmdArgs.BufferPitch = (cl_uint)rowPitch;

    size_t slicePitch = elementSize * CmdArgs.Height;

    size_t bufferSize = slicePitch * CmdArgs.Depth * CmdArgs.NumArraySlices;
    if (dst_offset > buffer.m_Desc.image_width ||
        bufferSize > buffer.m_Desc.image_width ||
        dst_offset + bufferSize > buffer.m_Desc.image_width)
    {
        return ReportError("dst_offset cannot exceed the buffer bounds.", CL_INVALID_VALUE);
    }
    CmdArgs.BufferOffset = dst_offset;

    try
    {
        std::unique_ptr<Task> task(new CopyBufferAndImageTask(context, image, buffer, command_queue, CmdArgs, CL_COMMAND_COPY_IMAGE_TO_BUFFER));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyBufferToImage(cl_command_queue command_queue,
    cl_mem           src_buffer,
    cl_mem           dst_image,
    size_t           src_offset,
    const size_t* dst_origin,
    const size_t* region,
    cl_uint          num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (!src_buffer || !dst_image)
    {
        return ReportError("dst_image and src_buffer must not be NULL.", CL_INVALID_MEM_OBJECT);
    }

    Resource& image = *static_cast<Resource*>(dst_image);
    Resource& buffer = *static_cast<Resource*>(src_buffer);
    if (image.m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("src_image must be an image.", CL_INVALID_MEM_OBJECT);
    }
    if (buffer.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("dst_buffer must be a buffer.", CL_INVALID_MEM_OBJECT);
    }

    if (&buffer.m_Parent.get() != &context ||
        &image.m_Parent.get() != &context)
    {
        return ReportError("Both the buffer and image must belong to the same context as the queue.", CL_INVALID_CONTEXT);
    }

    CopyBufferAndImageTask::Args CmdArgs = {};
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    auto imageResult = ProcessImageDimensions(ReportError, dst_origin, region, image,
                                              CmdArgs.FirstImageArraySlice, CmdArgs.NumArraySlices, CmdArgs.FirstImageMipLevel,
                                              CmdArgs.Height, CmdArgs.Depth, CmdArgs.ImageY, CmdArgs.ImageZ);
    if (imageResult != CL_SUCCESS)
    {
        return imageResult;
    }

    cl_uint elementSize = GetFormatSizeBytes(image.m_Format);
    size_t rowPitch = elementSize * CmdArgs.Width;
    CmdArgs.BufferPitch = (cl_uint)rowPitch;

    size_t slicePitch = elementSize * CmdArgs.Height;

    size_t bufferSize = slicePitch * CmdArgs.Depth * CmdArgs.NumArraySlices;
    if (src_offset > buffer.m_Desc.image_width ||
        bufferSize > buffer.m_Desc.image_width ||
        src_offset + bufferSize > buffer.m_Desc.image_width)
    {
        return ReportError("dst_offset cannot exceed the buffer bounds.", CL_INVALID_VALUE);
    }
    CmdArgs.BufferOffset = src_offset;

    try
    {
        std::unique_ptr<Task> task(new CopyBufferAndImageTask(context, buffer, image, command_queue, CmdArgs, CL_COMMAND_COPY_BUFFER_TO_IMAGE));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
    return CL_SUCCESS;
}

MapTask::MapTask(Context& Parent, cl_command_queue command_queue, Resource& resource, cl_map_flags flags, cl_command_type command, Args const& args)
    : Task(Parent, command, command_queue)
    , m_Resource(resource)
    , m_Args(args)
    , m_MapFlags(flags)
{
    m_Resource.AddInternalRef();
}

MapTask::~MapTask()
{
    if (GetState() == State::Queued ||
        GetState() == State::Submitted ||
        GetState() == State::Ready ||
        GetState() == State::Running)
    {
        m_Resource.ReleaseInternalRef();
    }
}

void MapTask::OnComplete()
{
    m_Resource.ReleaseInternalRef();
}

void MapTask::MigrateResources()
{
    m_Resource.EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
}

class MapUseHostPtrResourceTask : public MapTask
{
public:
    MapUseHostPtrResourceTask(Context& Parent, cl_command_queue command_queue, cl_map_flags flags, Resource& resource, Args const& args, cl_command_type command)
        : MapTask(Parent, command_queue, resource, flags, command, args)
    {
        // If subbuffer, the args here have the offset applied in the SrcX so don't double-apply it with the sub-buffer's offset too
        void* basePtr = resource.m_ParentBuffer.Get() ? resource.m_ParentBuffer->m_pHostPointer : resource.m_pHostPointer;
        m_Pointer = (byte*)basePtr +
            resource.m_Desc.image_slice_pitch * (m_Args.SrcZ + m_Args.FirstArraySlice) +
            resource.m_Desc.image_row_pitch * m_Args.SrcY +
            GetFormatSizeBytes(resource.m_Format) * m_Args.SrcX;
        m_RowPitch = resource.m_Desc.image_row_pitch;
        m_SlicePitch = resource.m_Desc.image_slice_pitch;
    }

private:
    void RecordImpl() final
    {
        // Always read back data so we don't write garbage into regions the app didn't write
        {
            MemReadTask::Args ReadArgs = {};
            ReadArgs.SrcX = ReadArgs.DstX = m_Args.SrcX;
            ReadArgs.SrcY = ReadArgs.DstY = m_Args.SrcY;
            ReadArgs.SrcZ = ReadArgs.DstZ = m_Args.SrcZ;
            ReadArgs.Width = m_Args.Width;
            ReadArgs.Height = m_Args.Height;
            ReadArgs.Depth = m_Args.Depth;
            ReadArgs.FirstArraySlice = m_Args.FirstArraySlice;
            ReadArgs.NumArraySlices = m_Args.NumArraySlices;
            assert(m_Args.FirstMipLevel == 0);
            ReadArgs.pData = m_Resource.m_pHostPointer;
            ReadArgs.DstRowPitch = (cl_uint)m_Resource.m_Desc.image_row_pitch;
            ReadArgs.DstSlicePitch = (cl_uint)m_Resource.m_Desc.image_slice_pitch;
            MemReadTask(m_Parent.get(), m_Resource, CL_COMMAND_READ_BUFFER, m_CommandQueue.Get(), ReadArgs).Record();
        }
    }
    void Unmap(bool IsResourceBeingDestroyed) final
    {
        // Don't create the write-back task if the resource is being destroyed.
        // A) This is an optimization since clearly the resource contents don't need to be updated.
        // B) The task would add-ref the resource, which would result in a double-delete
        if ((m_MapFlags & CL_MAP_WRITE) && !IsResourceBeingDestroyed)
        {
            MemWriteFillTask::Args WriteArgs = {};
            MemWriteFillTask::WriteData PointerArgs = {};
            PointerArgs.pData = m_Resource.m_pHostPointer;
            PointerArgs.RowPitch = (cl_uint)m_Resource.m_Desc.image_row_pitch;
            PointerArgs.SlicePitch = (cl_uint)m_Resource.m_Desc.image_slice_pitch;
            WriteArgs.Data = PointerArgs;
            WriteArgs.SrcX = WriteArgs.DstX = m_Args.SrcX;
            WriteArgs.SrcY = WriteArgs.DstY = m_Args.SrcY;
            WriteArgs.SrcZ = WriteArgs.DstZ = m_Args.SrcZ;
            WriteArgs.Width = m_Args.Width;
            WriteArgs.Height = m_Args.Height;
            WriteArgs.Depth = m_Args.Depth;
            WriteArgs.FirstArraySlice = m_Args.FirstArraySlice;
            WriteArgs.NumArraySlices = m_Args.NumArraySlices;
            assert(m_Args.FirstMipLevel == 0);
            MemWriteFillTask(m_Parent.get(), m_Resource, CL_COMMAND_WRITE_BUFFER, m_CommandQueue.Get(), WriteArgs, true).Record();
        }
    }
};

constexpr static D3D12_RANGE EmptyRange = {};
class MapSynchronizeTask : public MapTask
{
public:
    MapSynchronizeTask(Context& Parent, cl_command_queue command_queue, cl_map_flags flags, Resource& resource, Args const& args, cl_command_type command)
        : MapTask(Parent, command_queue, resource, flags, command, args)
    {
        void* basePointer = nullptr;
        auto& Device = m_CommandQueue->GetD3DDevice();
        UINT subresource = args.FirstArraySlice * resource.m_CreationArgs.m_appDesc.m_MipLevels + args.FirstMipLevel;
        D3D12TranslationLayer::ThrowFailure(resource.GetUnderlyingResource(&Device)->GetUnderlyingResource()->Map(0, &EmptyRange, &basePointer));
        auto& Placement = resource.GetUnderlyingResource(&Device)->GetSubresourcePlacement(subresource);
        m_RowPitch = Placement.Footprint.RowPitch;
        m_SlicePitch = args.NumArraySlices > 1 ?
            (UINT)(resource.GetUnderlyingResource(&Device)->GetSubresourcePlacement(subresource + 1).Offset - Placement.Offset) :
            resource.GetUnderlyingResource(&Device)->DepthPitch(subresource);

        m_Pointer = (byte*)basePointer +
            m_SlicePitch * m_Args.SrcZ +
            m_RowPitch * m_Args.SrcY +
            GetFormatSizeBytes(resource.m_Format) * m_Args.SrcX +
            resource.GetUnderlyingResource(&Device)->GetSubresourcePlacement(subresource).Offset;
    }

private:
    void RecordImpl() final
    {
        D3D12TranslationLayer::MAP_TYPE MapType = [](cl_map_flags flags)
        {
            switch (flags)
            {
            default:
            case CL_MAP_READ | CL_MAP_WRITE: return D3D12TranslationLayer::MAP_TYPE_READ;
            case CL_MAP_READ: return D3D12TranslationLayer::MAP_TYPE_READWRITE;
            case CL_MAP_WRITE: return D3D12TranslationLayer::MAP_TYPE_WRITE;
            }
        }(m_MapFlags);
        for (cl_uint i = 0; i < m_Args.NumArraySlices; ++i)
        {
            UINT subresource = (m_Args.FirstArraySlice + i) * m_Resource.m_CreationArgs.m_appDesc.m_MipLevels + m_Args.FirstMipLevel;
            m_Resource.GetActiveUnderlyingResource()->m_pParent->SynchronizeForMap(
                m_Resource.GetActiveUnderlyingResource(),
                subresource, MapType, false);
        };
    }
    void Unmap([[maybe_unused]] bool IsResourceBeingDestroyed) final
    {
        m_Resource.GetActiveUnderlyingResource()->GetUnderlyingResource()->Unmap(0, &EmptyRange);
    }
};

class MapCopyTask : public MapTask
{
public:
    MapCopyTask(Context& Parent, cl_command_queue command_queue, cl_map_flags flags, Resource& resource, Args const& args, cl_command_type command)
        : MapTask(Parent, command_queue, resource, flags, command, args)
    {
        D3D12TranslationLayer::ResourceCreationArgs Args = resource.m_CreationArgs;
        Args.m_appDesc.m_Subresources = args.NumArraySlices;
        Args.m_appDesc.m_SubresourcesPerPlane = args.NumArraySlices;
        Args.m_appDesc.m_ArraySize = args.NumArraySlices;
        Args.m_appDesc.m_MipLevels = 1;
        Args.m_appDesc.m_Depth = args.Depth;
        Args.m_appDesc.m_Width = args.Width;
        Args.m_appDesc.m_Height = args.Height;
        Args.m_appDesc.m_usage = D3D12TranslationLayer::RESOURCE_USAGE_STAGING;
        Args.m_appDesc.m_bindFlags = D3D12TranslationLayer::RESOURCE_BIND_NONE;
        Args.m_appDesc.m_cpuAcess = D3D12TranslationLayer::RESOURCE_CPU_ACCESS_READ | D3D12TranslationLayer::RESOURCE_CPU_ACCESS_WRITE;
        Args.m_heapDesc = CD3DX12_HEAP_DESC(0, D3D12_HEAP_TYPE_READBACK);
        Args.m_desc12.Flags = D3D12_RESOURCE_FLAG_NONE;

        cl_mem_flags stagingFlags = CL_MEM_ALLOC_HOST_PTR;
        if (resource.m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
        {
            m_MappableResource.Attach(Resource::CreateBuffer(Parent, Args, nullptr, stagingFlags, nullptr));
        }
        else
        {
            cl_image_desc NewDesc = resource.m_Desc;
            NewDesc.image_width = args.Width;
            NewDesc.image_height = args.Height;
            NewDesc.image_depth = args.Depth;
            NewDesc.image_array_size = args.NumArraySlices;
            NewDesc.image_row_pitch = 0;
            NewDesc.image_slice_pitch = 0;
            m_MappableResource.Attach(Resource::CreateImage(Parent, Args, nullptr, resource.m_Format, NewDesc, stagingFlags, nullptr));
        }

        m_MappableResource->EnqueueMigrateResource(&m_CommandQueue->GetD3DDevice(), this, 0);
        auto UnderlyingMapArgs = args;
        UnderlyingMapArgs.SrcX = 0;
        UnderlyingMapArgs.SrcY = 0;
        UnderlyingMapArgs.SrcZ = 0;
        UnderlyingMapArgs.FirstArraySlice = 0;
        UnderlyingMapArgs.FirstMipLevel = 0;
        m_UnderlyingMapTask.reset(new MapSynchronizeTask(Parent, command_queue, flags, *m_MappableResource.Get(), UnderlyingMapArgs, command));
        m_RowPitch = m_UnderlyingMapTask->GetRowPitch();
        m_SlicePitch = m_UnderlyingMapTask->GetSlicePitch();
        m_Pointer = m_UnderlyingMapTask->GetPointer();
    }

private:
    void RecordImpl() final
    {
        // Always read back data so we don't write garbage into regions the app didn't write
        {
            CopyResourceTask::Args CopyArgs = {};
            // Leave Dst coords as 0
            CopyArgs.SrcX = m_Args.SrcX;
            CopyArgs.SrcY = m_Args.SrcY;
            CopyArgs.SrcZ = m_Args.SrcZ;
            CopyArgs.FirstSrcArraySlice = m_Args.FirstArraySlice;
            CopyArgs.FirstSrcMipLevel = m_Args.FirstMipLevel;
            CopyArgs.Width = m_Args.Width;
            CopyArgs.Height = m_Args.Height;
            CopyArgs.Depth = m_Args.Depth;
            CopyArgs.NumArraySlices = m_Args.NumArraySlices;
            CopyResourceTask(m_Parent.get(), m_Resource, *m_MappableResource.Get(), m_CommandQueue.Get(), CopyArgs, CL_COMMAND_COPY_IMAGE).Record();
        }
        m_UnderlyingMapTask->Record();
    }
    void Unmap(bool IsResourceBeingDestroyed) final
    {
        static_cast<MapTask*>(m_UnderlyingMapTask.get())->Unmap(IsResourceBeingDestroyed);
        if ((m_MapFlags & CL_MAP_WRITE) && !IsResourceBeingDestroyed)
        {
            CopyResourceTask::Args CopyArgs = {};
            // Leave Src coords as 0
            CopyArgs.DstX = m_Args.SrcX;
            CopyArgs.DstY = m_Args.SrcY;
            CopyArgs.DstZ = m_Args.SrcZ;
            CopyArgs.FirstDstArraySlice = m_Args.FirstArraySlice;
            CopyArgs.FirstDstMipLevel = m_Args.FirstMipLevel;
            CopyArgs.Width = m_Args.Width;
            CopyArgs.Height = m_Args.Height;
            CopyArgs.Depth = m_Args.Depth;
            CopyArgs.NumArraySlices = m_Args.NumArraySlices;
            CopyResourceTask(m_Parent.get(), *m_MappableResource.Get(), m_Resource, m_CommandQueue.Get(), CopyArgs, CL_COMMAND_COPY_IMAGE).Record();
        }

        m_MappableResource.Release();
        m_UnderlyingMapTask.reset();
    }

    Resource::ref_ptr m_MappableResource;
    std::unique_ptr<MapSynchronizeTask> m_UnderlyingMapTask;
};

extern CL_API_ENTRY void * CL_API_CALL
clEnqueueMapBuffer(cl_command_queue command_queue,
    cl_mem           buffer,
    cl_bool          blocking_map,
    cl_map_flags     map_flags,
    size_t           offset,
    size_t           size,
    cl_uint          num_events_in_wait_list,
    const cl_event * event_wait_list,
    cl_event *       event,
    cl_int *         errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_COMMAND_QUEUE;
        return nullptr;
    }
    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter(errcode_ret);
    if (!buffer)
    {
        return ReportError("buffer must not be null.", CL_INVALID_MEM_OBJECT);
    }

    Resource& resource = *static_cast<Resource*>(buffer);
    if (resource.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("buffer must be a buffer object.", CL_INVALID_MEM_OBJECT);
    }
    if (&resource.m_Parent.get() != &context)
    {
        return ReportError("buffer must belong to the same context as the queue.", CL_INVALID_CONTEXT);
    }

    if ((resource.m_Flags & CL_MEM_HOST_NO_ACCESS) ||
        ((resource.m_Flags & CL_MEM_HOST_READ_ONLY) && (map_flags & (CL_MAP_WRITE | CL_MAP_WRITE_INVALIDATE_REGION))) ||
        ((resource.m_Flags & CL_MEM_HOST_WRITE_ONLY) && (map_flags & CL_MAP_READ)))
    {
        return ReportError("Resource flags preclude operation requested by map flags.", CL_INVALID_OPERATION);
    }

    if (offset > resource.m_Desc.image_width ||
        size > resource.m_Desc.image_width ||
        offset + size > resource.m_Desc.image_width)
    {
        return ReportError("offset and size must fit within the resource size.", CL_INVALID_VALUE);
    }

    switch (map_flags)
    {
    case CL_MAP_WRITE_INVALIDATE_REGION:
        // TODO: Support buffer renaming if we're invalidating a whole buffer
        map_flags = CL_MAP_WRITE;
        break;
    case CL_MAP_READ:
    case CL_MAP_WRITE:
    case CL_MAP_READ | CL_MAP_WRITE:
        break;
    default:
        return ReportError("map_flags must contain read and/or write bits, or must be equal to CL_MAP_WRITE_INVALIDATE_REGION.", CL_INVALID_VALUE);
    }

    MapTask::Args CmdArgs = {};
    CmdArgs.SrcX = (cl_uint)(offset + resource.m_Offset);
    CmdArgs.Width = (cl_uint)size;
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    try
    {
        std::unique_ptr<MapTask> task;
        if (resource.m_Flags & CL_MEM_USE_HOST_PTR)
        {
            task.reset(new MapUseHostPtrResourceTask(context, command_queue, map_flags, resource, CmdArgs, CL_COMMAND_MAP_BUFFER));
        }
        else if (resource.m_Flags & CL_MEM_ALLOC_HOST_PTR)
        {
            task.reset(new MapSynchronizeTask(context, command_queue, map_flags, resource, CmdArgs, CL_COMMAND_MAP_BUFFER));
        }
        else
        {
            task.reset(new MapCopyTask(context, command_queue, map_flags, resource, CmdArgs, CL_COMMAND_MAP_BUFFER));
        }

        resource.AddMapTask(task.get());
        auto RemoveMapTask = wil::scope_exit([&task, &resource]()
        {
            resource.RemoveMapTask(task.get());
        });

        {
            auto Lock = g_Platform->GetTaskPoolLock();
            task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
            queue.QueueTask(task.get(), Lock);
            if (blocking_map)
            {
                queue.Flush(Lock, /* flushDevice */ true);
            }
        }

        cl_int taskError = CL_SUCCESS;
        if (blocking_map)
        {
            taskError = task->WaitForCompletion();
        }

        // No more exceptions
        if (errcode_ret)
        {
            *errcode_ret = taskError;
        }

        auto pointer = task->GetPointer();

        if (event)
            *event = task.release();
        else
            task.release()->Release();
        RemoveMapTask.release();

        return pointer;
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
}

extern CL_API_ENTRY void * CL_API_CALL
clEnqueueMapImage(cl_command_queue  command_queue,
    cl_mem            image,
    cl_bool           blocking_map,
    cl_map_flags      map_flags,
    const size_t *    origin,
    const size_t *    region,
    size_t *          image_row_pitch,
    size_t *          image_slice_pitch,
    cl_uint           num_events_in_wait_list,
    const cl_event *  event_wait_list,
    cl_event *        event,
    cl_int *          errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_COMMAND_QUEUE;
        return nullptr;
    }
    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter(errcode_ret);
    if (!image)
    {
        return ReportError("image must not be null.", CL_INVALID_MEM_OBJECT);
    }

    Resource& resource = *static_cast<Resource*>(image);
    if (resource.m_Desc.image_type == CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("image must not be a buffer object.", CL_INVALID_MEM_OBJECT);
    }
    if (&resource.m_Parent.get() != &context)
    {
        return ReportError("image must belong to the same context as the queue.", CL_INVALID_CONTEXT);
    }

    if ((resource.m_Flags & CL_MEM_HOST_NO_ACCESS) ||
        ((resource.m_Flags & CL_MEM_HOST_READ_ONLY) && (map_flags & (CL_MAP_WRITE | CL_MAP_WRITE_INVALIDATE_REGION))) ||
        ((resource.m_Flags & CL_MEM_HOST_WRITE_ONLY) && (map_flags & CL_MAP_READ)))
    {
        return ReportError("Resource flags preclude operation requested by map flags.", CL_INVALID_OPERATION);
    }

    switch (map_flags)
    {
    case CL_MAP_WRITE_INVALIDATE_REGION:
        // TODO: Support buffer renaming if we're invalidating a whole buffer
        map_flags = CL_MAP_WRITE;
        break;
    case CL_MAP_READ:
    case CL_MAP_WRITE:
    case CL_MAP_READ | CL_MAP_WRITE:
        break;
    default:
        return ReportError("map_flags must contain read and/or write bits, or must be equal to CL_MAP_WRITE_INVALIDATE_REGION.", CL_INVALID_VALUE);
    }

    MapTask::Args CmdArgs = {};
    CmdArgs.SrcX = (cl_uint)origin[0];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    cl_int imageResult = ProcessImageDimensions(context.GetErrorReporter(), origin, region, resource,
                                                CmdArgs.FirstArraySlice, CmdArgs.NumArraySlices, CmdArgs.FirstMipLevel,
                                                CmdArgs.Height, CmdArgs.Depth, CmdArgs.SrcY, CmdArgs.SrcZ);

    if (imageResult != CL_SUCCESS)
    {
        if (errcode_ret)
            *errcode_ret = imageResult;
        return nullptr;
    }

    try
    {
        std::unique_ptr<MapTask> task;
        if (resource.m_Flags & CL_MEM_USE_HOST_PTR)
        {
            task.reset(new MapUseHostPtrResourceTask(context, command_queue, map_flags, resource, CmdArgs, CL_COMMAND_MAP_IMAGE));
        }
        else
        {
            task.reset(new MapCopyTask(context, command_queue, map_flags, resource, CmdArgs, CL_COMMAND_MAP_IMAGE));
        }

        resource.AddMapTask(task.get());
        auto RemoveMapTask = wil::scope_exit([&task, &resource]()
        {
            resource.RemoveMapTask(task.get());
        });

        {
            auto Lock = g_Platform->GetTaskPoolLock();
            task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
            queue.QueueTask(task.get(), Lock);
            if (blocking_map)
            {
                queue.Flush(Lock, /* flushDevice */ true);
            }
        }

        cl_int taskError = CL_SUCCESS;
        if (blocking_map)
        {
            taskError = task->WaitForCompletion();
        }

        // No more exceptions
        if (errcode_ret)
        {
            *errcode_ret = taskError;
        }

        auto pointer = task->GetPointer();
        if (image_slice_pitch)
            *image_slice_pitch = task->GetSlicePitch();
        if (image_row_pitch)
            *image_row_pitch = task->GetRowPitch();

        if (event)
            *event = task.release();
        else
            task.release()->Release();
        RemoveMapTask.release();

        return pointer;
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
}

class UnmapTask : public Task
{
public:
    UnmapTask(Context& Parent, cl_command_queue command_queue, MapTask* task)
        : Task(Parent, CL_COMMAND_UNMAP_MEM_OBJECT, command_queue)
        , m_MapTask(task)
        , m_Resource(&task->GetResource())
    {
    }

private:
    ::ref_ptr_int<MapTask> m_MapTask;
    Resource::ref_ptr_int m_Resource;

    void MigrateResources() final
    {
    }
    void RecordImpl() final
    {
        m_MapTask->Unmap(false);
    }
    void OnComplete() final
    {
        m_MapTask.Release();
        m_Resource.Release();
    }
};

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueUnmapMemObject(cl_command_queue command_queue,
    cl_mem           memobj,
    void *           mapped_ptr,
    cl_uint          num_events_in_wait_list,
    const cl_event * event_wait_list,
    cl_event *       event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    auto& queue = *static_cast<CommandQueue*>(command_queue);
    auto& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (!memobj)
    {
        return ReportError("memobj must not be null.", CL_INVALID_MEM_OBJECT);
    }

    Resource& resource = *static_cast<Resource*>(memobj);
    if (&resource.m_Parent.get() != &context)
    {
        return ReportError("memobj must belong to the same context as the queue.", CL_INVALID_CONTEXT);
    }

    MapTask* mapTask = resource.GetMapTask(mapped_ptr);
    if (!mapTask)
    {
        return ReportError("mapped_ptr must be a valid pointer returned from a previous map operation.", CL_INVALID_VALUE);
    }

    try
    {
        std::unique_ptr<Task> task(new UnmapTask(context, command_queue, mapTask));
        auto Lock = g_Platform->GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
        resource.RemoveMapTask(mapTask);
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception& e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    catch (Task::DependencyException&) { return ReportError("Context mismatch between command_queue and event_wait_list", CL_INVALID_CONTEXT); }
    return CL_SUCCESS;
}

void MemReadTask::RecordViaCopy()
{
    MapTask::Args MapArgs = {};
    MapArgs.SrcX = m_Args.SrcX + (cl_uint)m_Source->m_Offset;
    MapArgs.SrcY = m_Args.SrcY;
    MapArgs.SrcZ = m_Args.SrcZ;
    MapArgs.Width = m_Args.Width;
    MapArgs.Height = m_Args.Height;
    MapArgs.Depth = m_Args.Depth;
    MapArgs.FirstArraySlice = m_Args.FirstArraySlice;
    MapArgs.NumArraySlices = m_Args.NumArraySlices;
    MapArgs.FirstMipLevel = m_Args.FirstMipLevel;
    if (m_CommandType == CL_COMMAND_READ_BUFFER_RECT)
    {
        MapArgs = {};
        MapArgs.SrcX = (cl_uint)m_Source->m_Offset;
        MapArgs.Width = (cl_uint)m_Source->m_Desc.image_width;
        MapArgs.Height = 1;
        MapArgs.Depth = 1;
        MapArgs.NumArraySlices = 1;
    }
    MapCopyTask MapCopy(m_Parent.get(), m_CommandQueue.Get(), CL_MAP_READ, *m_Source.Get(), MapArgs, CL_COMMAND_MAP_IMAGE);
    MapCopy.Record();

    MemReadTask::Args MemReadArgs = m_Args;
    if (m_CommandType != CL_COMMAND_READ_BUFFER_RECT)
    {
        MemReadArgs.SrcX = 0;
        MemReadArgs.SrcY = 0;
        MemReadArgs.SrcZ = 0;
        MemReadArgs.FirstArraySlice = 0;
        MemReadArgs.SrcBufferRowPitch = (cl_uint)MapCopy.GetRowPitch();
        MemReadArgs.SrcBufferSlicePitch = (cl_uint)MapCopy.GetSlicePitch();
    }
    MemReadTask Read(m_Parent.get(), *m_Source.Get(), m_CommandType, m_CommandQueue.Get(), MemReadArgs);
    for (cl_uint i = 0; i < MemReadArgs.NumArraySlices; ++i)
    {
        Read.CopyBits(MapCopy.GetPointer(), i + MemReadArgs.FirstArraySlice, MemReadArgs.SrcBufferRowPitch, MemReadArgs.SrcBufferSlicePitch);
    }

    static_cast<MapTask&>(MapCopy).Unmap(false);
}
