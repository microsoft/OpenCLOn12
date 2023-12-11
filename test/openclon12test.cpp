// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "gtest/gtest.h"

#define CL_TARGET_OPENCL_VERSION 220
#include <CL/cl.h>

#define CL_HPP_ENABLE_EXCEPTIONS
#define CL_HPP_TARGET_OPENCL_VERSION 220
#define CL_HPP_MINIMUM_OPENCL_VERSION 220
#define CL_HPP_CL_1_2_DEFAULT_BUILD
#include "cl2.hpp"
#include <memory>
#include <utility>
#include <algorithm>
#include <numeric>

#include <d3d12.h>

#include <gl/GL.h>
#include "gl_tokens.hpp"

#include <wil/resource.h>

std::pair<cl::Context, cl::Device> GetWARPContext()
{
    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);

    if (platforms.size() != 1)
    {
        ADD_FAILURE() << "Unexpected platforms";
    }

    std::vector<cl::Device> devices;
    platforms[0].getDevices(CL_DEVICE_TYPE_CPU, &devices);
    if (devices.size() == 0)
    {
        ADD_FAILURE() << "Unexpected device count";
        return {};
    }

    cl::Device device;
    for (auto& d : devices)
    {
        auto vendor_id = d.getInfo<CL_DEVICE_VENDOR_ID>();

        if (vendor_id == 0x1414) // Microsoft
        {
            device = d;
            break;
        }
    }

    if (!device())
    {
        ADD_FAILURE() << "Couldn't find WARP";
        return {};
    }

    cl_context_properties context_props[] = { CL_CONTEXT_PLATFORM, (cl_context_properties)platforms[0](), 0 };
    cl::Context context(device, context_props,
        [](const char* msg, const void*, size_t, void*)
    {
        ADD_FAILURE() << msg;
    });

    return { context, device };
}

TEST(OpenCLOn12, Basic)
{
    (void)GetWARPContext();
}

TEST(OpenCLOn12, SimpleKernel)
{
    auto&& [context, device] = GetWARPContext();
    if (!context.get())
    {
        return;
    }
    cl::CommandQueue queue(context, device);

    const char* kernel_source =
    "__kernel void main_test(__global uint *output)\n\
    {\n\
        output[get_global_id(0)] = get_global_id(0);\n\
    }\n";

    const size_t width = 4;
    cl::Buffer buffer(context, (cl_mem_flags)(CL_MEM_ALLOC_HOST_PTR | CL_MEM_READ_WRITE), width * sizeof(uint32_t));

    cl::Program program(context, kernel_source, true /*build*/);
    cl::Kernel kernel(program, "main_test");

    kernel.setArg(0, buffer);
    queue.enqueueNDRangeKernel(kernel, 1, width);

    uint32_t result[width] = {};
    std::fill_n(result, width, 0xdeaddead);

    queue.enqueueReadBuffer(buffer, true, 0, sizeof(result), result);

    for (uint32_t i = 0; i < width; ++i)
    {
        EXPECT_EQ(result[i], i);
    }

    queue.enqueueNDRangeKernel(kernel, 1, 0);
    queue.enqueueReadBuffer(buffer, true, 0, sizeof(result), result);

    for (uint32_t i = 0; i < width; ++i)
    {
        EXPECT_EQ(result[i], i);
    }
}

