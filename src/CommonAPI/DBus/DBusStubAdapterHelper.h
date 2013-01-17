/* Copyright (C) 2013 BMW Group
 * Author: Manfred Bathelt (manfred.bathelt@bmw.de)
 * Author: Juergen Gehring (juergen.gehring@bmw.de)
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef COMMONAPI_DBUS_DBUS_STUB_ADAPTER_HELPER_H_
#define COMMONAPI_DBUS_DBUS_STUB_ADAPTER_HELPER_H_

#include "DBusStubAdapter.h"
#include "DBusInputStream.h"
#include "DBusOutputStream.h"
#include "DBusHelper.h"

#include <memory>
#include <initializer_list>
#include <tuple>
#include <unordered_map>

namespace CommonAPI {
namespace DBus {

template <typename _StubClass>
class DBusStubAdapterHelper: public DBusStubAdapter, public std::enable_shared_from_this<typename _StubClass::StubAdapterType> {
 public:
    typedef typename _StubClass::StubAdapterType StubAdapterType;
    typedef typename _StubClass::RemoteEventHandlerType RemoteEventHandlerType;

    class StubDispatcher {
     public:
        virtual ~StubDispatcher() { }
        virtual bool dispatchDBusMessage(const DBusMessage& dbusMessage, DBusStubAdapterHelper<_StubClass>& dbusStubAdapterHelper) = 0;
    };

 public:
    DBusStubAdapterHelper(const std::string& dbusBusName,
                          const std::string& dbusObjectPath,
                          const std::string& interfaceName,
                          const std::shared_ptr<DBusProxyConnection>& dbusConnection,
                          std::shared_ptr<_StubClass> stub):
                    DBusStubAdapter(dbusBusName, dbusObjectPath, interfaceName, dbusConnection),
                    stub_(stub) {
    }

    virtual ~DBusStubAdapterHelper() { }

    virtual void init() {
        DBusStubAdapter::init();
        remoteEventHandler_ = stub_->initStubAdapter(getStubAdapter());
    }

    inline std::shared_ptr<StubAdapterType> getStubAdapter() {
        return this->shared_from_this();
    }

    inline const std::shared_ptr<_StubClass>& getStub() {
        return stub_;
    }

    inline RemoteEventHandlerType* getRemoteEventHandler() {
        return remoteEventHandler_;
    }

 protected:
    // interfaceMemberName, interfaceMemberSignature
    typedef std::pair<const char*, const char*> DBusInterfaceMemberPath;
    typedef std::unordered_map<DBusInterfaceMemberPath, StubDispatcher*> StubDispatcherTable;

    virtual bool onInterfaceDBusMessage(const DBusMessage& dbusMessage) {
        const char* interfaceMemberName = dbusMessage.getMemberName();
        const char* interfaceMemberSignature = dbusMessage.getSignatureString();

        assert(interfaceMemberName);
        assert(interfaceMemberSignature);

        DBusInterfaceMemberPath dbusInterfaceMemberPath(interfaceMemberName, interfaceMemberSignature);
        auto findIterator = this->stubDispatcherTable_.find(dbusInterfaceMemberPath);
        const bool foundInterfaceMemberHandler = (findIterator != this->stubDispatcherTable_.end());
        bool dbusMessageHandled = false;

        if (foundInterfaceMemberHandler) {
            StubDispatcher* stubDispatcher = findIterator->second;
            dbusMessageHandled = stubDispatcher->dispatchDBusMessage(dbusMessage, *this);
        }

        return dbusMessageHandled;
    }

    std::shared_ptr<_StubClass> stub_;
    RemoteEventHandlerType* remoteEventHandler_;
    static const StubDispatcherTable stubDispatcherTable_;
};

template< class >
struct DBusStubSignalHelper;

template<template<class ...> class _In, class... _InArgs>
struct DBusStubSignalHelper<_In<_InArgs...>> {
    template <typename _DBusStub = DBusStubAdapter>
    static bool sendSignal(const _DBusStub& dbusStub,
                    const char* signalName,
                    const char* signalSignature,
                    const _InArgs&... inArgs) {
        DBusMessage dbusMessage = DBusMessage::createSignal(
                        dbusStub.getObjectPath().c_str(),
                        dbusStub.getInterfaceName(),
                        signalName,
                        signalSignature);

        if (sizeof...(_InArgs) > 0) {
            DBusOutputStream outputStream(dbusMessage);
            const bool success = DBusSerializableArguments<_InArgs...>::serialize(outputStream, inArgs...);
            if (!success) {
                return false;
            }
            outputStream.flush();
        }

        const bool dbusMessageSent = dbusStub.getDBusConnection()->sendDBusMessage(dbusMessage);
        return dbusMessageSent;
    }
};



template< class, class >
class DBusMethodStubDispatcher;

template <
    typename _StubClass,
    template <class...> class _In, class... _InArgs>
class DBusMethodStubDispatcher<_StubClass, _In<_InArgs...> >: public DBusStubAdapterHelper<_StubClass>::StubDispatcher {
 public:
    typedef DBusStubAdapterHelper<_StubClass> DBusStubAdapterHelperType;
    typedef void (_StubClass::*_StubFunctor)(_InArgs...);

    DBusMethodStubDispatcher(_StubFunctor stubFunctor):
            stubFunctor_(stubFunctor) {
    }

    bool dispatchDBusMessage(const DBusMessage& dbusMessage, DBusStubAdapterHelperType& dbusStubAdapterHelper) {
        return handleDBusMessage(dbusMessage, dbusStubAdapterHelper, typename make_sequence<sizeof...(_InArgs)>::type());
    }

 private:
    template <int... _InArgIndices, int... _OutArgIndices>
    inline bool handleDBusMessage(const DBusMessage& dbusMessage,
                                  DBusStubAdapterHelperType& dbusStubAdapterHelper,
                                  index_sequence<_InArgIndices...>) const {
        std::tuple<_InArgs...> argTuple;

        if (sizeof...(_InArgs) > 0) {
            DBusInputStream dbusInputStream(dbusMessage);
            const bool success = DBusSerializableArguments<_InArgs...>::deserialize(dbusInputStream, std::get<_InArgIndices>(argTuple)...);
            if (!success)
                return false;
        }

        (dbusStubAdapterHelper.getStub().get()->*stubFunctor_)(std::move(std::get<_InArgIndices>(argTuple))...);

        return true;
    }

    _StubFunctor stubFunctor_;
};


template< class, class, class >
class DBusMethodWithReplyStubDispatcher;

template <
    typename _StubClass,
    template <class...> class _In, class... _InArgs,
    template <class...> class _Out, class... _OutArgs>
class DBusMethodWithReplyStubDispatcher<_StubClass, _In<_InArgs...>, _Out<_OutArgs...> >:
            public DBusStubAdapterHelper<_StubClass>::StubDispatcher {
 public:
    typedef DBusStubAdapterHelper<_StubClass> DBusStubAdapterHelperType;
    typedef void (_StubClass::*_StubFunctor)(_InArgs..., _OutArgs&...);

    DBusMethodWithReplyStubDispatcher(_StubFunctor stubFunctor, const char* dbusReplySignature):
            stubFunctor_(stubFunctor),
            dbusReplySignature_(dbusReplySignature) {
    }

    bool dispatchDBusMessage(const DBusMessage& dbusMessage, DBusStubAdapterHelperType& dbusStubAdapterHelper) {
        return handleDBusMessage(
                        dbusMessage,
                        dbusStubAdapterHelper,
                        typename make_sequence_range<sizeof...(_InArgs), 0>::type(),
                        typename make_sequence_range<sizeof...(_OutArgs), sizeof...(_InArgs)>::type());
    }

 private:
    template <int... _InArgIndices, int... _OutArgIndices>
    inline bool handleDBusMessage(const DBusMessage& dbusMessage,
                                  DBusStubAdapterHelperType& dbusStubAdapterHelper,
                                  index_sequence<_InArgIndices...>,
                                  index_sequence<_OutArgIndices...>) const {
        std::tuple<_InArgs..., _OutArgs...> argTuple;

        if (sizeof...(_InArgs) > 0) {
            DBusInputStream dbusInputStream(dbusMessage);
            const bool success = DBusSerializableArguments<_InArgs...>::deserialize(dbusInputStream, std::get<_InArgIndices>(argTuple)...);
            if (!success)
                return false;
        }

        (dbusStubAdapterHelper.getStub().get()->*stubFunctor_)(std::move(std::get<_InArgIndices>(argTuple))..., std::get<_OutArgIndices>(argTuple)...);

        DBusMessage dbusMessageReply = dbusMessage.createMethodReturn(dbusReplySignature_);

        if (sizeof...(_OutArgs) > 0) {
            DBusOutputStream dbusOutputStream(dbusMessageReply);
            const bool success = DBusSerializableArguments<_OutArgs...>::serialize(dbusOutputStream, std::get<_OutArgIndices>(argTuple)...);
            if (!success)
                return false;

            dbusOutputStream.flush();
        }

        return dbusStubAdapterHelper.getDBusConnection()->sendDBusMessage(dbusMessageReply);
    }

    _StubFunctor stubFunctor_;
    const char* dbusReplySignature_;
};


template <typename _StubClass, typename _AttributeType>
class DBusGetAttributeStubDispatcher: public DBusStubAdapterHelper<_StubClass>::StubDispatcher {
 public:
    typedef DBusStubAdapterHelper<_StubClass> DBusStubAdapterHelperType;
    typedef const _AttributeType& (_StubClass::*GetStubFunctor)();

    DBusGetAttributeStubDispatcher(GetStubFunctor getStubFunctor, const char* dbusSignature):
        getStubFunctor_(getStubFunctor),
        dbusSignature_(dbusSignature) {
    }

    bool dispatchDBusMessage(const DBusMessage& dbusMessage, DBusStubAdapterHelperType& dbusStubAdapterHelper) {
        return sendAttributeValueReply(dbusMessage, dbusStubAdapterHelper);
    }

 protected:
    inline bool sendAttributeValueReply(const DBusMessage& dbusMessage, DBusStubAdapterHelperType& dbusStubAdapterHelper) {
        DBusMessage dbusMessageReply = dbusMessage.createMethodReturn(dbusSignature_);
        DBusOutputStream dbusOutputStream(dbusMessageReply);

        dbusOutputStream << (dbusStubAdapterHelper.getStub().get()->*getStubFunctor_)();
        dbusOutputStream.flush();

        return dbusStubAdapterHelper.getDBusConnection()->sendDBusMessage(dbusMessageReply);
    }

    GetStubFunctor getStubFunctor_;
    const char* dbusSignature_;
};


template <typename _StubClass, typename _AttributeType>
class DBusSetAttributeStubDispatcher: public DBusGetAttributeStubDispatcher<_StubClass, _AttributeType> {
 public:
    typedef typename DBusGetAttributeStubDispatcher<_StubClass, _AttributeType>::DBusStubAdapterHelperType DBusStubAdapterHelperType;
    typedef typename DBusStubAdapterHelperType::RemoteEventHandlerType RemoteEventHandlerType;

    typedef typename DBusGetAttributeStubDispatcher<_StubClass, _AttributeType>::GetStubFunctor GetStubFunctor;
    typedef bool (RemoteEventHandlerType::*OnRemoteSetFunctor)(_AttributeType);
    typedef void (RemoteEventHandlerType::*OnRemoteChangedFunctor)();

    DBusSetAttributeStubDispatcher(GetStubFunctor getStubFunctor,
                                   OnRemoteSetFunctor onRemoteSetFunctor,
                                   OnRemoteChangedFunctor onRemoteChangedFunctor,
                                   const char* dbusSignature) :
                    DBusGetAttributeStubDispatcher<_StubClass, _AttributeType>(getStubFunctor, dbusSignature),
                    onRemoteSetFunctor_(onRemoteSetFunctor),
                    onRemoteChangedFunctor_(onRemoteChangedFunctor) {
    }

    bool dispatchDBusMessage(const DBusMessage& dbusMessage, DBusStubAdapterHelperType& dbusStubAdapterHelper) {
        bool attributeValueChanged;

        if (!setAttributeValue(dbusMessage, dbusStubAdapterHelper, attributeValueChanged))
            return false;

        if (attributeValueChanged)
            notifyOnRemoteChanged(dbusStubAdapterHelper);

        return true;
    }

 protected:
    inline bool setAttributeValue(const DBusMessage& dbusMessage, DBusStubAdapterHelperType& dbusStubAdapterHelper, bool& attributeValueChanged) {
        DBusInputStream dbusInputStream(dbusMessage);
        _AttributeType attributeValue;
        dbusInputStream >> attributeValue;
        if (dbusInputStream.hasError())
            return false;

        attributeValueChanged = (dbusStubAdapterHelper.getRemoteEventHandler()->*onRemoteSetFunctor_)(std::move(attributeValue));

        return this->sendAttributeValueReply(dbusMessage, dbusStubAdapterHelper);
    }

    inline void notifyOnRemoteChanged(DBusStubAdapterHelperType& dbusStubAdapterHelper) {
        (dbusStubAdapterHelper.getRemoteEventHandler()->*onRemoteChangedFunctor_)();
    }

    inline const _AttributeType& getAttributeValue(DBusStubAdapterHelperType& dbusStubAdapterHelper) {
        return (dbusStubAdapterHelper.getStub().get()->*(this->getStubFunctor_))();
    }

    const OnRemoteSetFunctor onRemoteSetFunctor_;
    const OnRemoteChangedFunctor onRemoteChangedFunctor_;
};


template <typename _StubClass, typename _AttributeType>
class DBusSetObservableAttributeStubDispatcher: public DBusSetAttributeStubDispatcher<_StubClass, _AttributeType> {
 public:
    typedef typename DBusSetAttributeStubDispatcher<_StubClass, _AttributeType>::DBusStubAdapterHelperType DBusStubAdapterHelperType;
    typedef typename DBusStubAdapterHelperType::StubAdapterType StubAdapterType;

    typedef typename DBusSetAttributeStubDispatcher<_StubClass, _AttributeType>::GetStubFunctor GetStubFunctor;
    typedef typename DBusSetAttributeStubDispatcher<_StubClass, _AttributeType>::OnRemoteSetFunctor OnRemoteSetFunctor;
    typedef typename DBusSetAttributeStubDispatcher<_StubClass, _AttributeType>::OnRemoteChangedFunctor OnRemoteChangedFunctor;
    typedef void (StubAdapterType::*FireChangedFunctor)(const _AttributeType&);

    DBusSetObservableAttributeStubDispatcher(GetStubFunctor getStubFunctor,
                                             OnRemoteSetFunctor onRemoteSetFunctor,
                                             OnRemoteChangedFunctor onRemoteChangedFunctor,
                                             FireChangedFunctor fireChangedFunctor,
                                             const char* dbusSignature) :
                    DBusSetAttributeStubDispatcher<_StubClass, _AttributeType>(getStubFunctor,
                                                                               onRemoteSetFunctor,
                                                                               onRemoteChangedFunctor,
                                                                               dbusSignature),
                    fireChangedFunctor_(fireChangedFunctor) {
    }

    bool dispatchDBusMessage(const DBusMessage& dbusMessage, DBusStubAdapterHelperType& dbusStubAdapterHelper) {
        bool attributeValueChanged;
        if (!this->setAttributeValue(dbusMessage, dbusStubAdapterHelper, attributeValueChanged))
            return false;

        if (attributeValueChanged) {
            fireAttributeValueChanged(dbusStubAdapterHelper);
            this->notifyOnRemoteChanged(dbusStubAdapterHelper);
        }
        return true;
    }

 private:
    inline void fireAttributeValueChanged(DBusStubAdapterHelperType& dbusStubAdapterHelper) {
        (dbusStubAdapterHelper.getStubAdapter().get()->*fireChangedFunctor_)(this->getAttributeValue(dbusStubAdapterHelper));
    }

    const FireChangedFunctor fireChangedFunctor_;
};

} // namespace DBus
} // namespace CommonAPI

#endif // COMMONAPI_DBUS_DBUS_STUB_ADAPTER_HELPER_H_
