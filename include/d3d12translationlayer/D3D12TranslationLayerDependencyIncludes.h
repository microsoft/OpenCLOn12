// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

// The Windows build uses DBG for debug builds, but Visual Studio defaults to NDEBUG for retail
// We'll pick TRANSLATION_LAYER_DBG for CMake (VS) builds, and we'll convert DBG to that here
// for Windows builds
#if DBG
#define TRANSLATION_LAYER_DBG 1
#endif

//SDK Headers
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _ATL_NO_WIN_SUPPORT
#include <windows.h>
#include <atlbase.h>
#include <comdef.h>
#include <strsafe.h>

#define INITGUID
#include <guiddef.h>
#include <directx/dxcore.h>
#undef INITGUID
#include <directx/d3dx12.h>
#include <directx/d3d12compatibility.h>

#include <utility>
using std::min;
using std::max;