TEST(OpenCLOn12, SimpleImages)
{
    auto&& [context, device] = GetWARPContext();
    if (!context.get())
    {
        return;
    }
    cl::CommandQueue queue(context, device);

    const char* kernel_source =
    "__kernel void main_test(read_only image2d_t input, write_only image2d_t output, float foo)\n\
    {\n\
        int2 coord = (int2)(get_global_id(0), get_global_id(1));\n\
        write_imagef(output, coord, read_imagef(input, coord) + foo);\n\
    }\n";

    const size_t width = 16;
    const size_t height = 16;
    cl::NDRange offset(0, 0);
    cl::NDRange localSize(4, 4);
    cl::NDRange globalSize(width, height);

    float InputData[width * height * 4];
    std::iota(InputData, std::end(InputData), 1.0f);

    cl::Image2D input(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                      cl::ImageFormat(CL_RGBA, CL_FLOAT), width, height,
                      sizeof(float) * width * 4, InputData);

    cl::Image2D output(context, CL_MEM_WRITE_ONLY | CL_MEM_HOST_READ_ONLY,
                       cl::ImageFormat(CL_RGBA, CL_FLOAT), width, height);

    cl::Program program(context, kernel_source, true /*build*/);
    cl::Kernel kernel(program, "main_test");

    kernel.setArg(0, input);
    kernel.setArg(1, output);
    kernel.setArg(2, 0.0f);
    queue.enqueueNDRangeKernel(kernel, offset, globalSize, localSize);

    float OutputData[width * height * 4];
    cl::array<cl::size_type, 3> origin{}, region{ width, height, 1 };
    queue.enqueueReadImage(output, true, origin, region, sizeof(float) * width * 4,
                           sizeof(float) * width * height * 4, OutputData);

    for (int i = 0; i < std::extent_v<decltype(InputData)>; ++i)
    {
        EXPECT_EQ(InputData[i], OutputData[i]);
    }
}

TEST(OpenCLOn12, LargeDispatch)
{
    auto&& [context, device] = GetWARPContext();
    if (!context.get())
    {
        return;
    }
    cl::CommandQueue queue(context, device);

    const char* kernel_source =
    R"(struct OutputStruct { unsigned global_id; unsigned local_id; unsigned work_group_id; };
    __kernel void main_test(__global struct OutputStruct *output)
    {
        uint global_id = get_global_id(0);
        output[global_id].global_id = global_id;
        output[global_id].local_id = get_local_id(0);
        output[global_id].work_group_id = get_group_id(0);
    })";

    struct OutputStruct { uint32_t global, local, work_group; };
    const size_t widthInStructs = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * D3D12_CS_THREAD_GROUP_MAX_X * 2;
    const size_t widthInBytes = widthInStructs * sizeof(OutputStruct);
    static_assert(widthInBytes < UINT32_MAX);
    cl::NDRange offset(0);
    cl::NDRange globalSize(widthInStructs);

    cl::Buffer output(context, CL_MEM_WRITE_ONLY, widthInBytes, nullptr);

    cl::Program program(context, kernel_source, true /*build*/);
    cl::Kernel kernel(program, "main_test");

    kernel.setArg(0, output);
    queue.enqueueNDRangeKernel(kernel, offset, globalSize);

    std::vector<OutputStruct> OutputData(widthInStructs);
    queue.enqueueReadBuffer(output, true, 0, widthInBytes, OutputData.data());

    for (uint32_t i = 0; i < widthInStructs; ++i)
    {
        EXPECT_EQ(OutputData[i].global, i);
        EXPECT_EQ(OutputData[i].local, i % D3D12_CS_THREAD_GROUP_MAX_X);
        EXPECT_EQ(OutputData[i].work_group, i / D3D12_CS_THREAD_GROUP_MAX_X);
    }
}

TEST(OpenCLOn12, Printf)
{
    auto&& [context, device] = GetWARPContext();
    if (!context.get())
    {
        return;
    }
    cl::CommandQueue queue(context, device);

    const char* kernel_source =
    R"(
    constant uchar arr[6] = {'c', 'l', 'o', 'n', '1', '2'};
    kernel void test_printf() {
	    printf("hello %d %f %s %s %c\n", 15, 1.5, "test", "this string", arr[3]);
	    printf("goodbye %d %f %s %c %s\n", 30, -1.5, "cruel", arr[2], "world");
        printf("hello cl\n", 10, "oh now");
        printf("hello cl %s\n", "again");
    })";

    cl::Program program(context, kernel_source, true /*build*/);
    cl::Kernel kernel(program, "test_printf");

    queue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(1));
    queue.finish();
}

