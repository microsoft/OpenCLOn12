# OpenCLOn12

OpenCLOn12 is a mapping layer, which implements the OpenCL 1.2 API on top of D3D12. It also implements the OpenCL ICD extension, and can therefore be loaded by the existing ICD loader.

OpenCLOn12 is very much still a work in progress.

This repository contains the implementations of the APIs. It depends on various other repositories for further functionality, either implicitly or explicitly:
* [WIL](https://github.com/microsoft/wil) is used occasionally throughout.
* The [OpenCL headers](https://github.com/KhronosGroup/OpenCL-Headers) are referenced.
* [GoogleTest](https://github.com/google/googletest) is used for unit testing.
* The [D3D12TranslationLayer](https://github.com/microsoft/D3D12TranslationLayer) handles some of the complexity of using D3D12 for us.
* The compiler infrastructure for consuming OpenCL C and SPIR-V and converting to DXIL comes from the [Mesa](https://gitlab.freedesktop.org/mesa/mesa) project. This dependency is only required at runtime, as a copy of the compiler interface header is contained in this repo.

Additionally, DXIL.dll from the Windows SDK will be required at runtime to sign and validate the DXIL shaders produced by the compiler.

For more details about OpenCLOn12, see:
* [Microsoft blog post](https://devblogs.microsoft.com/directx/in-the-works-opencl-and-opengl-mapping-layers-to-directx/)
* [Collabora blog post](https://www.collabora.com/news-and-blog/news-and-events/introducing-opencl-and-opengl-on-directx.html)

## Current Status

At this point, the OpenCL 1.2 API is fully implemented, with no optional extensions.

## Building

This project is expected to be included in a CMake build environment where the D3D12TranslationLayer project is also included.

At the time of publishing, OpenCLOn12 and the D3D12TranslationLayer require the latest released version of the SDK (19041).

An example CMakeLists.txt for building OpenCLOn12 would be:

```
cmake_minimum_required(VERSION 3.13)
add_subdirectory(D3D12TranslationLayer)
add_subdirectory(OpenCLOn12)
```

OpenCLOn12 requires C++17, and only supports building with MSVC at the moment.

## Data Collection

The software may collect information about you and your use of the software and send it to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may turn off the telemetry as described in the repository. There are also some features in the software that may enable you and Microsoft to collect data from users of your applications. If you use these features, you must comply with applicable law, including providing appropriate notices to users of your applications together with a copy of Microsoft's privacy statement. Our privacy statement is located at https://go.microsoft.com/fwlink/?LinkID=824704. You can learn more about data collection and use in the help documentation and our privacy statement. Your use of the software operates as your consent to these practices.

Note however that no data collection is performed when using your private builds.