// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "D3D12TranslationLayerDependencyIncludes.h"
#include "DeviceChild.hpp"
#include "SubresourceHelpers.hpp"
#include <bitset>

namespace D3D12TranslationLayer
{
    class Resource;
    //==================================================================================================================================
    // View
    //==================================================================================================================================

    // These types are purely used to specialize the templated
    // view class
    enum class ShaderResourceViewType {};
    enum class UnorderedAccessViewType {};

    template< class TIface >
    struct CViewMapper;

#define DECLARE_VIEW_MAPPER(View, DescType12, TranslationLayerDesc) \
    template<> struct CViewMapper<##View##Type> \
    { \
    typedef TranslationLayerDesc TTranslationLayerDesc; \
    typedef D3D12_##DescType12 TDesc12; \
    static decltype(&ID3D12Device::Create##View) GetCreate() { return &ID3D12Device::Create##View; } \
    }

    DECLARE_VIEW_MAPPER(ShaderResourceView, SHADER_RESOURCE_VIEW_DESC, D3D12_SHADER_RESOURCE_VIEW_DESC);
    DECLARE_VIEW_MAPPER(UnorderedAccessView, UNORDERED_ACCESS_VIEW_DESC, D3D12_UNORDERED_ACCESS_VIEW_DESC);
#undef DECLARE_VIEW_MAPPER

    class ViewBase : public DeviceChild
    {
    public: // Methods
        ViewBase(ImmediateContext* pDevice, Resource* pResource, CViewSubresourceSubset const& Subresources) noexcept;

        // Note: This is hiding the base class implementation not overriding it
        // Warning: this method is hidden in the UAV type, and is not virtual
        // Always ensure that this method is called on the most derived type.
        void UsedInCommandList(UINT64 id);

    public: // Members
        Resource* const m_pResource;

    protected:
        D3D12_CPU_DESCRIPTOR_HANDLE m_Descriptor;
        UINT m_DescriptorHeapIndex;

    public:
        CViewSubresourceSubset m_subresources;
        UINT m_ViewUniqueness;
    };

    template< class TIface >
    class View : public ViewBase
    {
    public: // Types
        typedef CViewMapper<TIface> TMapper;
        typedef typename CViewMapper<TIface>::TDesc12 TDesc12;
        typedef typename CViewMapper<TIface>::TTranslationLayerDesc TTranslationLayerDesc;

    public: // Methods
        static View *CreateView(ImmediateContext* pDevice, const typename TDesc12 &Desc, Resource &ViewResource) noexcept(false) { return new View(pDevice, Desc, ViewResource); }
        static void DestroyView(View* pView) { delete pView; }

        View(ImmediateContext* pDevice, const typename TDesc12 &Desc, Resource &ViewResource) noexcept(false);
        ~View() noexcept;

        const TDesc12& GetDesc12() noexcept;

        bool IsUpToDate() const noexcept { return m_pResource->GetUniqueness<TIface>() == m_ViewUniqueness; }
        HRESULT RefreshUnderlying() noexcept;
        D3D12_CPU_DESCRIPTOR_HANDLE GetRefreshedDescriptorHandle()
        {
            HRESULT hr = RefreshUnderlying();
            if (FAILED(hr))
            {
                assert(hr != E_INVALIDARG);
                ThrowFailure(hr);
            }
            return m_Descriptor;
        }

    private:
        TDesc12 m_Desc;

        // We tamper with m_Desc.Buffer.FirstElement when renaming resources for map discard so it is important that we record the 
        // original first element expected by the API
        UINT64 APIFirstElement;

        void UpdateMinLOD(float MinLOD);
    };

    typedef View<ShaderResourceViewType> TSRV;
    typedef View<UnorderedAccessViewType> TUAV;

    class CDescriptorHeapManager;
    struct DescriptorHeapEntry
    {
        DescriptorHeapEntry(CDescriptorHeapManager *pDescriptorHeapManager, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor, UINT DescriptorHeapIndex, UINT64 LastUsedCommandListID) :
            m_pDescriptorHeapManager(pDescriptorHeapManager), m_Descriptor(Descriptor), m_DescriptorHeapIndex(DescriptorHeapIndex), m_LastUsedCommandListID(LastUsedCommandListID) {}

        D3D12_CPU_DESCRIPTOR_HANDLE m_Descriptor;
        CDescriptorHeapManager *m_pDescriptorHeapManager;
        UINT m_DescriptorHeapIndex;
        UINT64 m_LastUsedCommandListID;
    };

    typedef TSRV SRV;
    typedef TUAV UAV;
};