TEST(OpenCLOn12, RecursiveFlush)
{
    auto&& [context, device] = GetWARPContext();
    if (!context.get())
    {
        return;
    }
    cl::CommandQueue queue1(context, device);
    cl::CommandQueue queue2(context, device);

    cl::UserEvent userEvent(context);
    cl::Event queue1Task1;

    cl::vector<cl::Event> waitList({ userEvent });
    queue1.enqueueBarrierWithWaitList(&waitList, &queue1Task1);

    waitList = {{ queue1Task1 }};
    cl::Event queue2Task1;
    queue2.enqueueBarrierWithWaitList(&waitList, &queue2Task1);

    waitList = {{ queue2Task1 }};
    cl::Event queue1Task2;
    queue1.enqueueBarrierWithWaitList(&waitList, &queue1Task2);

    EXPECT_EQ(queue1Task1.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>(), CL_QUEUED);
    EXPECT_EQ(queue2Task1.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>(), CL_QUEUED);
    EXPECT_EQ(queue1Task2.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>(), CL_QUEUED);

    queue1.flush();

    EXPECT_EQ(queue1Task1.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>(), CL_SUBMITTED);
    EXPECT_EQ(queue2Task1.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>(), CL_SUBMITTED);
    EXPECT_EQ(queue1Task2.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>(), CL_SUBMITTED);

    userEvent.setStatus(CL_SUCCESS);
    queue1.finish();

    EXPECT_EQ(queue1Task1.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>(), CL_COMPLETE);
    EXPECT_EQ(queue2Task1.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>(), CL_COMPLETE);
    EXPECT_EQ(queue1Task2.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>(), CL_COMPLETE);
}

