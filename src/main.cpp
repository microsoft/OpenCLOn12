// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "platform.hpp"

#include <windows.h>
#include <cstring>
#include <type_traits>
#include <memory>

// {1926C47A-D119-4A54-BA9C-D119D68F50A7}
TRACELOGGING_DEFINE_PROVIDER(
    g_hOpenCLOn12Provider,
    "Microsoft.OpenCLOn12",
    (0x1926c47a, 0xd119, 0x4a54, 0xba, 0x9c, 0xd1, 0x19, 0xd6, 0x8f, 0x50, 0xa7)
);

struct TraceLoggingRegistration
{
    TraceLoggingRegistration() { TraceLoggingRegister(g_hOpenCLOn12Provider); }
    ~TraceLoggingRegistration() { TraceLoggingUnregister(g_hOpenCLOn12Provider); }
} g_TraceLoggingRegistration;

struct ExtensionTableEntry
{
    const char *name;
    void *func;
};

#define EXT_FUNC(name) { #name, (void*)(name) }

static struct ExtensionTableEntry clExtensions[] =
{
    // cl_khr_icd
    EXT_FUNC(clIcdGetPlatformIDsKHR),

    // cl_khr_gl_sharing
    EXT_FUNC(clGetGLContextInfoKHR),
    EXT_FUNC(clCreateFromGLBuffer),
    EXT_FUNC(clCreateFromGLTexture),
    EXT_FUNC(clCreateFromGLTexture2D),
    EXT_FUNC(clCreateFromGLTexture3D),
    EXT_FUNC(clCreateFromGLRenderbuffer),
    EXT_FUNC(clEnqueueAcquireGLObjects),
    EXT_FUNC(clEnqueueReleaseGLObjects),
    EXT_FUNC(clGetGLObjectInfo),
    EXT_FUNC(clGetGLTextureInfo),

    // cl_khr_gl_event
    EXT_FUNC(clCreateEventFromGLsyncKHR),

    // cl_khr_il_program
    EXT_FUNC(clCreateProgramWithILKHR),
};

static const int clExtensionCount = sizeof(clExtensions) / sizeof(clExtensions[0]);

void * CL_API_CALL
clGetExtensionFunctionAddress(const char *name)
{
    int ii;

    for (ii = 0; ii < clExtensionCount; ii++) {
        if (!strcmp(name, clExtensions[ii].name)) {
            return clExtensions[ii].func;
        }
    }

    return nullptr;
}

/* Extension function access
*
* Returns the extension function address for the given function name,
* or NULL if a valid function can not be found.  The client must
* check to make sure the address is not NULL, before using or
* calling the returned function address.
*/
extern CL_API_ENTRY void * CL_API_CALL
clGetExtensionFunctionAddressForPlatform(cl_platform_id platform,
                                         const char *   func_name) CL_API_SUFFIX__VERSION_1_2
{
    if (platform != g_Platform)
    {
        return nullptr;
    }
    return clGetExtensionFunctionAddress(func_name);
}

