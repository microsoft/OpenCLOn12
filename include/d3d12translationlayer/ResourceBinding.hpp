// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include <bitset>

namespace D3D12TranslationLayer
{
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Resource state tracking structures
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //----------------------------------------------------------------------------------------------------------------------------------
    // Binding helpers
    // Tracks dirty bits, calls Bound/Unbound functions on binding changes,
    // and tracks binding data from shader decls to allow binding typed/additional NULLs
    //----------------------------------------------------------------------------------------------------------------------------------
    // Base class
    template <typename TBindable, UINT NumBindSlots>
    class CBoundState
    {
    public:
        static const UINT NumBindings = NumBindSlots;

    public:
        CBoundState() = default;

        bool DirtyBitsUpTo(_In_range_(0, NumBindings) UINT slot) const noexcept;
        void SetDirtyBit(_In_range_(0, NumBindings - 1) UINT slot) noexcept { m_DirtyBits.set(slot); }
        void SetDirtyBits(std::bitset<NumBindSlots> const& bits) noexcept { m_DirtyBits |= bits; }

        TBindable* const* GetBound() const noexcept { return m_Bound; }
        void ResetDirty(UINT slot) noexcept { m_DirtyBits.set(slot, false); }

        _Ret_range_(0, NumBindings) UINT GetNumBound() const noexcept { return m_NumBound; }
        
        bool UpdateBinding(_In_range_(0, NumBindings - 1) UINT slot, _In_opt_ TBindable* pBindable) noexcept
        {
            auto& Current = m_Bound[slot];
            if (pBindable)
            {
                m_NumBound = max(m_NumBound, slot + 1);
            }
            if (Current != pBindable)
            {
                Current = pBindable;
                if (!pBindable)
                {
                    TrimNumBound();
                }
                m_DirtyBits.set(slot);
                return true;
            }
            return false;
        }

        void Clear()
        {
            for (UINT i = 0; i < m_NumBound; ++i)
            {
                UpdateBinding(i, nullptr);
            }
        }

    protected:
        void TrimNumBound()
        {
            while (m_NumBound > 0 && !m_Bound[m_NumBound - 1])
            {
                --m_NumBound;
            }
        }

    protected:
        TBindable* m_Bound[NumBindings] = {};
        std::bitset<NumBindings> m_DirtyBits;
        _Field_range_(0, NumBindings) UINT m_NumBound = 0;
    };

    //----------------------------------------------------------------------------------------------------------------------------------
    // SRV, UAV
    template <typename TBindable, UINT NumBindSlots>
    class CViewBoundState : public CBoundState<TBindable, NumBindSlots>
    {
    public:
        typedef TDeclVector::value_type NullType;
        typedef D3D12_CPU_DESCRIPTOR_HANDLE Descriptor;
        static const NullType c_AnyNull = RESOURCE_DIMENSION::UNKNOWN;

    public:
        CViewBoundState() noexcept(false)
            : CBoundState<TBindable, NumBindSlots>()
        {
            m_ShaderData.reserve(NumBindings); // throw( bad_alloc )
        }

        bool UpdateBinding(_In_range_(0, NumBindings - 1) UINT slot, _In_opt_ TBindable* pBindable) noexcept;
        bool IsDirty(TDeclVector const& New, UINT rootSignatureBucketSize, bool bKnownDirty) noexcept;

        NullType GetNullType(_In_range_(0, NumBindings - 1) UINT slot) const noexcept
        {
            if (slot >= m_ShaderData.size())
                return c_AnyNull;
            return m_ShaderData[slot];
        }

        void FillDescriptors(_Out_writes_(NumBindings) Descriptor* pDescriptors,
            _In_reads_(D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY) Descriptor* pNullDescriptors,
            _In_range_(0, NumBindings) UINT RootSignatureHWM) noexcept
        {
            for (UINT i = 0; i < RootSignatureHWM; ++i)
            {
                if (this->m_Bound[i])
                {
                    pDescriptors[i] = this->m_Bound[i]->GetRefreshedDescriptorHandle();
                }
                else
                {
                    pDescriptors[i] = pNullDescriptors[(UINT)GetNullType(i)];
                }
                this->m_DirtyBits.set(i, false);
            }
        }

        void Clear()
        {
            for (UINT i = 0; i < NumBindSlots; ++i)
            {
                UpdateBinding(i, nullptr);
            }
        }

    protected:
        TDeclVector m_ShaderData;
    };

    //----------------------------------------------------------------------------------------------------------------------------------
    class CConstantBufferBoundState : public CBoundState<Resource, D3D11_COMMONSHADER_CONSTANT_BUFFER_HW_SLOT_COUNT>
    {
    public:
        CConstantBufferBoundState() noexcept
            : CBoundState()
        {
        }

        bool UpdateBinding(_In_range_(0, NumBindings - 1) UINT slot, _In_opt_ Resource* pBindable) noexcept;

        bool IsDirty(_In_range_(0, NumBindings) UINT rootSignatureBucketSize) noexcept
        {
            bool bDirty = rootSignatureBucketSize > m_ShaderData || DirtyBitsUpTo(rootSignatureBucketSize);
            m_ShaderData = rootSignatureBucketSize;

            return bDirty;
        }

        void Clear()
        {
            for (UINT i = 0; i < NumBindings; ++i)
            {
                UpdateBinding(i, nullptr);
            }
        }
    protected:
        _Field_range_(0, NumBindings) UINT m_ShaderData = 0;
    };

    //----------------------------------------------------------------------------------------------------------------------------------
    class CSamplerBoundState : public CBoundState<Sampler, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT>
    {
    public:
        typedef D3D12_CPU_DESCRIPTOR_HANDLE Descriptor;

    public:
        CSamplerBoundState() noexcept
            : CBoundState()
        {
        }

        bool UpdateBinding(_In_range_(0, NumBindings - 1) UINT slot,
            _In_ Sampler* pBindable) noexcept;

        bool IsDirty(_In_range_(0, NumBindings) UINT rootSignatureBucketSize) noexcept
        {
            bool bDirty = rootSignatureBucketSize > m_ShaderData || DirtyBitsUpTo(rootSignatureBucketSize);
            m_ShaderData = rootSignatureBucketSize;

            return bDirty;
        }

        void FillDescriptors(_Out_writes_(NumBindings) Descriptor* pDescriptors, Descriptor* pNullDescriptor,
            _In_range_(0, NumBindings) UINT RootSignatureHWM) noexcept
        {
            for (UINT i = 0; i < RootSignatureHWM; ++i)
            {
                pDescriptors[i] = (m_Bound[i]) ? m_Bound[i]->m_Descriptor : *pNullDescriptor;
                m_DirtyBits.set(i, false);
            }
        }

        void Clear()
        {
            for (UINT i = 0; i < NumBindings; ++i)
            {
                UpdateBinding(i, nullptr);
            }
        }

    protected:
        _Field_range_(0, NumBindings) UINT m_ShaderData = 0;
    };

};