TEST(OpenCLOn12, SPIRV)
{
    // This is the pre-assembled SPIR-V from the compiler DLL's "spec_constant" test:
    // https://gitlab.freedesktop.org/mesa/mesa/-/blob/f8517d9f43cc191fc7465db2850c2b813b94f023/src/microsoft/clc/clc_compiler_test.cpp#L2226.
    // The original source was the "built_ins_global_id_rmw" test, with the hardcoded 1 manually modified in the asm to make it a spec constant:
    // https://gitlab.freedesktop.org/mesa/mesa/-/blob/f8517d9f43cc191fc7465db2850c2b813b94f023/src/microsoft/clc/clc_compiler_test.cpp#L394
    /* __kernel void main_test(__global uint *output)
       {
           uint id = get_global_id(0);
           output[id] = output[id] * (id + {spec constant, id 1, default value 1});
       } */
    static const unsigned char spirv[] = {
0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11,
 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x11, 0x00, 0x02, 0x00, 0x06, 0x00, 0x00, 0x00, 0x11, 0x00, 0x02, 0x00, 0x0b, 0x00,
 0x00, 0x00, 0x0b, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4f, 0x70, 0x65, 0x6e, 0x43, 0x4c, 0x2e, 0x73, 0x74, 0x64, 0x00,
 0x00, 0x0e, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x07, 0x00, 0x06, 0x00, 0x00, 0x00,
 0x02, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e, 0x5f, 0x74, 0x65, 0x73, 0x74, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x07,
 0x00, 0x0b, 0x00, 0x04, 0x00, 0x00, 0x00, 0x6b, 0x65, 0x72, 0x6e, 0x65, 0x6c, 0x5f, 0x61, 0x72, 0x67, 0x5f, 0x74, 0x79, 0x70,
 0x65, 0x2e, 0x6d, 0x61, 0x69, 0x6e, 0x5f, 0x74, 0x65, 0x73, 0x74, 0x2e, 0x75, 0x69, 0x6e, 0x74, 0x2a, 0x2c, 0x00, 0x00, 0x00,
 0x00, 0x03, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x70, 0x8e, 0x01, 0x00, 0x05, 0x00, 0x0b, 0x00, 0x03, 0x00, 0x00, 0x00,
 0x5f, 0x5f, 0x73, 0x70, 0x69, 0x72, 0x76, 0x5f, 0x42, 0x75, 0x69, 0x6c, 0x74, 0x49, 0x6e, 0x47, 0x6c, 0x6f, 0x62, 0x61, 0x6c,
 0x49, 0x6e, 0x76, 0x6f, 0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x49, 0x64, 0x00, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00, 0x05, 0x00,
 0x00, 0x00, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x65, 0x6e, 0x74,
 0x72, 0x79, 0x00, 0x00, 0x00, 0x05, 0x00, 0x05, 0x00, 0x07, 0x00, 0x00, 0x00, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x2e, 0x61,
 0x64, 0x64, 0x72, 0x00, 0x05, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x00, 0x69, 0x64, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00, 0x09,
 0x00, 0x00, 0x00, 0x63, 0x61, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x63, 0x6f,
 0x6e, 0x76, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x69, 0x64, 0x78, 0x70, 0x72, 0x6f, 0x6d,
 0x00, 0x05, 0x00, 0x05, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x61, 0x72, 0x72, 0x61, 0x79, 0x69, 0x64, 0x78, 0x00, 0x00, 0x00, 0x00,
 0x05, 0x00, 0x03, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x61, 0x64, 0x64, 0x00, 0x05, 0x00, 0x03, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x6d,
 0x75, 0x6c, 0x00, 0x05, 0x00, 0x05, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x69, 0x64, 0x78, 0x70, 0x72, 0x6f, 0x6d, 0x31, 0x00, 0x00,
 0x00, 0x00, 0x05, 0x00, 0x05, 0x00, 0x10, 0x00, 0x00, 0x00, 0x61, 0x72, 0x72, 0x61, 0x79, 0x69, 0x64, 0x78, 0x32, 0x00, 0x00,
 0x00, 0x47, 0x00, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x47, 0x00, 0x03, 0x00,
 0x03, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x04,
 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x47, 0x00,
 0x04, 0x00, 0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x15, 0x00, 0x04, 0x00, 0x12, 0x00, 0x00,
 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x04, 0x00, 0x13, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0x04, 0x00, 0x13, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x17,
 0x00, 0x04, 0x00, 0x14, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x15, 0x00,
 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x13, 0x00, 0x02, 0x00, 0x16, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04,
 0x00, 0x17, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x21, 0x00, 0x04, 0x00, 0x18, 0x00, 0x00, 0x00,
 0x16, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x19, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x17,
 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x3b, 0x00,
 0x04, 0x00, 0x15, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x36, 0x00, 0x05, 0x00, 0x16, 0x00, 0x00,
 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x37, 0x00, 0x03, 0x00, 0x17, 0x00, 0x00, 0x00,
 0x05, 0x00, 0x00, 0x00, 0xf8, 0x00, 0x02, 0x00, 0x06, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00, 0x19, 0x00, 0x00, 0x00, 0x07,
 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07, 0x00,
 0x00, 0x00, 0x3e, 0x00, 0x05, 0x00, 0x07, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
 0x00, 0x3d, 0x00, 0x06, 0x00, 0x14, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
 0x20, 0x00, 0x00, 0x00, 0x51, 0x00, 0x05, 0x00, 0x12, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x71, 0x00, 0x04, 0x00, 0x13, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x3e, 0x00,
 0x05, 0x00, 0x08, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x06,
 0x00, 0x17, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
 0x3d, 0x00, 0x06, 0x00, 0x13, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04,
 0x00, 0x00, 0x00, 0x71, 0x00, 0x04, 0x00, 0x12, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x46, 0x00,
 0x05, 0x00, 0x17, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x06,
 0x00, 0x13, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
 0x3d, 0x00, 0x06, 0x00, 0x13, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04,
 0x00, 0x00, 0x00, 0x80, 0x00, 0x05, 0x00, 0x13, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x11, 0x00,
 0x00, 0x00, 0x84, 0x00, 0x05, 0x00, 0x13, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00,
 0x00, 0x3d, 0x00, 0x06, 0x00, 0x17, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
 0x08, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x06, 0x00, 0x13, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02,
 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x71, 0x00, 0x04, 0x00, 0x12, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x21, 0x00,
 0x00, 0x00, 0x46, 0x00, 0x05, 0x00, 0x17, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00,
 0x00, 0x3e, 0x00, 0x05, 0x00, 0x10, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
 0xfd, 0x00, 0x01, 0x00, 0x38, 0x00, 0x01, 0x00,
    };

    auto&& [context, device] = GetWARPContext();
    if (!context.get())
    {
        return;
    }
    cl::CommandQueue queue(context, device);

    std::vector<char> IL(spirv, std::end(spirv));
    cl::Program prog(context, IL, true /*build*/);
    cl::Kernel kernel(prog, "main_test");

    uint32_t data[] = { 0x00000001, 0x10000001, 0x00020002, 0x04010203 };
    cl::Buffer inout(context, data, std::end(data), false, true);

    kernel.setArg(0, inout);
    cl::NDRange offset(0);
    cl::NDRange global(_countof(data));
    queue.enqueueNDRangeKernel(kernel, offset, global);
    queue.enqueueMapBuffer(inout, CL_TRUE, CL_MAP_READ, 0, sizeof(data));

    EXPECT_EQ(data[0], 0x00000001u);
    EXPECT_EQ(data[1], 0x20000002u);
    EXPECT_EQ(data[2], 0x00060006u);
    EXPECT_EQ(data[3], 0x1004080cu);
}

