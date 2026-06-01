#include "minimal_invite_rta/MinimalInviteRtaClient.h"
#include "minimal_invite_rta/minimal_invite_rta_c.h"

#include <httpClient/httpClient.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>

namespace rapidjson
{
void* (*g_pRapidJsonMemAllocHook)(size_t) = malloc;
void (*g_pRapidJsonMemFreeHook)(void*) = free;
}

namespace minimal_invite_rta
{
namespace
{

using JsonDocument = ::rapidjson::Document;
using JsonValue = ::rapidjson::Value;

constexpr const char* kRtaUri = "wss://rta.xboxlive.com/connect";
constexpr const char* kRtaSubprotocol = "rta.xboxlive.com.V2";

enum class MessageType : uint32_t
{
    Subscribe = 1,
    Unsubscribe = 2,
    Event = 3,
    Resync = 4
};

struct SignedRequestData
{
    std::string token;
    std::string signature;
};

struct HttpCallContext
{
    std::mutex mutex;
    std::condition_variable cv;
    HCCallHandle call{};
    HRESULT asyncStatus{E_FAIL};
    HRESULT networkError{S_OK};
    uint32_t platformNetworkError{};
    uint32_t httpStatus{};
    std::string response;
    bool completed{};
};

std::string CreateGuidString()
{
    std::random_device randomDevice;
    std::mt19937_64 generator(randomDevice());
    std::uniform_int_distribution<uint32_t> distribution(0, 0xffffffffu);

    uint32_t data1 = distribution(generator);
    uint16_t data2 = static_cast<uint16_t>(distribution(generator) & 0xffffu);
    uint16_t data3 = static_cast<uint16_t>((distribution(generator) & 0x0fffu) | 0x4000u);
    uint16_t data4 = static_cast<uint16_t>((distribution(generator) & 0x3fffu) | 0x8000u);
    uint64_t data5 = ((static_cast<uint64_t>(distribution(generator)) << 16) |
        static_cast<uint64_t>(distribution(generator) & 0xffffu)) & 0xffffffffffffULL;

    std::ostringstream guid;
    guid << std::hex << std::nouppercase << std::setfill('0')
         << std::setw(8) << data1 << '-'
         << std::setw(4) << data2 << '-'
         << std::setw(4) << data3 << '-'
         << std::setw(4) << data4 << '-'
         << std::setw(12) << data5;
    return guid.str();
}

std::string BuildRtaRegistrationPayload(
    const std::string& systemId,
    const std::string& resourceUri,
    uint32_t titleId,
    const std::string& locale,
    const char* platform)
{
    JsonDocument document;
    document.SetObject();
    auto& allocator = document.GetAllocator();

    document.AddMember("systemId", JsonValue(systemId.c_str(), allocator), allocator);
    document.AddMember("endpointUri", JsonValue(resourceUri.c_str(), allocator), allocator);
    document.AddMember("platform", JsonValue(platform, allocator), allocator);
    document.AddMember("transport", "RTA", allocator);
    document.AddMember("locale", JsonValue(locale.c_str(), allocator), allocator);

    std::string titleIdString = std::to_string(titleId);
    document.AddMember("titleId", JsonValue(titleIdString.c_str(), allocator), allocator);

    JsonValue filters(::rapidjson::kArrayType);

    auto addFilter = [&allocator, &filters](int source, int type)
    {
        JsonValue filter(::rapidjson::kObjectType);
        filter.AddMember("action", "Include", allocator);
        filter.AddMember("source", source, allocator);
        filter.AddMember("type", type, allocator);
        filters.PushBack(filter, allocator);
    };

    addFilter(6, 1);
    addFilter(6, 8);
    addFilter(8, 1);
    document.AddMember("filters", filters, allocator);

    ::rapidjson::StringBuffer buffer;
    ::rapidjson::Writer<::rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    return buffer.GetString();
}

HRESULT GetSignedRequestData(
    XalUserHandle user,
    const char* method,
    const char* url,
    const XalHttpHeader* headers,
    uint32_t headerCount,
    const std::string& body,
    XTaskQueueHandle queue,
    SignedRequestData& signedRequest)
{
    struct AsyncContext
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<uint8_t> buffer;
        HRESULT hr{E_FAIL};
        bool completed{};
    };

