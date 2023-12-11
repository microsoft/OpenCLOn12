// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

namespace D3D12TranslationLayer
{
    class ImmediateContext;

    class DeviceChild
    {
    public:
        DeviceChild(ImmediateContext* pParent) noexcept
            : m_pParent(pParent)
        {
        }

        ImmediateContext* m_pParent = nullptr;
        UINT64 m_LastUsedCommandListID = {};

        // Warning: this method is hidden in some derived child types, and is not virtual
        // Always ensure that this method is called on the most derived type.
        void UsedInCommandList(UINT64 CommandListID) noexcept
        {
            assert(CommandListID >= m_LastUsedCommandListID);
            m_LastUsedCommandListID = CommandListID;
        }

        void MarkUsedInCommandListIfNewer(UINT64 CommandListID) noexcept
        {
            if (CommandListID >= m_LastUsedCommandListID)
            {
                UsedInCommandList(CommandListID);
            }
        }

        void ResetLastUsedInCommandList()
        {
            m_LastUsedCommandListID = 0;
        }

    protected:
        template <typename TIface>
        void AddToDeferredDeletionQueue(unique_comptr<TIface>& spObject)
        {
            if (spObject)
            {
                AddToDeferredDeletionQueue(spObject.get());
                spObject.reset();
            }
        }

        template <typename TIface>
        void AddToDeferredDeletionQueue(unique_comptr<TIface>& spObject, UINT64 CommandListID)
        {
            m_LastUsedCommandListID = CommandListID;
            AddToDeferredDeletionQueue(spObject);
        }

        void AddToDeferredDeletionQueue(ID3D12Object* pObject);
    };

    template <typename TIface>
    class DeviceChildImpl : public DeviceChild
    {
    public:
        DeviceChildImpl(ImmediateContext* pParent) noexcept
            : DeviceChild(pParent)
        {
        }
        void Destroy() { AddToDeferredDeletionQueue(m_spIface); }
        ~DeviceChildImpl() { Destroy(); }

        bool Created() { return m_spIface.get() != nullptr; }
        TIface** GetForCreate() { Destroy(); return &m_spIface; }
        TIface* GetForUse(UINT64 CommandListID)
        {
            MarkUsedInCommandListIfNewer(CommandListID);
            return m_spIface.get();
        }
        TIface* GetForUse()
        {
            return GetForUse(m_pParent->GetCommandListID());
        }
        TIface* GetForImmediateUse() { return m_spIface.get(); }

    private:
        unique_comptr<TIface> m_spIface;
    };
};