class window
{
public:
    window(uint32_t width = 64, uint32_t height = 64);
    ~window();

    HWND get_hwnd() const { return _window; };
    HDC get_hdc() const { return _hdc; };
    HGLRC get_hglrc() const { return _hglrc; }
    bool valid() const { return _window && _hdc && _hglrc; }
    void show() {
        ShowWindow(_window, SW_SHOW);
    }

    void recreate_attribs(const int *attribList);

private:
    HWND _window = nullptr;
    HDC _hdc = nullptr;
    HGLRC _hglrc = nullptr;
};

window::window(uint32_t width, uint32_t height)
{
    _window = CreateWindowW(
        L"STATIC",
        L"OpenGLTestWindow",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        width,
        height,
        NULL,
        NULL,
        NULL,
        NULL
    );

    if (_window == nullptr)
        return;

    _hdc = ::GetDC(_window);

    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR),  /* size */
        1,                              /* version */
        PFD_SUPPORT_OPENGL |
        PFD_DRAW_TO_WINDOW |
        PFD_DOUBLEBUFFER,               /* support double-buffering */
        PFD_TYPE_RGBA,                  /* color type */
        8,                              /* prefered color depth */
        0, 0, 0, 0, 0, 0,               /* color bits (ignored) */
        0,                              /* no alpha buffer */
        0,                              /* alpha bits (ignored) */
        0,                              /* no accumulation buffer */
        0, 0, 0, 0,                     /* accum bits (ignored) */
        32,                             /* depth buffer */
        0,                              /* no stencil buffer */
        0,                              /* no auxiliary buffers */
        PFD_MAIN_PLANE,                 /* main layer */
        0,                              /* reserved */
        0, 0, 0,                        /* no layer, visible, damage masks */
    };
    int pixel_format = ChoosePixelFormat(_hdc, &pfd);
    if (pixel_format == 0)
        return;
    if (!SetPixelFormat(_hdc, pixel_format, &pfd))
        return;

    _hglrc = wglCreateContext(_hdc);
    if (!_hglrc)
        return;

    wglMakeCurrent(_hdc, _hglrc);
}

void window::recreate_attribs(const int *attribs)
{
    using pwglCreateContextAttribsARB = HGLRC(WINAPI*)(HDC, HGLRC, const int *);
    auto wglCreateContextAttribsARB = (pwglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!wglCreateContextAttribsARB)
        GTEST_FAIL() << "failed to get wglCreateContextAttribsARB";

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(_hglrc);
    _hglrc = wglCreateContextAttribsARB(_hdc, nullptr, attribs);
    if (!_hglrc)
        return;

    wglMakeCurrent(_hdc, _hglrc);
}

