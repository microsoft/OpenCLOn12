// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "compiler.hpp"
#include "platform.hpp"
#include <dxc/dxcapi.h>

void Logger::Log(const char *msg) const
{
    std::lock_guard lock(m_lock);
    m_buildLog += msg;
}

static ProgramBinary::Kernel const& FindKernelInfo(std::vector<ProgramBinary::Kernel> const& kernels, const char *name)
{
    auto iter = std::find_if(kernels.begin(), kernels.end(), [name](ProgramBinary::Kernel const& k) { return strcmp(k.name, name) == 0; });
    assert(iter != kernels.end()); // We can't get DXIL if there's no data for a kernel with this name
    return *iter;
}

CompiledDxil::CompiledDxil(ProgramBinary const& parent, const char *name)
    : m_Parent(parent)
    , m_Metadata(FindKernelInfo(parent.GetKernelInfo(), name))
{
}

CompiledDxil::CompiledDxil(ProgramBinary const& parent, Metadata const &metadata)
    : m_Parent(parent)
    , m_Metadata(metadata)
{
}

CompiledDxil::Metadata const& CompiledDxil::GetMetadata() const
{
    return m_Metadata;
}

static void SignBlob(void* pBlob, size_t size)
{
    auto& DXIL = g_Platform->GetDXIL();
    auto pfnCreateInstance = DXIL.proc_address<decltype(&DxcCreateInstance)>("DxcCreateInstance");
    ComPtr<IDxcValidator> spValidator;
    if (SUCCEEDED(pfnCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(&spValidator))))
    {
        struct Blob : IDxcBlob
        {
            void* pBlob;
            UINT Size;
            Blob(void* p, UINT s) : pBlob(p), Size(s) { }
            STDMETHOD(QueryInterface)(REFIID, void** ppv) { *ppv = this; return S_OK; }
            STDMETHOD_(ULONG, AddRef)() { return 1; }
            STDMETHOD_(ULONG, Release)() { return 0; }
            STDMETHOD_(void*, GetBufferPointer)() override { return pBlob; }
            STDMETHOD_(SIZE_T, GetBufferSize)() override { return Size; }
        } Blob = { pBlob, (UINT)size };
        ComPtr<IDxcOperationResult> spResult;
        (void)spValidator->Validate(&Blob, DxcValidatorFlags_InPlaceEdit, &spResult);
        HRESULT hr = S_OK;
        if (spResult)
        {
            (void)spResult->GetStatus(&hr);
        }
        if (FAILED(hr))
        {
            ComPtr<IDxcBlobEncoding> spError;
            spResult->GetErrorBuffer(&spError);
            BOOL known = FALSE;
            UINT32 cp = 0;
            spError->GetEncoding(&known, &cp);
            if (cp == CP_UTF8 || cp == CP_ACP)
                printf("%s", (char*)spError->GetBufferPointer());
            else
                printf("%S", (wchar_t*)spError->GetBufferPointer());
            DebugBreak();
        }
    }
}

void CompiledDxil::Sign()
{
    SignBlob(GetBinary(), GetBinarySize());
}

std::vector<ProgramBinary::Kernel> const& ProgramBinary::GetKernelInfo() const
{
    return m_KernelInfo;
}

const ProgramBinary::SpecConstantInfo *ProgramBinary::GetSpecConstantInfo(uint32_t ID) const
{
    auto iter = m_SpecConstants.find(ID);
    if (iter == m_SpecConstants.end())
        return nullptr;

    return &iter->second;
}