    auto async = std::make_shared<AsyncContext>();
    XAsyncBlock block{};
    block.queue = queue;
    block.context = async.get();
    block.callback = [](XAsyncBlock* block)
    {
        auto* context = static_cast<AsyncContext*>(block->context);
        std::lock_guard<std::mutex> lock(context->mutex);
        context->hr = XAsyncGetStatus(block, false);
        context->completed = true;
        context->cv.notify_one();
    };

    XalUserGetTokenAndSignatureArgs args{};
    args.method = method;
    args.url = url;
    args.headerCount = headerCount;
    args.headers = headers;
    args.bodySize = body.size();
    args.body = body.empty() ? nullptr : reinterpret_cast<const uint8_t*>(body.data());
    args.forceRefresh = false;

    HRESULT hr = XalUserGetTokenAndSignatureSilentlyAsync(user, &args, &block);
    if (FAILED(hr))
    {
        return hr;
    }

    {
        std::unique_lock<std::mutex> lock(async->mutex);
        async->cv.wait(lock, [&async]() { return async->completed; });
    }

    if (FAILED(async->hr))
    {
        return async->hr;
    }

    size_t bufferSize{};
    hr = XalUserGetTokenAndSignatureSilentlyResultSize(&block, &bufferSize);
    if (FAILED(hr))
    {
        return hr;
    }

    async->buffer.resize(bufferSize);
    XalUserGetTokenAndSignatureData* auth{};
    hr = XalUserGetTokenAndSignatureSilentlyResult(
        &block,
        async->buffer.size(),
        async->buffer.data(),
        &auth,
        nullptr);
    if (FAILED(hr))
    {
        return hr;
    }

