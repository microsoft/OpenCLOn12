// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
namespace D3D12TranslationLayer
{
    template<>
    inline void View<ShaderResourceViewType>::UpdateMinLOD(float MinLOD)
    {
        switch (m_Desc.ViewDimension)
        {
        case D3D12_SRV_DIMENSION_BUFFER:
        case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1D:
            m_Desc.Texture1D.ResourceMinLODClamp = MinLOD;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            m_Desc.Texture1DArray.ResourceMinLODClamp = MinLOD;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            m_Desc.Texture2D.ResourceMinLODClamp = MinLOD;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            m_Desc.Texture2DArray.ResourceMinLODClamp = MinLOD;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            m_Desc.Texture3D.ResourceMinLODClamp = MinLOD;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            m_Desc.TextureCube.ResourceMinLODClamp = MinLOD;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            m_Desc.TextureCubeArray.ResourceMinLODClamp = MinLOD;
            break;
        }
    }

    template< class TIface >
    inline void View<TIface>::UpdateMinLOD(float /*MinLOD*/)
    {
        // Do nothing
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    inline UINT GetDynamicBufferOffset(Resource* pBuffer)
    {
        UINT64 offset = pBuffer ? pBuffer->GetSubresourcePlacement(0).Offset : 0;
        assert(offset < (UINT)-1); // D3D11 resources shouldn't be able to produce offsetable buffers of more than UINT_MAX
        return (UINT)offset;
    }

    template <typename TDesc>
    inline UINT GetDynamicBufferSize(Resource* pBuffer, UINT offset)
    {
        UINT width = pBuffer->GetSubresourcePlacement(0).Footprint.Width;
        return offset > width ? 0 : width - offset;
    }

    template<>
    inline UINT GetDynamicBufferSize<D3D12_CONSTANT_BUFFER_VIEW_DESC>(Resource* pBuffer, UINT offset)
    {
        UINT pitch = pBuffer->GetSubresourcePlacement(0).Footprint.RowPitch;
        return offset > pitch ? 0 : pitch - offset;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    template<typename TIface>
    const typename View<TIface>::TDesc12& View<TIface>::GetDesc12() noexcept
    {
        typedef decltype(TDesc12::Buffer) TBufferDesc12;
        if (m_pResource->AppDesc()->ResourceDimension() == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
            UINT Divisor = GetByteAlignment(m_Desc.Format);
            if (m_Desc.Buffer.StructureByteStride != 0)
            {
                Divisor = m_Desc.Buffer.StructureByteStride;
            }
            UINT ByteOffset = GetDynamicBufferOffset(m_pResource);
            assert(ByteOffset % Divisor == 0);
            m_Desc.Buffer.FirstElement = APIFirstElement + ByteOffset / Divisor;
        }
        return m_Desc;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    template<typename TIface>
    HRESULT View<TIface>::RefreshUnderlying() noexcept
    {
        if (m_ViewUniqueness == UINT_MAX)
        {
            UpdateMinLOD(0.0f);

            const TDesc12 &Desc = GetDesc12();
            (m_pParent->m_pDevice12.get()->*CViewMapper<TIface>::GetCreate())(
                m_pResource->GetUnderlyingResource(),
                &Desc,
                m_Descriptor);

            m_ViewUniqueness = 0;
            return S_OK;
        }
        return S_FALSE;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    // Specialized because ID3D12Device::CreateUnorderedAccessView takes 2 resources as input
    template<>
    inline HRESULT View<UnorderedAccessViewType>::RefreshUnderlying() noexcept
    {
        if (m_ViewUniqueness == UINT_MAX)
        {
            const TDesc12 &Desc = GetDesc12();

            m_pParent->m_pDevice12.get()->CreateUnorderedAccessView(
                m_pResource->GetUnderlyingResource(),
                nullptr,
                &Desc,
                m_Descriptor
                );

            m_ViewUniqueness = 0;
        }
        return S_OK;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    inline ViewBase::ViewBase(ImmediateContext* pDevice, Resource* pResource, CViewSubresourceSubset const& Subresources) noexcept
        : DeviceChild(pDevice)
        , m_pResource(pResource)
        , m_ViewUniqueness(UINT_MAX)
        , m_subresources(Subresources)
    {
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    template<typename TIface>
    View<TIface>::View(ImmediateContext* pDevice, const typename TDesc12 &Desc, Resource &ViewResource) noexcept(false)
        : ViewBase(pDevice,
        &ViewResource,
        CViewSubresourceSubset(Desc,
        (UINT8)ViewResource.AppDesc()->MipLevels(),
        (UINT16)ViewResource.AppDesc()->ArraySize(),
        (UINT8)ViewResource.AppDesc()->NonOpaquePlaneCount() * ViewResource.SubresourceMultiplier())),
        m_Desc(Desc),
        APIFirstElement(0)
    {
        __if_exists(TDesc12::Buffer)
        {
            APIFirstElement = Desc.Buffer.FirstElement;
        }

        m_Descriptor = pDevice->GetViewAllocator<TIface>().AllocateHeapSlot(&m_DescriptorHeapIndex); // throw( _com_error )
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    template<typename TIface>
    View<TIface>::~View() noexcept
    {
        m_pParent->GetViewAllocator<TIface>().FreeHeapSlot(m_Descriptor, m_DescriptorHeapIndex);
    }

};