window::~window()
{
    if (_hglrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(_hglrc);
    }
    if (_hdc)
        ReleaseDC(_window, _hdc);
    if (_window)
        DestroyWindow(_window);
}

TEST(OpenCLOn12, WGLInterop)
{
    window glWindow;
    EXPECT_TRUE(glWindow.valid());

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (!strstr(renderer, "D3D12"))
        GTEST_SKIP();

    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);

    EXPECT_EQ(platforms.size(), 1);

    cl_context_properties context_props[] = {
        CL_CONTEXT_PLATFORM, (cl_context_properties)platforms[0](),
        CL_GL_CONTEXT_KHR, (cl_context_properties)glWindow.get_hglrc(),
        CL_WGL_HDC_KHR, (cl_context_properties)glWindow.get_hdc(),
        0
    };
    cl_device_id glDevice;
    EXPECT_EQ(CL_SUCCESS,
              clGetGLContextInfoKHR(context_props, CL_CURRENT_DEVICE_FOR_GL_CONTEXT_KHR,
                                    sizeof(glDevice), &glDevice, nullptr));
    EXPECT_NE(glDevice, nullptr);
    {
        cl::Context context(cl::Device(glDevice), context_props);
    }

    wglMakeCurrent(nullptr, nullptr);
    cl::Context context(cl::Device(glDevice), context_props);
}

TEST(OpenCLOn12, EGLInterop)
{
    HMODULE egl = LoadLibraryA("libEGL.dll");
    if (!egl)
        GTEST_SKIP();

    auto getDisplay = reinterpret_cast<decltype(&eglGetPlatformDisplay)>(GetProcAddress(egl, "eglGetPlatformDisplay"));
    if (!getDisplay)
        GTEST_SKIP();
    auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
    EXPECT_NE(display, nullptr);

    reinterpret_cast<decltype(&eglInitialize)>(GetProcAddress(egl, "eglInitialize"))(display, nullptr, nullptr);
    auto terminate = reinterpret_cast<decltype(&eglTerminate)>(GetProcAddress(egl, "eglTerminate"));
    auto displayCleanup = wil::scope_exit([&]() { terminate(display); });

    auto createContext = reinterpret_cast<decltype(&eglCreateContext)>(GetProcAddress(egl, "eglCreateContext"));
    auto glcontext = createContext(display, nullptr, nullptr, nullptr);
    EXPECT_NE(glcontext, nullptr);
    auto destroyContext = reinterpret_cast<decltype(&eglDestroyContext)>(GetProcAddress(egl, "eglDestroyContext"));
    auto contextCleanup = wil::scope_exit([&]() { destroyContext(display, glcontext); });

    auto makeCurrent = reinterpret_cast<decltype(&eglMakeCurrent)>(GetProcAddress(egl, "eglMakeCurrent"));
    EXPECT_NE(makeCurrent(display, nullptr, nullptr, glcontext), 0u);

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (!strstr(renderer, "D3D12"))
        GTEST_SKIP();

    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);

    EXPECT_EQ(platforms.size(), 1);

    cl_context_properties context_props[] = {
        CL_CONTEXT_PLATFORM, (cl_context_properties)platforms[0](),
        CL_GL_CONTEXT_KHR, (cl_context_properties)glcontext,
        CL_EGL_DISPLAY_KHR, (cl_context_properties)display,
        0
    };
    cl_device_id glDevice;
    EXPECT_EQ(CL_SUCCESS,
              clGetGLContextInfoKHR(context_props, CL_CURRENT_DEVICE_FOR_GL_CONTEXT_KHR,
                                    sizeof(glDevice), &glDevice, nullptr));
    EXPECT_NE(glDevice, nullptr);
    {
        cl::Context context(cl::Device(glDevice), context_props);
    }

    makeCurrent(display, nullptr, nullptr, nullptr);
    cl::Context context(cl::Device(glDevice), context_props);
}

int main(int argc, char** argv)
{
    ID3D12Debug* pDebug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebug))))
    {
        pDebug->EnableDebugLayer();
        pDebug->Release();
    }

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}