    signedRequest.token = auth && auth->token ? auth->token : "";
    signedRequest.signature = auth && auth->signature ? auth->signature : "";
    return S_OK;
}

HRESULT PerformHttpPostSync(
    XTaskQueueHandle queue,
    const char* url,
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers,
    HttpCallContext& context)
{
    HRESULT hr = HCHttpCallCreate(&context.call);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = HCHttpCallRequestSetUrl(context.call, "POST", url);
    if (FAILED(hr))
    {
        HCHttpCallCloseHandle(context.call);
        context.call = nullptr;
        return hr;
    }

    hr = HCHttpCallRequestSetRequestBodyString(context.call, body.c_str());
    if (FAILED(hr))
    {
        HCHttpCallCloseHandle(context.call);
        context.call = nullptr;
        return hr;
    }

    for (const auto& header : headers)
    {
        hr = HCHttpCallRequestSetHeader(context.call, header.first.c_str(), header.second.c_str(), true);
        if (FAILED(hr))
        {
            HCHttpCallCloseHandle(context.call);
            context.call = nullptr;
            return hr;
        }
    }

    XAsyncBlock async{};
    async.queue = queue;
    async.context = &context;
    async.callback = [](XAsyncBlock* asyncBlock)
    {
        auto* httpContext = static_cast<HttpCallContext*>(asyncBlock->context);
        {
            std::lock_guard<std::mutex> lock(httpContext->mutex);
            httpContext->asyncStatus = XAsyncGetStatus(asyncBlock, false);
            HCHttpCallResponseGetNetworkErrorCode(
                httpContext->call,
                &httpContext->networkError,
                &httpContext->platformNetworkError);
            if (SUCCEEDED(httpContext->asyncStatus))
            {
                HCHttpCallResponseGetStatusCode(httpContext->call, &httpContext->httpStatus);
                const char* responseString = nullptr;
                HCHttpCallResponseGetResponseString(httpContext->call, &responseString);
                if (responseString)
                {
                    httpContext->response = responseString;
                }
            }

            HCHttpCallCloseHandle(httpContext->call);
            httpContext->call = nullptr;
            httpContext->completed = true;
        }
        httpContext->cv.notify_one();
    };

    hr = HCHttpCallPerformAsync(context.call, &async);
    if (FAILED(hr))
    {
        HCHttpCallCloseHandle(context.call);
        context.call = nullptr;
        return hr;
    }

    {
        std::unique_lock<std::mutex> lock(context.mutex);
        context.cv.wait(lock, [&context]() { return context.completed; });
    }

    return context.asyncStatus;
}

std::string GetString(const JsonValue& object, const char* key)
{
    auto it = object.FindMember(key);
    if (it == object.MemberEnd() || !it->value.IsString())
    {
        return {};
    }

    return it->value.GetString();
}

uint64_t GetUint64(const JsonValue& object, const char* key)
{
    auto it = object.FindMember(key);
    if (it == object.MemberEnd())
    {
        return 0;
    }

    if (it->value.IsUint64())
    {
        return it->value.GetUint64();
    }

    if (it->value.IsString())
    {
        return std::strtoull(it->value.GetString(), nullptr, 10);
    }

    return 0;
}

time_t GetTimeT(const JsonValue& object, const char* key)
{
    auto it = object.FindMember(key);
    if (it == object.MemberEnd())
    {
        return 0;
    }

    if (it->value.IsInt64())
    {
        return static_cast<time_t>(it->value.GetInt64());
    }

    if (it->value.IsUint64())
    {
        return static_cast<time_t>(it->value.GetUint64());
    }

    if (it->value.IsString())
    {
        return static_cast<time_t>(std::strtoll(it->value.GetString(), nullptr, 10));
    }

    return 0;
}

std::string SerializeInviteHandleField(const JsonValue& inviteHandle)
{
    JsonDocument document;
    document.SetObject();
    document.AddMember(
        "invite_handle",
        JsonValue(inviteHandle, document.GetAllocator()),
        document.GetAllocator());

    ::rapidjson::StringBuffer buffer;
    ::rapidjson::Writer<::rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    return buffer.GetString();
}

bool TryParseInvite(const JsonValue& inviteHandle, InviteEvent& invite)
{
    if (!inviteHandle.IsObject())
    {
        return false;
    }

    invite.inviteHandle = SerializeInviteHandleField(inviteHandle);
    invite.invitedXboxUserId = GetUint64(inviteHandle, "invitedXuid");
    invite.senderXboxUserId = GetUint64(inviteHandle, "senderXuid");
    invite.inviteHandleId = GetString(inviteHandle, "id");
    invite.inviteProtocol = GetString(inviteHandle, "inviteProtocol");
    invite.expiration = GetTimeT(inviteHandle, "expiration");

    if (inviteHandle.HasMember("inviteAttributes") && inviteHandle["inviteAttributes"].IsObject())
    {
        invite.inviteContext = GetString(inviteHandle["inviteAttributes"], "context");
    }

    if (inviteHandle.HasMember("sessionRef") && inviteHandle["sessionRef"].IsObject())
    {
        const auto& sessionRef = inviteHandle["sessionRef"];
        invite.sessionReference.scid = GetString(sessionRef, "scid");
        invite.sessionReference.templateName = GetString(sessionRef, "templateName");
        invite.sessionReference.sessionName = GetString(sessionRef, "name");
    }

    if (!inviteHandle.HasMember("inviteInfo") || !inviteHandle["inviteInfo"].IsObject())
    {
        return false;
    }

    const auto& info = inviteHandle["inviteInfo"];
    invite.senderGamertag = GetString(info, "sender");
    invite.senderModernGamertag = GetString(info, "senderModernGamertag");
    invite.senderModernGamertagSuffix = GetString(info, "senderModernGamertagSuffix");
    invite.senderUniqueModernGamertag = GetString(info, "senderUniqueModernGamertag");
    invite.senderImageUrl = GetString(info, "senderImageUrl");
    return true;
}

} // namespace

struct MinimalInviteRtaClient::Impl
{
    struct ServiceSubscription
    {
        std::string uri;
        uint32_t clientId{1};
        uint32_t serviceId{};
    };

    struct AsyncContext
    {
        std::shared_ptr<MinimalInviteRtaClient> owner;
        XAsyncBlock block{};
        std::vector<uint8_t> buffer;

        explicit AsyncContext(std::shared_ptr<MinimalInviteRtaClient> value, XTaskQueueHandle queue) :
            owner(std::move(value))
        {
            block.queue = queue;
            block.context = this;
        }
    };

    explicit Impl(const Config& config) :
        user(config.user),
        queue(config.queue),
        xuid(config.xuid),
        titleId(config.titleId),
        locale(config.locale ? config.locale : "en-US"),
        userAgent(config.userAgent ? config.userAgent : "MinimalInviteRtaClient/1.0"),
        systemId(CreateGuidString()),
        autoReconnect(config.autoReconnect)
    {
    }

    ~Impl()
    {
        shuttingDown.store(true);
        if (websocket)
        {
            HCWebSocketDisconnect(websocket);
            HCWebSocketCloseHandle(websocket);
            websocket = nullptr;
        }
    }

