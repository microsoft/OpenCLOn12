// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "SubresourceHelpers.hpp"
#include "Util.hpp"

namespace D3D12TranslationLayer
{
    //==================================================================================================================================
    //
    // TRIGGER_CONDITION
    //
    // Used to essentially accumulate multiple conditions into one value. For example: during validation checking, one needs to check
    // to ensure multiple conditions are conceptually false, before it is known that all the parameters are valid. Starting with a
    // single value (uInvalid), initialized to 0, TRIGGER_CONDITION can be used to accumulate evaluations into such a value. At the
    // end of usage, (uInvalid) will be non-zero if any of those conditions were true. Ideal usage of TRIGGER_CONDITION uses only
    // a single condition check per statement. Multiple checks will conform to the C rules of evaluate the second statement only if
    // required, causing jumps, defeating the purpose. TRIGGER_CONDITION should be used more with (<, <=, >, >=, ==) than with
    // bit-checking and !=. For such cases, using math directly is better.
    //
    //==================================================================================================================================
#ifndef TRIGGER_CONDITION
#if defined( _X86_ )
    // x86 preferred syntax that generates math:
#define TRIGGER_CONDITION( uInvalid, Condition ) { uInvalid |= SIZE_T( Condition ); }
#else
    // x64 preferred syntax that generates cmov:
#define TRIGGER_CONDITION( uInvalid, Condition ) if (Condition) { uInvalid = 1; }
#endif
#endif