cl_icd_dispatch g_DispatchTable
{
    /* OpenCL 1.0 */
    clGetPlatformIDs,
    clGetPlatformInfo,
    clGetDeviceIDs,
    clGetDeviceInfo,
    clCreateContext,
    clCreateContextFromType,
    clRetainContext,
    clReleaseContext,
    clGetContextInfo,
    clCreateCommandQueue,
    clRetainCommandQueue,
    clReleaseCommandQueue,
    clGetCommandQueueInfo,
    clSetCommandQueueProperty,
    clCreateBuffer,
    clCreateImage2D,
    clCreateImage3D,
    clRetainMemObject,
    clReleaseMemObject,
    clGetSupportedImageFormats,
    clGetMemObjectInfo,
    clGetImageInfo,
    clCreateSampler,
    clRetainSampler,
    clReleaseSampler,
    clGetSamplerInfo,
    clCreateProgramWithSource,
    clCreateProgramWithBinary,
    clRetainProgram,
    clReleaseProgram,
    clBuildProgram,
    clUnloadCompiler,
    clGetProgramInfo,
    clGetProgramBuildInfo,
    clCreateKernel,
    clCreateKernelsInProgram,
    clRetainKernel,
    clReleaseKernel,
    clSetKernelArg,
    clGetKernelInfo,
    clGetKernelWorkGroupInfo,
    clWaitForEvents,
    clGetEventInfo,
    clRetainEvent,
    clReleaseEvent,
    clGetEventProfilingInfo,
    clFlush,
    clFinish,
    clEnqueueReadBuffer,
    clEnqueueWriteBuffer,
    clEnqueueCopyBuffer,
    clEnqueueReadImage,
    clEnqueueWriteImage,
    clEnqueueCopyImage,
    clEnqueueCopyImageToBuffer,
    clEnqueueCopyBufferToImage,
    clEnqueueMapBuffer,
    clEnqueueMapImage,
    clEnqueueUnmapMemObject,
    clEnqueueNDRangeKernel,
    clEnqueueTask,
    clEnqueueNativeKernel,
    clEnqueueMarker,
    clEnqueueWaitForEvents,
    clEnqueueBarrier,
    clGetExtensionFunctionAddress,
    clCreateFromGLBuffer,
    clCreateFromGLTexture2D,
    clCreateFromGLTexture3D,
    clCreateFromGLRenderbuffer,
    clGetGLObjectInfo,
    clGetGLTextureInfo,
    clEnqueueAcquireGLObjects,
    clEnqueueReleaseGLObjects,
    clGetGLContextInfoKHR,

    /* cl_khr_d3d10_sharing */
    nullptr, // clGetDeviceIDsFromD3D10KHR,
    nullptr, // clCreateFromD3D10BufferKHR,
    nullptr, // clCreateFromD3D10Texture2DKHR,
    nullptr, // clCreateFromD3D10Texture3DKHR,
    nullptr, // clEnqueueAcquireD3D10ObjectsKHR,
    nullptr, // clEnqueueReleaseD3D10ObjectsKHR,

    /* OpenCL 1.1 */
    clSetEventCallback,
    clCreateSubBuffer,
    clSetMemObjectDestructorCallback,
    clCreateUserEvent,
    clSetUserEventStatus,
    clEnqueueReadBufferRect,
    clEnqueueWriteBufferRect,
    clEnqueueCopyBufferRect,

    /* cl_ext_device_fission */
    nullptr, // clCreateSubDevicesEXT,
    nullptr, // clRetainDeviceEXT,
    nullptr, // clReleaseDeviceEXT,

    /* cl_khr_gl_event */
    clCreateEventFromGLsyncKHR,

    /* OpenCL 1.2 */
    clCreateSubDevices,
    clRetainDevice,
    clReleaseDevice,
    clCreateImage,
    clCreateProgramWithBuiltInKernels,
    clCompileProgram,
    clLinkProgram,
    clUnloadPlatformCompiler,
    clGetKernelArgInfo,
    clEnqueueFillBuffer,
    clEnqueueFillImage,
    clEnqueueMigrateMemObjects,
    clEnqueueMarkerWithWaitList,
    clEnqueueBarrierWithWaitList,
    clGetExtensionFunctionAddressForPlatform,
    clCreateFromGLTexture,

    /* cl_khr_d3d11_sharing */
    nullptr, // clGetDeviceIDsFromD3D11KHR,
    nullptr, // clCreateFromD3D11BufferKHR,
    nullptr, // clCreateFromD3D11Texture2DKHR,
    nullptr, // clCreateFromD3D11Texture3DKHR,
    nullptr, // clCreateFromDX9MediaSurfaceKHR,
    nullptr, // clEnqueueAcquireD3D11ObjectsKHR,
    nullptr, // clEnqueueReleaseD3D11ObjectsKHR,

    /* cl_khr_dx9_media_sharing */
    nullptr, // clGetDeviceIDsFromDX9MediaAdapterKHR,
    nullptr, // clEnqueueAcquireDX9MediaSurfacesKHR,
    nullptr, // clEnqueueReleaseDX9MediaSurfacesKHR,

    /* cl_khr_egl_image */
    nullptr, // clCreateFromEGLImageKHR,
    nullptr, // clEnqueueAcquireEGLObjectsKHR,
    nullptr, // clEnqueueReleaseEGLObjectsKHR,

    /* cl_khr_egl_event */
    nullptr, // clCreateEventFromEGLSyncKHR,

    /* OpenCL 2.0 */
    clCreateCommandQueueWithProperties,
    clCreatePipe,
    clGetPipeInfo,
    clSVMAlloc,
    clSVMFree,
    clEnqueueSVMFree,
    clEnqueueSVMMemcpy,
    clEnqueueSVMMemFill,
    clEnqueueSVMMap,
    clEnqueueSVMUnmap,
    clCreateSamplerWithProperties,
    clSetKernelArgSVMPointer,
    clSetKernelExecInfo,

    /* cl_khr_sub_groups */
    nullptr, // clGetKernelSubGroupInfoKHR,

    /* OpenCL 2.1 */
    clCloneKernel,
    clCreateProgramWithIL,
    clEnqueueSVMMigrateMem,
    clGetDeviceAndHostTimer,
    clGetHostTimer,
    clGetKernelSubGroupInfo,
    clSetDefaultDeviceCommandQueue,

    /* OpenCL 2.2 */
    clSetProgramReleaseCallback,
    clSetProgramSpecializationConstant,

    /* OpenCL 3.0 */
    clCreateBufferWithProperties,
    clCreateImageWithProperties,
    clSetContextDestructorCallback,
};

Platform* g_Platform = nullptr;

CL_API_ENTRY cl_int CL_API_CALL
clIcdGetPlatformIDsKHR(cl_uint           num_entries,
    cl_platform_id * platforms,
    cl_uint *        num_platforms)
{
    if (!g_Platform)
    {
        try
        {
            g_Platform = new Platform(&g_DispatchTable);
        }
        catch (std::bad_alloc&) { return CL_OUT_OF_HOST_MEMORY; }
        catch (std::exception&) { return CL_OUT_OF_RESOURCES; }
    }

    if ((platforms && num_entries <= 0) ||
        (!platforms && num_entries >= 1))
    {
        return CL_INVALID_VALUE;
    }

    if (platforms && num_entries >= 1)
    {
        platforms[0] = g_Platform;
    }

    if (num_platforms)
    {
        *num_platforms = 1;
    }

    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetPlatformIDs(cl_uint          num_entries,
    cl_platform_id * platforms,
    cl_uint *        num_platforms) CL_API_SUFFIX__VERSION_1_0
{
    return clIcdGetPlatformIDsKHR(num_entries, platforms, num_platforms);
}

extern "C" extern BOOL WINAPI DllMain(HINSTANCE, UINT dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_DETACH)
    {
        if (!g_Platform)
            return TRUE;

        // If this is process termination, and we have D3D devices owned by
        // the platform, just go ahead and leak them, rather than trying
        // to clean them up.
        if (lpReserved && g_Platform->AnyD3DDevicesExist())
        {
            // At the very least close shader caches cleanly so they can be flushed
            // to disk.
            g_Platform->CloseCaches();
            return TRUE;
        }

        delete g_Platform;
    }

    return TRUE;
}

#ifndef HAS_TELASSERT
void __stdcall MicrosoftTelemetryAssertTriggeredNoArgs() { }
#endif