    static Impl& From(MinimalInviteRtaClient& owner)
    {
        return *owner.m_impl;
    }

    static const Impl& From(const MinimalInviteRtaClient& owner)
    {
        return *owner.m_impl;
    }

    void NotifyState(bool connected)
    {
        StateHandler handler;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            handler = stateHandler;
        }

        if (handler)
        {
            handler(connected);
        }
    }

    void OnMessage(std::string_view message)
    {
        JsonDocument json;
        json.Parse(message.data(), message.size());
        if (json.HasParseError() || !json.IsArray() || json.Empty() || !json[0].IsUint())
        {
            return;
        }

        switch (static_cast<MessageType>(json[0].GetUint()))
        {
        case MessageType::Subscribe:
            OnSubscribeResponse(json);
            break;
        case MessageType::Event:
            OnEvent(json);
            break;
        case MessageType::Resync:
            owner->SendSubscribe();
            break;
        default:
            break;
        }
    }

    void OnSubscribeResponse(const JsonValue& message)
    {
        if (message.Size() < 4 || !message[2].IsUint())
        {
            return;
        }

        std::printf("RTA subscribe response code=%u\n", message[2].GetUint());
        if (message[3].IsString())
        {
            std::printf("RTA subscribe response detail=%s\n", message[3].GetString());
        }

        if (message[2].GetUint() == 0 && message[3].IsUint())
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            subscription.serviceId = message[3].GetUint();
            std::printf("RTA subscribe serviceId=%u resource=%s\n", subscription.serviceId, subscription.uri.c_str());
        }
    }

    void OnEvent(const JsonValue& message)
    {
        if (message.Size() < 3 || !message[1].IsUint())
        {
            return;
        }

        InviteHandler handler;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            if (message[1].GetUint() != subscription.serviceId)
            {
                return;
            }
            handler = inviteHandler;
        }

        const auto& payload = message[2];
        if (!payload.IsObject() || !payload.HasMember("inviteHandle"))
        {
            return;
        }

        InviteEvent invite;
        if (!TryParseInvite(payload["inviteHandle"], invite))
        {
            return;
        }

        if (handler)
        {
            handler(invite);
        }
    }

    HRESULT EnsureWebSocket()
    {
        if (websocket)
        {
            return S_OK;
        }

        return HCWebSocketCreate(
            &websocket,
            &Impl::ReceiveHandler,
            &Impl::BinaryReceiveHandler,
            &Impl::CloseHandler,
            owner);
    }

    static void CALLBACK ReceiveHandler(HCWebsocketHandle, const char* message, void* context)
    {
        auto* client = static_cast<MinimalInviteRtaClient*>(context);
        From(*client).OnMessage(message ? message : "");
    }

    static void CALLBACK BinaryReceiveHandler(HCWebsocketHandle, const uint8_t* payload, uint32_t payloadSize, void* context)
    {
        auto* client = static_cast<MinimalInviteRtaClient*>(context);
        std::string message(reinterpret_cast<const char*>(payload), payloadSize);
        From(*client).OnMessage(message);
    }

    static void CALLBACK CloseHandler(HCWebsocketHandle, HCWebSocketCloseStatus, void* context)
    {
        auto* client = static_cast<MinimalInviteRtaClient*>(context);
        auto& impl = From(*client);
        impl.NotifyState(false);

        if (!impl.shuttingDown.load() && impl.autoReconnect.load())
        {
            client->Connect();
        }
    }

    MinimalInviteRtaClient* owner{};
    XalUserHandle user{};
    XTaskQueueHandle queue{};
    uint64_t xuid{};
    uint32_t titleId{};
    std::string locale;
    std::string userAgent;
    std::string systemId;
    HCWebsocketHandle websocket{};
    std::atomic<bool> shuttingDown{false};
    std::atomic<bool> autoReconnect{true};
    std::mutex callbackMutex;
    ServiceSubscription subscription;
    InviteHandler inviteHandler;
    StateHandler stateHandler;
};

std::shared_ptr<MinimalInviteRtaClient> MinimalInviteRtaClient::Create(const Config& config)
{
    auto client = std::shared_ptr<MinimalInviteRtaClient>(new MinimalInviteRtaClient(config));
    client->m_impl->owner = client.get();
    return client;
}

