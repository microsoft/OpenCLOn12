// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "gtest/gtest.h"

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#define CL_HPP_ENABLE_EXCEPTIONS
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#include "cl2.hpp"
#include <memory>
#include <utility>
#include <algorithm>
#include <numeric>

#include <d3d12.h>

std::pair<cl::Context, cl::Device> GetWARPContext()
{
    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);

    if (platforms.size() != 1)
    {
        ADD_FAILURE() << "Unexpected platforms";
    }

    std::vector<cl::Device> devices;
    platforms[0].getDevices(CL_DEVICE_TYPE_GPU, &devices);
    if (devices.size() < 2)
    {
        ADD_FAILURE() << "Unexpected device count";
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
}

TEST(OpenCLOn12, SimpleImages)
{
    auto&& [context, device] = GetWARPContext();
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

    queue.enqueueTask(kernel);
    queue.finish();
}

TEST(OpenCLOn12, RecursiveFlush)
{
    auto&& [context, device] = GetWARPContext();
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