    //----------------------------------------------------------------------------------------------------------------------------------
    //----------------------------------------------------------------------------------------------------------------------------------
    CSubresourceSubset::CSubresourceSubset( UINT8 NumMips, UINT16 NumArraySlices, UINT8 NumPlanes, UINT8 FirstMip, UINT16 FirstArraySlice, UINT8 FirstPlane ) noexcept :
        m_BeginArray( FirstArraySlice ),
        m_EndArray( FirstArraySlice + NumArraySlices ),
        m_BeginMip( FirstMip ),
        m_EndMip( FirstMip + NumMips ),
        m_BeginPlane( FirstPlane ),
        m_EndPlane( FirstPlane + NumPlanes )
    {
        assert(NumMips > 0 && NumArraySlices > 0 && NumPlanes > 0);
        assert(NumNonExtendedSubresources() > 0 && NumExtendedSubresources() > 0);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CSubresourceSubset::CSubresourceSubset( const CBufferView& ) :
        m_BeginArray( 0 ),
        m_EndArray( 1 ),
        m_BeginMip( 0 ),
        m_EndMip( 1 ),
        m_BeginPlane(0),
        m_EndPlane(1)
    {
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CSubresourceSubset::CSubresourceSubset( const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc ) noexcept :
        m_BeginArray( 0 ),
        m_EndArray( 1 ),
        m_BeginMip( 0 ),
        m_EndMip( 1 ),
        m_BeginPlane(0),
        m_EndPlane(1)
    {
        switch (Desc.ViewDimension)
        {
        default: ASSUME( 0 && "Corrupt Resource Type on Shader Resource View" ); break;

        case (D3D12_SRV_DIMENSION_BUFFER): 
            break;

        case (D3D12_SRV_DIMENSION_TEXTURE1D):
            m_BeginMip = UINT8( Desc.Texture1D.MostDetailedMip );
            m_EndMip = UINT8( m_BeginMip + Desc.Texture1D.MipLevels );
            break;

        case (D3D12_SRV_DIMENSION_TEXTURE1DARRAY):
            m_BeginArray = UINT16( Desc.Texture1DArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture1DArray.ArraySize );
            m_BeginMip = UINT8( Desc.Texture1DArray.MostDetailedMip );
            m_EndMip = UINT8( m_BeginMip + Desc.Texture1DArray.MipLevels );
            break;

        case (D3D12_SRV_DIMENSION_TEXTURE2D):
            m_BeginMip = UINT8( Desc.Texture2D.MostDetailedMip );
            m_EndMip = UINT8( m_BeginMip + Desc.Texture2D.MipLevels );
            m_BeginPlane = UINT8(Desc.Texture2D.PlaneSlice);
            m_EndPlane = UINT8(Desc.Texture2D.PlaneSlice + 1);
            break;

        case (D3D12_SRV_DIMENSION_TEXTURE2DARRAY):
            m_BeginArray = UINT16( Desc.Texture2DArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture2DArray.ArraySize );
            m_BeginMip = UINT8( Desc.Texture2DArray.MostDetailedMip );
            m_EndMip = UINT8( m_BeginMip + Desc.Texture2DArray.MipLevels );
            m_BeginPlane = UINT8(Desc.Texture2DArray.PlaneSlice);
            m_EndPlane = UINT8(Desc.Texture2DArray.PlaneSlice + 1);
            break;

        case (D3D12_SRV_DIMENSION_TEXTURE2DMS): break;

        case (D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY):
            m_BeginArray = UINT16( Desc.Texture2DMSArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture2DMSArray.ArraySize );
            break;

        case (D3D12_SRV_DIMENSION_TEXTURE3D):
            m_EndArray = UINT16( -1 ); //all slices
            m_BeginMip = UINT8( Desc.Texture3D.MostDetailedMip );
            m_EndMip = UINT8( m_BeginMip + Desc.Texture3D.MipLevels );
            break;

        case (D3D12_SRV_DIMENSION_TEXTURECUBE):
            m_BeginMip = UINT8( Desc.TextureCube.MostDetailedMip );
            m_EndMip = UINT8( m_BeginMip + Desc.TextureCube.MipLevels );
            m_BeginArray = 0;
            m_EndArray = 6;
            break;

        case (D3D12_SRV_DIMENSION_TEXTURECUBEARRAY):
            m_BeginArray = UINT16( Desc.TextureCubeArray.First2DArrayFace );
            m_EndArray = UINT16( m_BeginArray + Desc.TextureCubeArray.NumCubes * 6 );
            m_BeginMip = UINT8( Desc.TextureCubeArray.MostDetailedMip );
            m_EndMip = UINT8( m_BeginMip + Desc.TextureCubeArray.MipLevels );
            break;
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CSubresourceSubset::CSubresourceSubset( const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc ) noexcept :
        m_BeginArray( 0 ),
        m_EndArray( 1 ),
        m_BeginMip( 0 ),
        m_BeginPlane(0),
        m_EndPlane(1)
    {
        switch (Desc.ViewDimension)
        {
        default: ASSUME( 0 && "Corrupt Resource Type on Unordered Access View" ); break;

        case (D3D12_UAV_DIMENSION_BUFFER): break;

        case (D3D12_UAV_DIMENSION_TEXTURE1D):
            m_BeginMip = UINT8( Desc.Texture1D.MipSlice );
            break;

        case (D3D12_UAV_DIMENSION_TEXTURE1DARRAY):
            m_BeginArray = UINT16( Desc.Texture1DArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture1DArray.ArraySize );
            m_BeginMip = UINT8( Desc.Texture1DArray.MipSlice );
            break;

        case (D3D12_UAV_DIMENSION_TEXTURE2D):
            m_BeginMip = UINT8( Desc.Texture2D.MipSlice );
            m_BeginPlane = UINT8(Desc.Texture2D.PlaneSlice);
            m_EndPlane = UINT8(Desc.Texture2D.PlaneSlice + 1);
            break;

        case (D3D12_UAV_DIMENSION_TEXTURE2DARRAY):
            m_BeginArray = UINT16( Desc.Texture2DArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture2DArray.ArraySize );
            m_BeginMip = UINT8( Desc.Texture2DArray.MipSlice );
            m_BeginPlane = UINT8(Desc.Texture2DArray.PlaneSlice);
            m_EndPlane = UINT8(Desc.Texture2DArray.PlaneSlice + 1);
            break;

        case (D3D12_UAV_DIMENSION_TEXTURE3D):
            m_BeginArray = UINT16( Desc.Texture3D.FirstWSlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture3D.WSize );
            m_BeginMip = UINT8( Desc.Texture3D.MipSlice );
            break;
        }

        m_EndMip = m_BeginMip + 1;
    }


    //----------------------------------------------------------------------------------------------------------------------------------
    CSubresourceSubset::CSubresourceSubset( const D3D12_RENDER_TARGET_VIEW_DESC& Desc ) noexcept :
        m_BeginArray( 0 ),
        m_EndArray( 1 ),
        m_BeginMip( 0 ),
        m_BeginPlane(0),
        m_EndPlane(1)
    {
        switch (Desc.ViewDimension)
        {
        default: ASSUME( 0 && "Corrupt Resource Type on Render Target View" ); break;

        case (D3D12_RTV_DIMENSION_BUFFER): break;

        case (D3D12_RTV_DIMENSION_TEXTURE1D):
            m_BeginMip = UINT8( Desc.Texture1D.MipSlice );
            break;

        case (D3D12_RTV_DIMENSION_TEXTURE1DARRAY):
            m_BeginArray = UINT16( Desc.Texture1DArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture1DArray.ArraySize );
            m_BeginMip = UINT8( Desc.Texture1DArray.MipSlice );
            break;

        case (D3D12_RTV_DIMENSION_TEXTURE2D):
            m_BeginMip = UINT8( Desc.Texture2D.MipSlice );
            m_BeginPlane = UINT8(Desc.Texture2D.PlaneSlice);
            m_EndPlane = UINT8(Desc.Texture2D.PlaneSlice + 1);
            break;

        case (D3D12_RTV_DIMENSION_TEXTURE2DMS): break;

        case (D3D12_RTV_DIMENSION_TEXTURE2DARRAY):
            m_BeginArray = UINT16( Desc.Texture2DArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture2DArray.ArraySize );
            m_BeginMip = UINT8( Desc.Texture2DArray.MipSlice );
            m_BeginPlane = UINT8(Desc.Texture2DArray.PlaneSlice);
            m_EndPlane = UINT8(Desc.Texture2DArray.PlaneSlice + 1);
            break;

        case (D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY):
            m_BeginArray = UINT16( Desc.Texture2DMSArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture2DMSArray.ArraySize );
            break;

        case (D3D12_RTV_DIMENSION_TEXTURE3D):
            m_BeginArray = UINT16( Desc.Texture3D.FirstWSlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture3D.WSize );
            m_BeginMip = UINT8( Desc.Texture3D.MipSlice );
            break;
        }

        m_EndMip = m_BeginMip + 1;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CSubresourceSubset::CSubresourceSubset( const D3D12_DEPTH_STENCIL_VIEW_DESC& Desc ) noexcept :
        m_BeginArray( 0 ),
        m_EndArray( 1 ),
        m_BeginMip( 0 ),
        m_BeginPlane(0),
        m_EndPlane(1)
    {
        switch (Desc.ViewDimension)
        {
        default: ASSUME( 0 && "Corrupt Resource Type on Depth Stencil View" ); break;

        case (D3D12_DSV_DIMENSION_TEXTURE1D):
            m_BeginMip = UINT8( Desc.Texture1D.MipSlice );
            break;

        case (D3D12_DSV_DIMENSION_TEXTURE1DARRAY):
            m_BeginArray = UINT16( Desc.Texture1DArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture1DArray.ArraySize );
            m_BeginMip = UINT8( Desc.Texture1DArray.MipSlice );
            break;

        case (D3D12_DSV_DIMENSION_TEXTURE2D):
            m_BeginMip = UINT8( Desc.Texture2D.MipSlice );
            break;

        case (D3D12_DSV_DIMENSION_TEXTURE2DMS): break;

        case (D3D12_DSV_DIMENSION_TEXTURE2DARRAY):
            m_BeginArray = UINT16( Desc.Texture2DArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture2DArray.ArraySize );
            m_BeginMip = UINT8( Desc.Texture2DArray.MipSlice );
            break;

        case (D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY):
            m_BeginArray = UINT16( Desc.Texture2DMSArray.FirstArraySlice );
            m_EndArray = UINT16( m_BeginArray + Desc.Texture2DMSArray.ArraySize );
            break;
        }

        m_EndMip = m_BeginMip + 1;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT CSubresourceSubset::Mask() const noexcept
    {
        // This only works for views that only reference subresources < 32.
        // This is sufficient for the YUV/Decode SwapChain case where it is used.
        assert(m_BeginMip == 0);
        assert(m_EndMip == 1);
        assert(m_EndArray <= 32);
        assert(m_EndArray >= 1);

        UINT result = (2 << (m_EndArray-1)) - (1 << m_BeginArray);
    #if DBG
        for (unsigned i = 0; i < 32; ++i)
        {
            assert( (!!(result & (1<<i))) == (m_BeginArray <= i && m_EndArray > i) );
        }
        assert( ((2 << 31) - 1) == -1 );
    #endif
        return result;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    SIZE_T CSubresourceSubset::DoesNotOverlap( const CSubresourceSubset& other ) const noexcept
    {
        SIZE_T uDoNotOverlap = 0;

        TRIGGER_CONDITION( uDoNotOverlap, m_EndArray <= other.m_BeginArray );

        TRIGGER_CONDITION( uDoNotOverlap, other.m_EndArray <= m_BeginArray );

        TRIGGER_CONDITION( uDoNotOverlap, m_EndMip <= other.m_BeginMip );

        TRIGGER_CONDITION( uDoNotOverlap, other.m_EndMip <= m_BeginMip );

        TRIGGER_CONDITION( uDoNotOverlap, m_EndPlane <= other.m_BeginPlane );

        TRIGGER_CONDITION( uDoNotOverlap, other.m_EndPlane <= m_BeginPlane );

        return uDoNotOverlap;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT CSubresourceSubset::NumNonExtendedSubresources() const noexcept
    {
        return (m_EndArray - m_BeginArray) * (m_EndMip - m_BeginMip);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT CSubresourceSubset::NumExtendedSubresources() const noexcept
    {
        return (m_EndArray - m_BeginArray) * (m_EndMip - m_BeginMip) * (m_EndPlane - m_BeginPlane);
    }

    //==================================================================================================================================
    // CViewSubresourceSubset
    // Extends CSubresourceSubset to support iterating over subresource ranges
    //==================================================================================================================================

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceSubset( CSubresourceSubset const& Subresources, UINT8 MipLevels, UINT16 ArraySize, UINT8 PlaneCount )
        : CSubresourceSubset( Subresources )
        , m_MipLevels( MipLevels )
        , m_ArraySlices( ArraySize )
        , m_PlaneCount( PlaneCount )
    {
        Reduce();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceSubset( const CBufferView& )
        : CSubresourceSubset( CBufferView() )
        , m_MipLevels( 1 )
        , m_ArraySlices( 1 )
        , m_PlaneCount( 1 )
    {
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceSubset( const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc, UINT8 MipLevels, UINT16 ArraySize, UINT8 PlaneCount )
        : CSubresourceSubset( Desc )
        , m_MipLevels( MipLevels )
        , m_ArraySlices( ArraySize )
        , m_PlaneCount( PlaneCount )
    {
        if (Desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D)
        {
            assert(m_BeginArray == 0);
            m_EndArray = 1;
        }
        Reduce();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceSubset( const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc, UINT8 MipLevels, UINT16 ArraySize, UINT8 PlaneCount )
        : CSubresourceSubset( Desc )
        , m_MipLevels( MipLevels )
        , m_ArraySlices( ArraySize )
        , m_PlaneCount( PlaneCount )
    {
        if (Desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D)
        {
            m_BeginArray = 0;
            m_EndArray = 1;
        }
        Reduce();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceSubset(const D3D12_DEPTH_STENCIL_VIEW_DESC& Desc, UINT8 MipLevels, UINT16 ArraySize, UINT8 PlaneCount, DepthStencilMode DSMode)
        : CSubresourceSubset( Desc )
        , m_MipLevels( MipLevels )
        , m_ArraySlices( ArraySize )
        , m_PlaneCount( PlaneCount )
    {
        // When this class is used by 11on12 for depthstencil formats, it treats them as planar
        // When binding DSVs of planar resources, additional view subresource subsets will be constructed
        if (m_PlaneCount == 2)
        {
            if (DSMode != ReadOrWrite)
            {
                bool bWritable = DSMode == WriteOnly;
                bool bDepth = !(Desc.Flags & D3D11_DSV_READ_ONLY_DEPTH) == bWritable;
                bool bStencil = !(Desc.Flags & D3D11_DSV_READ_ONLY_STENCIL) == bWritable;
                m_BeginPlane = (bDepth ? 0 : 1);
                m_EndPlane = (bStencil ? 2 : 1);
            }
            else
            {
                m_BeginPlane = 0;
                m_EndPlane = 2;
            }
        }

        Reduce();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceSubset( const D3D12_RENDER_TARGET_VIEW_DESC& Desc, UINT8 MipLevels, UINT16 ArraySize, UINT8 PlaneCount )
        : CSubresourceSubset( Desc )
        , m_MipLevels( MipLevels )
        , m_ArraySlices( ArraySize )
        , m_PlaneCount( PlaneCount )
    {
        if (Desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE3D)
        {
            m_BeginArray = 0;
            m_EndArray = 1;
        }
        Reduce();
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    // Allows constructing a CViewSubresourceSubset from a core layer C*View object
    template<typename T>
    /*static*/ CViewSubresourceSubset CViewSubresourceSubset::FromView( const T* pView )
    {
        return CViewSubresourceSubset( 
            pView->Desc(),
            static_cast<UINT8>(pView->Resource()->MipLevels()),
            static_cast<UINT16>(pView->Resource()->ArraySize()),
            static_cast<UINT8>(pView->Resource()->PlaneCount())
            );
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    // Strictly for performance, allows coalescing contiguous subresource ranges into a single range
    void CViewSubresourceSubset::Reduce()
    {
        if (   m_BeginMip == 0 
            && m_EndMip == m_MipLevels
            && m_BeginArray == 0
            && m_EndArray == m_ArraySlices)
        {
            UINT startSubresource = ComposeSubresourceIdxExtended(0, 0, m_BeginPlane, m_MipLevels, m_ArraySlices);
            UINT endSubresource = ComposeSubresourceIdxExtended(0, 0, m_EndPlane, m_MipLevels, m_ArraySlices);

            // Only coalesce if the full-resolution UINTs fit in the UINT8s used for storage here
            if (endSubresource < static_cast<UINT8>(-1))
            {
                m_BeginArray = 0;
                m_EndArray = 1;
                m_BeginPlane = 0;
                m_EndPlane = 1;
                m_BeginMip = static_cast<UINT8>(startSubresource);
                m_EndMip = static_cast<UINT8>(endSubresource);
            }
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool CViewSubresourceSubset::IsWholeResource() const
    {
        return m_BeginMip == 0 && m_BeginArray == 0 && m_BeginPlane == 0 && (m_EndMip * m_EndArray * m_EndPlane == m_MipLevels * m_ArraySlices * m_PlaneCount);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool CViewSubresourceSubset::IsEmpty() const
    {
        return m_BeginMip == m_EndMip || m_BeginArray == m_EndArray || m_BeginPlane == m_EndPlane;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT CViewSubresourceSubset::MinSubresource() const
    {
        return (*begin()).first;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT CViewSubresourceSubset::MaxSubresource() const
    {
        return (*(--end())).second;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT CViewSubresourceSubset::ArraySize() const
    {
        return m_ArraySlices;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceIterator CViewSubresourceSubset::begin() const
    {
        return CViewSubresourceIterator(*this, m_BeginArray, m_BeginPlane);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceIterator CViewSubresourceSubset::end() const
    {
        return CViewSubresourceIterator(*this, m_BeginArray, m_EndPlane);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceIterator::CViewSubresourceIterator(CViewSubresourceSubset const& SubresourceSet, UINT16 ArraySlice, UINT8 PlaneSlice)
        : m_Subresources(SubresourceSet)
        , m_CurrentArraySlice(ArraySlice)
        , m_CurrentPlaneSlice(PlaneSlice)
    {
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceIterator& CViewSubresourceSubset::CViewSubresourceIterator::operator++()
    {
        assert( m_CurrentArraySlice < m_Subresources.m_EndArray );

        if (++m_CurrentArraySlice >= m_Subresources.m_EndArray )
        {
            assert( m_CurrentPlaneSlice < m_Subresources.m_EndPlane );
            m_CurrentArraySlice= m_Subresources.m_BeginArray;
            ++m_CurrentPlaneSlice;
        }

        return *this;

    }

    //----------------------------------------------------------------------------------------------------------------------------------
    CViewSubresourceSubset::CViewSubresourceIterator& CViewSubresourceSubset::CViewSubresourceIterator::operator--()
    {
        if (m_CurrentArraySlice <= m_Subresources.m_BeginArray)
        {
            m_CurrentArraySlice = m_Subresources.m_EndArray;

            assert( m_CurrentPlaneSlice > m_Subresources.m_BeginPlane );
            --m_CurrentPlaneSlice;
        }

        --m_CurrentArraySlice;

        return *this;

    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool CViewSubresourceSubset::CViewSubresourceIterator::operator==(CViewSubresourceIterator const& other) const
    {
        return &other.m_Subresources == &m_Subresources 
            && other.m_CurrentArraySlice == m_CurrentArraySlice
            && other.m_CurrentPlaneSlice == m_CurrentPlaneSlice;
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    bool CViewSubresourceSubset::CViewSubresourceIterator::operator!=(CViewSubresourceIterator const& other) const
    {
        return !(other == *this);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT CViewSubresourceSubset::CViewSubresourceIterator::StartSubresource() const
    {
        return ComposeSubresourceIdxExtended(m_Subresources.m_BeginMip, m_CurrentArraySlice, m_CurrentPlaneSlice, m_Subresources.m_MipLevels, m_Subresources.m_ArraySlices);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    UINT CViewSubresourceSubset::CViewSubresourceIterator::EndSubresource() const
    {
        return ComposeSubresourceIdxExtended(m_Subresources.m_EndMip, m_CurrentArraySlice, m_CurrentPlaneSlice, m_Subresources.m_MipLevels, m_Subresources.m_ArraySlices);
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    std::pair<UINT, UINT> CViewSubresourceSubset::CViewSubresourceIterator::operator*() const
    {
        return std::make_pair(StartSubresource(), EndSubresource());
    }
};