MinimalInviteRtaClient::MinimalInviteRtaClient(const Config& config) :
    m_impl(std::make_unique<Impl>(config))
{
}

MinimalInviteRtaClient::~MinimalInviteRtaClient() = default;

void MinimalInviteRtaClient::SetInviteHandler(InviteHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->callbackMutex);
    m_impl->inviteHandler = std::move(handler);
}

void MinimalInviteRtaClient::SetStateHandler(StateHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->callbackMutex);
    m_impl->stateHandler = std::move(handler);
}

void MinimalInviteRtaClient::SetAutoReconnect(bool enabled)
{
    m_impl->autoReconnect.store(enabled);
}

HRESULT MinimalInviteRtaClient::Connect()
{
    if (!m_impl->user || m_impl->titleId == 0)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = m_impl->EnsureWebSocket();
    if (FAILED(hr))
    {
        return hr;
    }

    if (m_impl->xuid == 0)
    {
        hr = XalUserGetId(m_impl->user, &m_impl->xuid);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    hr = RegisterFcmToken(nullptr);
    if (FAILED(hr))
    {
        return hr;
    }

    auto async = std::make_unique<Impl::AsyncContext>(shared_from_this(), m_impl->queue);
    async->block.callback = [](XAsyncBlock* block)
    {
        std::unique_ptr<Impl::AsyncContext> asyncContext(static_cast<Impl::AsyncContext*>(block->context));
        auto owner = asyncContext->owner;
        auto& impl = *owner->m_impl;

        size_t bufferSize{};
        HRESULT hr = XalUserGetTokenAndSignatureSilentlyResultSize(block, &bufferSize);
        if (FAILED(hr))
        {
            impl.NotifyState(false);
            return;
        }

        asyncContext->buffer.resize(bufferSize);
        XalUserGetTokenAndSignatureData* auth{};
        hr = XalUserGetTokenAndSignatureSilentlyResult(
            block,
            asyncContext->buffer.size(),
            asyncContext->buffer.data(),
            &auth,
            nullptr);
        if (FAILED(hr) || auth == nullptr || auth->token == nullptr || auth->signature == nullptr)
        {
            impl.NotifyState(false);
            return;
        }

        HCWebSocketSetHeader(impl.websocket, "Authorization", auth->token);
        HCWebSocketSetHeader(impl.websocket, "Accept-Language", impl.locale.c_str());
        HCWebSocketSetHeader(impl.websocket, "User-Agent", impl.userAgent.c_str());

        auto connect = std::make_unique<Impl::AsyncContext>(owner, impl.queue);
        connect->block.callback = [](XAsyncBlock* block)
        {
            std::unique_ptr<Impl::AsyncContext> connectContext(static_cast<Impl::AsyncContext*>(block->context));
            auto owner = connectContext->owner;
            auto& impl = *owner->m_impl;

            WebSocketCompletionResult result{};
            HRESULT hr = HCGetWebSocketConnectResult(block, &result);
            if (FAILED(hr) || FAILED(result.errorCode))
            {
                impl.NotifyState(false);
                return;
            }

            impl.NotifyState(true);
            owner->m_impl->subscription.uri = owner->ResourceUri();
            owner->m_impl->subscription.clientId = 1;
            owner->SendSubscribe();
        };

        hr = HCWebSocketConnectAsync(kRtaUri, kRtaSubprotocol, impl.websocket, &connect->block);
        if (SUCCEEDED(hr))
        {
            connect.release();
        }
        else
        {
            impl.NotifyState(false);
        }
    };

    XalUserGetTokenAndSignatureArgs args{};
    args.method = "GET";
    args.url = kRtaUri;
    args.forceRefresh = false;

    hr = XalUserGetTokenAndSignatureSilentlyAsync(m_impl->user, &args, &async->block);
    if (SUCCEEDED(hr))
    {
        async.release();
    }
    return hr;
}

HRESULT MinimalInviteRtaClient::Disconnect()
{
    m_impl->autoReconnect.store(false);
    return m_impl->websocket ? HCWebSocketDisconnect(m_impl->websocket) : S_OK;
}

HRESULT MinimalInviteRtaClient::RegisterFcmToken(const char* fcmToken)
{
    (void)fcmToken;

    if (!m_impl->user || m_impl->titleId == 0)
    {
        return E_INVALIDARG;
    }

    const std::string resourceUri = ResourceUri();

    constexpr const char* url = "https://notify.xboxlive.com/system/notifications/endpoints";
    const std::string body = BuildRtaRegistrationPayload(
        m_impl->systemId,
        resourceUri,
        m_impl->titleId,
        m_impl->locale,
        "Win32");

    const XalHttpHeader signingHeaders[] =
    {
        { "Accept", "application/json" },
        { "Content-Type", "application/json; charset=utf-8" },
        { "X-ReportErrorAsSuccess", "true" },
        { "User-Agent", "libHttpClient/1.0.0.0" }
    };

    SignedRequestData signedRequest;
    HRESULT hr = GetSignedRequestData(
        m_impl->user,
        "POST",
        url,
        signingHeaders,
        static_cast<uint32_t>(std::size(signingHeaders)),
        body,
        m_impl->queue,
        signedRequest);
    if (FAILED(hr))
    {
        return hr;
    }

    std::printf("RTA registration resourceUri %s\n", resourceUri.c_str());
    std::printf("RTA registration payload %s\n", body.c_str());

    HttpCallContext httpContext{};
    hr = PerformHttpPostSync(
        m_impl->queue,
        url,
        body,
        {
            { "Accept", "application/json" },
            { "Authorization", signedRequest.token },
            { "Content-Type", "application/json; charset=utf-8" },
            { "User-Agent", "libHttpClient/1.0.0.0" },
            { "X-ReportErrorAsSuccess", "true" }
        },
        httpContext);
    if (FAILED(hr))
    {
        return hr;
    }

    std::printf("RTA registration HTTP status %u\n", httpContext.httpStatus);
    if (!httpContext.response.empty())
    {
        std::printf("RTA registration response %s\n", httpContext.response.c_str());
    }
    if (FAILED(httpContext.networkError))
    {
        std::printf("RTA registration network error 0x%x platformError=%u\n", static_cast<int>(httpContext.networkError), httpContext.platformNetworkError);
    }

    return (httpContext.httpStatus >= 200 && httpContext.httpStatus < 300) ? S_OK : E_FAIL;
}

std::string MinimalInviteRtaClient::ResourceUri() const
{
    return "https://notify.xboxlive.com/users/xuid(" + std::to_string(m_impl->xuid) +
        ")/deviceId/current/titleId/" + std::to_string(m_impl->titleId);
}

void MinimalInviteRtaClient::SendSubscribe()
{
    std::string message;
    {
        std::lock_guard<std::mutex> lock(m_impl->callbackMutex);
        m_impl->subscription.uri = ResourceUri();
        message = "[1," + std::to_string(m_impl->subscription.clientId) + ",\"" + m_impl->subscription.uri + "\"]";
    }

    std::printf("RTA subscribe payload %s\n", message.c_str());

    auto async = std::make_unique<Impl::AsyncContext>(shared_from_this(), m_impl->queue);
    async->block.callback = [](XAsyncBlock* block)
    {
        std::unique_ptr<Impl::AsyncContext> asyncContext(static_cast<Impl::AsyncContext*>(block->context));
        (void)asyncContext;
    };

    if (SUCCEEDED(HCWebSocketSendMessageAsync(m_impl->websocket, message.c_str(), &async->block)))
    {
        async.release();
    }
}

} // namespace minimal_invite_rta

