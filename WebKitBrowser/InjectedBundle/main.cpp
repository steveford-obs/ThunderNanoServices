/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Module.h"

#include <WPE/WebKit.h>
#include <WPE/WebKit/WKBundleFrame.h>
#include <WPE/WebKit/WKBundlePage.h>
#include <WPE/WebKit/WKURL.h>

#include <cstdio>
#include <memory>
#include <syslog.h>

#include "ClassDefinition.h"
#include "NotifyWPEFramework.h"
#include "Utils.h"
#include "WhiteListedOriginDomainsList.h"

using namespace WPEFramework;
using JavaScript::ClassDefinition;
using WebKit::WhiteListedOriginDomainsList;

WKBundleRef g_Bundle;
std::string g_currentURL;

namespace WPEFramework {
namespace WebKit {
namespace Utils {

WKBundleRef GetBundle() {
    return (g_Bundle);
}
std::string GetURL() {
    return (g_currentURL);
}

} } }

static Core::NodeId GetConnectionNode()
{
    string nodeName;

    Core::SystemInfo::GetEnvironment(string(_T("COMMUNICATOR_CONNECTOR")), nodeName);

    return (Core::NodeId(nodeName.c_str()));
}

static class PluginHost {
private:
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

public:
    PluginHost()
        : _engine(Core::ProxyType<RPC::InvokeServerType<2, 0, 4>>::Create())
        , _comClient(Core::ProxyType<RPC::CommunicatorClient>::Create(GetConnectionNode(), Core::ProxyType<Core::IIPCServer>(_engine)))
    {
        _engine->Announcements(_comClient->Announcement());
    }
    ~PluginHost()
    {
        TRACE_L1("Destructing injected bundle stuff!!! [%d]", __LINE__);
        Deinitialize();
    }

public:
    void Initialize(WKBundleRef bundle)
    {
        // We have something to report back, do so...
        uint32_t result = _comClient->Open(RPC::CommunicationTimeOut);
        if (result != Core::ERROR_NONE) {
            TRACE(Trace::Error, (_T("Could not open connection to node %s. Error: %s"), _comClient->Source().RemoteId(), Core::NumberType<uint32_t>(result).Text()));
        } else {
            // Due to the LXC container support all ID's get mapped. For the TraceBuffer, use the host given ID.
            Trace::TraceUnit::Instance().Open(_comClient->ConnectionId());
        }

        _whiteListedOriginDomainPairs = WhiteListedOriginDomainsList::RequestFromWPEFramework();
    }

    void Deinitialize()
    {
        if (_comClient.IsValid() == true) {
            _comClient.Release();
        }

        Core::Singleton::Dispose();
    }

    void WhiteList(WKBundleRef bundle)
    {

        // Whitelist origin/domain pairs for CORS, if set.
        if (_whiteListedOriginDomainPairs) {
            _whiteListedOriginDomainPairs->AddWhiteListToWebKit(bundle);
        }
    }

private:
    Core::ProxyType<RPC::InvokeServerType<2, 0, 4> > _engine;
    Core::ProxyType<RPC::CommunicatorClient> _comClient;

    // White list for CORS.
    std::unique_ptr<WhiteListedOriginDomainsList> _whiteListedOriginDomainPairs;

} _wpeFrameworkClient;

