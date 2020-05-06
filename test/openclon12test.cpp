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