struct MinimalInviteRtaClientHandle
{
    std::shared_ptr<minimal_invite_rta::MinimalInviteRtaClient> client;
    std::string resourceUri;
    MinimalInviteRtaInviteCallback inviteCallback{};
    void* inviteContext{};
    MinimalInviteRtaStateCallback stateCallback{};
    void* stateContext{};
};

namespace
{

MinimalInviteRtaInviteEvent ToCInviteEvent(const minimal_invite_rta::InviteEvent& invite)
{
    MinimalInviteRtaInviteEvent result{};
    result.invitedXboxUserId = invite.invitedXboxUserId;
    result.senderXboxUserId = invite.senderXboxUserId;
    result.senderGamertag = invite.senderGamertag.c_str();
    result.senderModernGamertag = invite.senderModernGamertag.c_str();
    result.senderModernGamertagSuffix = invite.senderModernGamertagSuffix.c_str();
    result.senderUniqueModernGamertag = invite.senderUniqueModernGamertag.c_str();
    result.senderImageUrl = invite.senderImageUrl.c_str();
    result.inviteHandle = invite.inviteHandle.c_str();
    result.inviteHandleId = invite.inviteHandleId.c_str();
    result.inviteProtocol = invite.inviteProtocol.c_str();
    result.inviteContext = invite.inviteContext.c_str();
    result.expiration = static_cast<int64_t>(invite.expiration);
    result.sessionReference.scid = invite.sessionReference.scid.c_str();
    result.sessionReference.templateName = invite.sessionReference.templateName.c_str();
    result.sessionReference.sessionName = invite.sessionReference.sessionName.c_str();
    return result;
}

} // namespace