extern "C" {

__attribute__((destructor)) static void unload()
{
    _wpeFrameworkClient.Deinitialize();
}

// Adds class to JS world.
void InjectInJSWorld(ClassDefinition& classDef, WKBundleFrameRef frame, WKBundleScriptWorldRef scriptWorld)
{
    // @Zan: for how long should "ClassDefinition.staticFunctions" remain valid? Can it be
    // released after "JSClassCreate"?

    JSGlobalContextRef context = WKBundleFrameGetJavaScriptContextForWorld(frame, scriptWorld);

    ClassDefinition::FunctionIterator function = classDef.GetFunctions();
    uint32_t functionCount = function.Count();

    // We need an extra entry that we set to all zeroes, to signal end of data.
    // TODO: memleak.
    JSStaticFunction* staticFunctions = new JSStaticFunction[functionCount + 1];

    int index = 0;
    while (function.Next()) {
        staticFunctions[index++] = (*function)->BuildJSStaticFunction();
    }

    staticFunctions[functionCount] = { nullptr, nullptr, 0 };

    // TODO: memleak.
    JSClassDefinition* JsClassDefinition = new JSClassDefinition{
        0, // version
        kJSClassAttributeNone, //attributes
        classDef.GetClassName().c_str(), // className
        0, // parentClass
        nullptr, // staticValues
        staticFunctions, // staticFunctions
        nullptr, //initialize
        nullptr, //finalize
        nullptr, //hasProperty
        nullptr, //getProperty
        nullptr, //setProperty
        nullptr, //deleteProperty
        nullptr, //getPropertyNames
        nullptr, //callAsFunction
        nullptr, //callAsConstructor
        nullptr, //hasInstance
        nullptr, //convertToType
    };

    JSClassRef jsClass = JSClassCreate(JsClassDefinition);
    JSValueRef jsObject = JSObjectMake(context, jsClass, nullptr);
    JSClassRelease(jsClass);

    // @Zan: can we make extension name same as ClassName?
    JSStringRef extensionString = JSStringCreateWithUTF8CString(classDef.GetExtName().c_str());
    JSObjectSetProperty(context, JSContextGetGlobalObject(context), extensionString, jsObject,
        kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, nullptr);
    JSStringRelease(extensionString);
}

static WKBundlePageLoaderClientV6 s_pageLoaderClient = {
    { 6, nullptr },
    nullptr, // didStartProvisionalLoadForFrame
    nullptr, // didReceiveServerRedirectForProvisionalLoadForFrame
    nullptr, // didFailProvisionalLoadWithErrorForFrame
    nullptr, // didCommitLoadForFrame
    nullptr, // didFinishDocumentLoadForFrame
    // didFinishLoadForFrame
    [](WKBundlePageRef pageRef, WKBundleFrameRef frame, WKTypeRef*, const void*) {

        WKBundleFrameRef mainFrame = WKBundlePageGetMainFrame(pageRef);
        WKURLRef mainFrameURL = WKBundleFrameCopyURL(mainFrame);
        WKStringRef urlString = WKURLCopyString(mainFrameURL);
        g_currentURL = WebKit::Utils::WKStringToString(urlString);
        WKRelease(urlString);
        WKRelease(mainFrameURL);
    },
    nullptr, // didFailLoadWithErrorForFrame
    nullptr, // didSameDocumentNavigationForFrame
    nullptr, // didReceiveTitleForFrame
    nullptr, // didFirstLayoutForFrame
    nullptr, // didFirstVisuallyNonEmptyLayoutForFrame
    nullptr, // didRemoveFrameFromHierarchy
    nullptr, // didDisplayInsecureContentForFrame
    nullptr, // didRunInsecureContentForFrame
    // didClearWindowObjectForFrame
    [](WKBundlePageRef, WKBundleFrameRef frame, WKBundleScriptWorldRef scriptWorld, const void*) {
        // Add JS classes to JS world.
        ClassDefinition::Iterator ite = ClassDefinition::GetClassDefinitions();
        while (ite.Next()) {
            InjectInJSWorld(*ite, frame, scriptWorld);
        }
    },
    nullptr, // didCancelClientRedirectForFrame
    nullptr, // willPerformClientRedirectForFrame
    nullptr, // didHandleOnloadEventsForFrame
    nullptr, // didLayoutForFrame
    nullptr, // didNewFirstVisuallyNonEmptyLayout_unavailable
    nullptr, // didDetectXSSForFrame
    nullptr, // shouldGoToBackForwardListItem
    nullptr, // globalObjectIsAvailableForFrame
    nullptr, // willDisconnectDOMWindowExtensionFromGlobalObject
    nullptr, // didReconnectDOMWindowExtensionToGlobalObject
    nullptr, // willDestroyGlobalObjectForDOMWindowExtension
    nullptr, // didFinishProgress
    nullptr, // shouldForceUniversalAccessFromLocalURL
    nullptr, // didReceiveIntentForFrame_unavailable
    nullptr, // registerIntentServiceForFrame_unavailable
    nullptr, // didLayout
    nullptr, // featuresUsedInPage
    nullptr, // willLoadURLRequest
    nullptr, // willLoadDataRequest
};

static WKBundlePageUIClientV4 s_pageUIClient = {
    { 4, nullptr },
    nullptr, // willAddMessageToConsole
    nullptr, // willSetStatusbarText
    nullptr, // willRunJavaScriptAlert
    nullptr, // willRunJavaScriptConfirm
    nullptr, // willRunJavaScriptPrompt
    nullptr, // mouseDidMoveOverElement
    nullptr, // pageDidScroll
    nullptr, // unused1
    nullptr, // shouldGenerateFileForUpload
    nullptr, // generateFileForUpload
    nullptr, // unused2
    nullptr, // statusBarIsVisible
    nullptr, // menuBarIsVisible
    nullptr, // toolbarsAreVisible
    nullptr, // didReachApplicationCacheOriginQuota
    nullptr, // didExceedDatabaseQuota
    nullptr, // createPlugInStartLabelTitle
    nullptr, // createPlugInStartLabelSubtitle
    nullptr, // createPlugInExtraStyleSheet
    nullptr, // createPlugInExtraScript
    nullptr, // unused3
    nullptr, // unused4
    nullptr, // unused5
    nullptr, // didClickAutoFillButton
    nullptr, // willAddDetailedMessageToConsole
};

static WKBundleClientV1 s_bundleClient = {
    { 1, nullptr },
    // didCreatePage
    [](WKBundleRef bundle, WKBundlePageRef page, const void* clientInfo) {
        // Register page loader client, for javascript callbacks.
        WKBundlePageSetPageLoaderClient(page, &s_pageLoaderClient.base);

        // Register UI client, this one will listen to log messages.
        WKBundlePageSetUIClient(page, &s_pageUIClient.base);

        _wpeFrameworkClient.WhiteList(bundle);
    },
    nullptr, // willDestroyPage
    nullptr, // didInitializePageGroup
    nullptr, // didReceiveMessage
    nullptr, // didReceiveMessageToPage
};

// Declare module name for tracer.
MODULE_NAME_DECLARATION(BUILD_REFERENCE)

void WKBundleInitialize(WKBundleRef bundle, WKTypeRef)
{
    g_Bundle = bundle;

    _wpeFrameworkClient.Initialize(bundle);

    WKBundleSetClient(bundle, &s_bundleClient.base);
}
}
