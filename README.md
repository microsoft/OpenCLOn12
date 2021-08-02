# OpenCLOn12

OpenCLOn12 is a mapping layer, which implements the OpenCL 1.2 API on top of D3D12. It also implements the OpenCL ICD extension, and can therefore be loaded by the existing ICD loader.

This repository contains the implementations of the APIs. It depends on various other repositories for further functionality, either implicitly or explicitly:
* [WIL](https://github.com/microsoft/wil) is used occasionally throughout.
* The [OpenCL headers](https://github.com/KhronosGroup/OpenCL-Headers) are referenced.
* [GoogleTest](https://github.com/google/googletest) is used for unit testing.
* The [D3D12TranslationLayer](https://github.com/microsoft/D3D12TranslationLayer) handles some of the complexity of using D3D12 for us.
* The compiler infrastructure for consuming OpenCL C and SPIR-V and converting to DXIL comes from the [Mesa](https://gitlab.freedesktop.org/mesa/mesa) project. This dependency is only required at runtime, as a copy of the compiler interface header is contained in this repo. The compiler interface in the `master` branch of this repository tracks `master` of Mesa.
  * The compiler was originally developed downstream from `master`, and the `downstream-abi` branch of this repository is intended to interface with that downstream interface.

Additionally, DXIL.dll from the Windows SDK will be required at runtime to sign and validate the DXIL shaders produced by the compiler.

For more details about OpenCLOn12, see:
* [Product release blog post](https://devblogs.microsoft.com/directx/announcing-the-opencl-and-opengl-compatibility-pack-for-windows-10-on-arm)
* [Microsoft blog post](https://devblogs.microsoft.com/directx/in-the-works-opencl-and-opengl-mapping-layers-to-directx/)
* [Collabora blog post](https://www.collabora.com/news-and-blog/news-and-events/introducing-opencl-and-opengl-on-directx.html)

Make sure that you visit the [DirectX Landing Page](https://devblogs.microsoft.com/directx/landing-page/) for more resources for DirectX developers.

## Current Status

At this point, the OpenCL 1.2 API is fully implemented, with no optional extensions. It has not yet been certified conformant, though it passes every conformance test for OpenCL 1.2, but has not yet passed all tests on a single underlying implementation.

## Building

The D3D12TranslationLayer project will be fetched from GitHub when building with CMake if D3D12TranslationLayer isn't already declared as a FetchContent source, such as by a parent CMakeLists.txt. Assuming there was a top level `CMakeLists.txt` in a directory that included both OpenCLOn12 and D3D12TranslationLayer, you could achieve that like this:

```CMake
cmake_minimum_required(VERSION 3.14)
include(FetchContent)

FetchContent_Declare(
    d3d12translationlayer
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/D3D12TranslationLayer
)
FetchContent_MakeAvailable(d3d12translationlayer)

add_subdirectory(OpenCLOn12)
```

At the time of publishing, OpenCLOn12 and the D3D12TranslationLayer require the latest released version of the SDK (19041).

OpenCLOn12 requires C++17, and only supports building with MSVC at the moment.

## Data Collection

The software may collect information about you and your use of the software and send it to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may turn off the telemetry as described in the repository. There are also some features in the software that may enable you and Microsoft to collect data from users of your applications. If you use these features, you must comply with applicable law, including providing appropriate notices to users of your applications together with a copy of Microsoft's privacy statement. Our privacy statement is located at https://go.microsoft.com/fwlink/?LinkID=824704. You can learn more about data collection and use in the help documentation and our privacy statement. Your use of the software operates as your consent to these practices.

Note however that no data collection is performed when using your private builds.