MINIMAL_INVITE_RTA_EXPORT uint32_t MinimalInviteRtaGetAbiVersion(void)
{
    return 1;
}

MINIMAL_INVITE_RTA_EXPORT MinimalInviteRtaClientHandle* MinimalInviteRtaCreate(
    const MinimalInviteRtaConfig* config)
{
    if (config == nullptr)
    {
        return nullptr;
    }

    minimal_invite_rta::MinimalInviteRtaClient::Config cppConfig{};
    cppConfig.user = config->user;
    cppConfig.queue = config->queue;
    cppConfig.xuid = config->xuid;
    cppConfig.titleId = config->titleId;
    cppConfig.locale = config->locale ? config->locale : "en-US";
    cppConfig.userAgent = config->userAgent ? config->userAgent : "MinimalInviteRtaClient/1.0";
    cppConfig.autoReconnect = config->autoReconnect;

    auto handle = std::make_unique<MinimalInviteRtaClientHandle>();
    handle->client = minimal_invite_rta::MinimalInviteRtaClient::Create(cppConfig);
    if (!handle->client)
    {
        return nullptr;
    }

    handle->client->SetInviteHandler([rawHandle = handle.get()](const minimal_invite_rta::InviteEvent& invite)
    {
        if (rawHandle->inviteCallback == nullptr)
        {
            return;
        }

        MinimalInviteRtaInviteEvent cInvite = ToCInviteEvent(invite);
        rawHandle->inviteCallback(rawHandle->inviteContext, &cInvite);
    });

    handle->client->SetStateHandler([rawHandle = handle.get()](bool connected)
    {
        if (rawHandle->stateCallback)
        {
            rawHandle->stateCallback(rawHandle->stateContext, connected);
        }
    });

    handle->resourceUri = handle->client->ResourceUri();
    return handle.release();
}

MINIMAL_INVITE_RTA_EXPORT void MinimalInviteRtaDestroy(
    MinimalInviteRtaClientHandle* handle)
{
    delete handle;
}

MINIMAL_INVITE_RTA_EXPORT void MinimalInviteRtaSetInviteCallback(
    MinimalInviteRtaClientHandle* handle,
    MinimalInviteRtaInviteCallback callback,
    void* context)
{
    if (handle == nullptr)
    {
        return;
    }

    handle->inviteCallback = callback;
    handle->inviteContext = context;
}

MINIMAL_INVITE_RTA_EXPORT void MinimalInviteRtaSetStateCallback(
    MinimalInviteRtaClientHandle* handle,
    MinimalInviteRtaStateCallback callback,
    void* context)
{
    if (handle == nullptr)
    {
        return;
    }

    handle->stateCallback = callback;
    handle->stateContext = context;
}

MINIMAL_INVITE_RTA_EXPORT void MinimalInviteRtaSetAutoReconnect(
    MinimalInviteRtaClientHandle* handle,
    bool enabled)
{
    if (handle == nullptr || !handle->client)
    {
        return;
    }

    handle->client->SetAutoReconnect(enabled);
}

MINIMAL_INVITE_RTA_EXPORT HRESULT MinimalInviteRtaConnect(
    MinimalInviteRtaClientHandle* handle)
{
    if (handle == nullptr || !handle->client)
    {
        return E_INVALIDARG;
    }

    handle->resourceUri = handle->client->ResourceUri();
    return handle->client->Connect();
}

MINIMAL_INVITE_RTA_EXPORT HRESULT MinimalInviteRtaDisconnect(
    MinimalInviteRtaClientHandle* handle)
{
    if (handle == nullptr || !handle->client)
    {
        return E_INVALIDARG;
    }

    return handle->client->Disconnect();
}

MINIMAL_INVITE_RTA_EXPORT HRESULT MinimalInviteRtaRegisterFcmToken(
    MinimalInviteRtaClientHandle* handle,
    const char* fcmToken)
{
    if (handle == nullptr || !handle->client)
    {
        return E_INVALIDARG;
    }

    return handle->client->RegisterFcmToken(fcmToken);
}

MINIMAL_INVITE_RTA_EXPORT const char* MinimalInviteRtaGetResourceUri(
    MinimalInviteRtaClientHandle* handle)
{
    if (handle == nullptr || !handle->client)
    {
        return nullptr;
    }

    handle->resourceUri = handle->client->ResourceUri();
    return handle->resourceUri.c_str();
}

#define cryptstring(x) x
#define dlsym_ptr(a, b) dlsym(a, b)
#define dlopen_ptr(a, b) dlopen(a, b)
#define IF_DEBUG(a) a
#include <iostream>
#include <dlfcn.h>
#include <jni.h>
#include <sstream>

static JavaVM *_vm;

jint JNI_OnLoad( JavaVM *vm, void *reserved )
{
    _vm = vm;
    (void)reserved;

    return JNI_VERSION_1_6;
}

extern "C" void Java_com_mojang_minecraftpe_NotificationListenerService_nativePushNotificationReceived(JNIEnv* env, jobject obj, jint type, jstring title, jstring body, jstring payload);

static decltype(&XalTryAddDefaultUserSilentlyResult) _XalTryAddDefaultUserSilentlyResult;

static HRESULT mockXalTryAddDefaultUserSilentlyResult(XAsyncBlock* async, XalUserHandle* newUser) {
    using namespace minimal_invite_rta;
    HRESULT hr = _XalTryAddDefaultUserSilentlyResult(async, newUser);
    if(hr == 0) {
        XalUserHandle user;
        XalUserDuplicateHandle(*newUser, &user);
        std::thread([user]() {
            MinimalInviteRtaClient::Config conf;
            conf.user = user;
            XalUserGetId(conf.user, &conf.xuid);
            conf.titleId = 0x67b57dac;

            static auto client = MinimalInviteRtaClient::Create(conf);
            client->RegisterFcmToken(nullptr);
            client->SetInviteHandler([&](const InviteEvent& ev) {
                printf("Invite %s\n", ev.senderGamertag.c_str());
                printf("sessionName %s\n", ev.sessionReference.sessionName.c_str());
                printf("templateName %s\n", ev.sessionReference.templateName.c_str());
                printf("senderXboxUserId %lld\n", (long long)ev.senderXboxUserId);
                printf("invitedXboxUserId %lld\n", (long long)ev.invitedXboxUserId);
                printf("inviteHandleId %s\n", ev.inviteHandleId.c_str());
                printf("inviteContext %s\n", ev.inviteContext.c_str());
                JNIEnv* env;
                _vm->GetEnv((void**)&env, JNI_VERSION_1_6);
                Java_com_mojang_minecraftpe_NotificationListenerService_nativePushNotificationReceived(env, nullptr, 1, env->NewStringUTF(ev.senderGamertag.c_str()), env->NewStringUTF("You have been invited"), env->NewStringUTF(ev.inviteHandle.c_str()));
            });
            client->SetStateHandler([&](bool connected) {
                printf("State Event %d\n", (int)connected);
            });
            client->Connect();
        }).detach();
    }
    return hr;
}

MINIMAL_INVITE_RTA_EXPORT void mod_init() {
    auto minecraft = dlopen_ptr(cryptstring("libminecraftpe.so"), RTLD_NOLOAD);
    auto mcpelauncher_mod = dlopen_ptr(cryptstring("libmcpelauncher_mod.so"), RTLD_NOLOAD);
    if(mcpelauncher_mod == nullptr) {
        IF_DEBUG(std::cout << cryptstring("mcpelauncher_mod not found") << std::endl);
        return;
    }

    auto mcpelauncher_relocate =
        (void (*)(void* handle, const char* name, void* hook))dlsym_ptr(mcpelauncher_mod, cryptstring("mcpelauncher_relocate"));
    
    if(!mcpelauncher_relocate) {
        IF_DEBUG(std::cout << cryptstring("mcpelauncher_relocate not found") << std::endl);
        return;
    }
    _XalTryAddDefaultUserSilentlyResult = &XalTryAddDefaultUserSilentlyResult;
    mcpelauncher_relocate(minecraft, "XalTryAddDefaultUserSilentlyResult", (void*)&mockXalTryAddDefaultUserSilentlyResult);
}
