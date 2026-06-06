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
#include <fstream>
#include <zlib.h>
#include <playapi/checkin.h>
#include <playapi/device_info.h>

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

std::string BuildFCMRegistrationPayload(
    const std::string& systemId,
    const std::string& resourceUri,
    uint32_t titleId,
    const std::string& locale)
{
    JsonDocument document;
    document.SetObject();
    auto& allocator = document.GetAllocator();

    document.AddMember("systemId", JsonValue(systemId.c_str(), allocator), allocator);
    document.AddMember("endpointUri", JsonValue(resourceUri.c_str(), allocator), allocator);
    document.AddMember("platform", "Android", allocator);
    document.AddMember("transport", "FCM", allocator);
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
    const std::string body = BuildFCMRegistrationPayload(
        m_impl->systemId,
        fcmToken,
        m_impl->titleId,
        m_impl->locale);
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

HRESULT DeleteInstallationToken(std::string fid, std::string refreshToken);
HRESULT CreateInstallationToken(std::string& fid, std::string& tkn, std::string& refToken);
HRESULT RegisterDevice(std::string& tkn, long androidId, long securityToken, std::string fid, std::string installToken);
HRESULT ListenMTalk(long androidId, long securityToken);
HRESULT GetAuthToken(std::string fid, std::string refreshToken, std::string& tkn);

static HRESULT mockXalTryAddDefaultUserSilentlyResult(XAsyncBlock* async, XalUserHandle* newUser) {
    using namespace minimal_invite_rta;
    HRESULT hr = _XalTryAddDefaultUserSilentlyResult(async, newUser);
    if(hr == 0) {
        XalUserHandle user;
        XalUserDuplicateHandle(*newUser, &user);
        std::thread([user]() {
            std::string fid;
            std::string tkn;
            std::string refToken;
            std::string installation = "/data/data/com.mojang.minecraftpe/installation.json";
            long android_Id = 0;
            long security_token = 0;
            std::ifstream in(installation);
            if (in.is_open()) {
                rapidjson::Document document;
                std::stringstream ss;
                ss << in.rdbuf();
                document.Parse(ss.str().data());
                if (document.IsObject() && document.HasMember("fid") && document.HasMember("refToken") && document["fid"].IsString() && document["refToken"].IsString()) {
                    fid = document["fid"].GetString();
                    refToken = document["refToken"].GetString();
                    android_Id = std::stol(document["android_id"].GetString());
                    security_token = std::stol(document["security_token"].GetString());
                    GetAuthToken(fid, refToken, tkn);
                }
            } else {
                playapi::device_info device;
                playapi::checkin_api checkin(device);
                auto checkinResult = checkin.perform_anonymous_checkin()->call();
                android_Id = checkinResult.android_id;
                security_token = checkinResult.security_token;

                CreateInstallationToken(fid, tkn, refToken);

                rapidjson::Document document;
                document.SetObject();
                auto& allocator = document.GetAllocator();

                document.AddMember("fid", rapidjson::Value(fid.c_str(), allocator), allocator);
                document.AddMember("refToken", rapidjson::Value(refToken.c_str(), allocator), allocator);

                RegisterDevice(tkn, android_Id, security_token, fid, tkn);

                document.AddMember("android_id", rapidjson::Value(std::to_string(android_Id).c_str(), allocator), allocator);
                document.AddMember("security_token", rapidjson::Value(std::to_string(security_token).c_str(), allocator), allocator);

                ::rapidjson::StringBuffer buffer;
                ::rapidjson::Writer<::rapidjson::StringBuffer> writer(buffer);
                document.Accept(writer);

                std::ofstream out(installation);
                out << buffer.GetString();
                out.close();
            }

            // RegisterDevice(tkn);
            MinimalInviteRtaClient::Config conf;
            conf.user = user;
            XalUserGetId(conf.user, &conf.xuid);
            conf.titleId = 0x67b57dac;

            static auto client = MinimalInviteRtaClient::Create(conf);
            client->RegisterFcmToken(tkn.data());
            ListenMTalk(android_Id, security_token);
        }).detach();
    }
    return hr;
}

struct defer {
    std::function<void()> d;
    defer(std::function<void()> && d) : d(d) {}
    void release() {
        d = []() {};
    }
    ~defer() {
        d();
    }
};

#define HRET(body) {\
HRESULT hr = body;\
if(FAILED(hr)) {\
    return hr;\
}\
}\

#include <mcs.pb.h>

HRESULT CreateInstallationToken(std::string& fid, std::string& tkn, std::string& refToken) {
    HCCallHandle handle;
    HRET(HCHttpCallCreate(&handle));
    defer __close([&]() {
        HCHttpCallCloseHandle(handle);
    });
    HRET(HCHttpCallSetTracing(handle, true));
    HRET(HCHttpCallRequestSetUrl(handle, "POST", "https://firebaseinstallations.googleapis.com/v1/projects/minecraft-bedrock-57580/installations"));
    HRET(HCHttpCallRequestSetHeader(handle, "Content-Type", "application/json", true));
    HRET(HCHttpCallRequestSetHeader(handle, "X-Android-Package", "com.example.fcmlogger", true));
    HRET(HCHttpCallRequestSetHeader(handle, "X-Android-Cert", "f925e1ababb4bcf8c3a5d225927f0c90fa95af63", true));
    HRET(HCHttpCallRequestSetHeader(handle, "User-Agent", "Android-GCM/1.5 (OnePlus7T QKQ1.190716.003)", true));
    HRET(HCHttpCallRequestSetHeader(handle, "x-goog-api-key", "AIzaSyC-QelK5mHbkAkFa-4s_enjDuIGAtNXyB4", false));
    HRET(HCHttpCallRequestSetRequestBodyString(handle, R"({
         "fid": "dtDFQmePHh2xIpEo5i-t-o",
         "appId": "1:486187589451:android:b2331110821fe2304bd2ce",
         "authVersion": "FIS_v2",
         "sdkVersion": "a:17.2.0"
    })"));
    XAsyncBlock block{};
    HRET(HCHttpCallPerformAsync(handle, &block));
    HRET(XAsyncGetStatus(&block, true));
    uint32_t statusCode;
    HRET(HCHttpCallResponseGetStatusCode(handle, &statusCode));
    const char *responseString;
    HRET(HCHttpCallResponseGetResponseString(handle, &responseString));
    // "{\n  \"name\": \"projects/486187589451/installations/dqBC1a1Jr82x8tUAbRayKI\",\n  \"fid\": \"dqBC1a1Jr82x8tUAbRayKI\",\n  \"refreshToken\": \"3_AS3qfwKaq0vFEBcx2nQrsuKwF19uV-SybJg1fGTUFHwRrYZVPam9MkKGF-9loKokdI4raILBjrCfKk2H5xccy5JMHMgkS2YQJXkQath86t10J7A\",\n  \"authToken\": {\n    \"token\": \"eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJhcHBJZCI6IjE6NDg2MTg3NTg5NDUxOmFuZHJvaWQ6YjIzMzExMTA4MjFmZTIzMDRiZDJjZSIsImV4cCI6MTc4MTI5NjYxOSwiZmlkIjoiZHFCQzFhMUpyODJ4OHRVQWJSYXlLSSIsInByb2plY3ROdW1iZXIiOjQ4NjE4NzU4OTQ1MX0.AB2LPV8wRQIhANkyQ3_wbCyOashioTm5wUjHuL3inXfdxuRVM3iV-mBjAiAzkqXw347Sp1pcl-m5zlhaWjUqIsF5CIou4kDgrQssNg\",\n    \"expiresIn\": \"604800s\"\n  }\n}\n"
    rapidjson::Document json;
    json.Parse(responseString, strlen(responseString));
    if (statusCode != 200 || json.HasParseError())
    {
        return E_FAIL;
    }
    fid = json["fid"].GetString();
    tkn = json["authToken"]["token"].GetString();
    refToken = json["refreshToken"].GetString();
    return S_OK;
}

HRESULT GetAuthToken(std::string fid, std::string refreshToken, std::string& tkn) {
    HCCallHandle handle;
    HRET(HCHttpCallCreate(&handle));
    defer __close([&]() {
        HCHttpCallCloseHandle(handle);
    });
    HRET(HCHttpCallSetTracing(handle, true));
    HRET(HCHttpCallRequestSetUrl(handle, "POST", ("https://firebaseinstallations.googleapis.com/v1/projects/minecraft-bedrock-57580/installations/" + fid + "/authTokens:generate").data()));
    HRET(HCHttpCallRequestSetHeader(handle, "Content-Type", "application/json", true));
    HRET(HCHttpCallRequestSetHeader(handle, "User-Agent", "Android-GCM/1.5 (OnePlus7T QKQ1.190716.003)", true));
    HRET(HCHttpCallRequestSetHeader(handle, "x-goog-api-key", "AIzaSyC-QelK5mHbkAkFa-4s_enjDuIGAtNXyB4", false));
    HRET(HCHttpCallRequestSetHeader(handle, "Authorization", ("FIS_v2 " + refreshToken).data(), false));
    XAsyncBlock block{};
    HRET(HCHttpCallPerformAsync(handle, &block));
    HRET(XAsyncGetStatus(&block, true));
    uint32_t statusCode;
    HRET(HCHttpCallResponseGetStatusCode(handle, &statusCode));
    const char *responseString;
    HRET(HCHttpCallResponseGetResponseString(handle, &responseString));
    rapidjson::Document json;
    json.Parse(responseString, strlen(responseString));
    if (statusCode != 200 || json.HasParseError())
    {
        return E_FAIL;
    }
    tkn = json["token"].GetString();
    return S_OK;
}

HRESULT DeleteInstallationToken(std::string fid, std::string refreshToken) {
    HCCallHandle handle;
    HRET(HCHttpCallCreate(&handle));
    defer __close([&]() {
        HCHttpCallCloseHandle(handle);
    });
    HRET(HCHttpCallSetTracing(handle, true));
    HRET(HCHttpCallRequestSetUrl(handle, "DELETE", ("https://firebaseinstallations.googleapis.com/v1/projects/minecraft-bedrock-57580/installations/" + fid).data()));
    HRET(HCHttpCallRequestSetHeader(handle, "Content-Type", "application/json", true));
    HRET(HCHttpCallRequestSetHeader(handle, "User-Agent", "Android-GCM/1.5 (OnePlus7T QKQ1.190716.003)", true));
    HRET(HCHttpCallRequestSetHeader(handle, "x-goog-api-key", "AIzaSyC-QelK5mHbkAkFa-4s_enjDuIGAtNXyB4", false));
    HRET(HCHttpCallRequestSetHeader(handle, "Authorization", ("FIS_v2 " + refreshToken).data(), false));
    XAsyncBlock block{};
    HRET(HCHttpCallPerformAsync(handle, &block));
    HRET(XAsyncGetStatus(&block, true));
    uint32_t statusCode;
    HRET(HCHttpCallResponseGetStatusCode(handle, &statusCode));
    const char *responseString;
    HRET(HCHttpCallResponseGetResponseString(handle, &responseString));
    return S_OK;
}

HRESULT RegisterDevice(std::string& tkn, long androidId, long securityToken, std::string fid, std::string installToken) {
    HCCallHandle handle;
    HRET(HCHttpCallCreate(&handle));
    defer __close([&]() {
        HCHttpCallCloseHandle(handle);
    });
    HRET(HCHttpCallSetTracing(handle, true));
    HRET(HCHttpCallRequestSetUrl(handle, "POST", "https://android.clients.google.com/c2dm/register3"));
    HRET(HCHttpCallRequestSetHeader(handle, "Content-Type", "application/x-www-form-urlencoded", true));
    HRET(HCHttpCallRequestSetHeader(handle, "User-Agent", "Android-GCM/1.5 (OnePlus7T QKQ1.190716.003)", true));
    HRET(HCHttpCallRequestSetHeader(handle, "Authorization", ("AidLogin " + std::to_string(androidId) + ":" + std::to_string(securityToken)).data(), false));
    HRET(HCHttpCallRequestSetRequestBodyString(handle, ("X-Firebase-Client=kotlin%2F1.4.10+fire-analytics%2F19.0.0+android-target-sdk%2F30+android-min-sdk%2F24+fire-core%2F20.0.0+device-name%2FOnePlus7T+device-model%2FOnePlus7T+fire-android%2F29+fire-iid%2F21.0.1+android-installer%2Fcom.android.vending+device-brand%2FOnePlus+fire-installations%2F17.0.0+android-platform%2F+fire-fcm%2F20.1.7_1p&X-Firebase-Client-Log-Type=1&X-Goog-Firebase-Installations-Auth=" + installToken + "&X-app_ver=10000&X-app_ver_name=1.0.0&X-appid=" + fid + "&X-cliv=fcm-23.1.2&X-firebase-app-name-hash=R1dAH9Ui7M-ynoznwBdw01tLxhI&X-gmp_app_id=1%3A486187589451%3Aandroid%3Ab2331110821fe2304bd2ce&X-gmsv=241718022&X-osv=29&X-scope=%2A&X-subtype=486187589451&app=com.example.fcmlogger&app_ver=10000&cert=f7918b406cae8782f94650240bd075fc3c76729e&device=" + std::to_string(androidId) +  "&gcm_ver=241718022&plat=0&sender=486187589451&target_ver=29").data()));
    XAsyncBlock block{};
    HRET(HCHttpCallPerformAsync(handle, &block));
    HRET(XAsyncGetStatus(&block, true));
    uint32_t statusCode;
    HRET(HCHttpCallResponseGetStatusCode(handle, &statusCode));
    const char *responseString;
    HRET(HCHttpCallResponseGetResponseString(handle, &responseString));
    
    if(statusCode == 200) {
        tkn = responseString + sizeof("TOKEN");
    }
    return S_OK;
}

#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>

HRESULT SendVarInt(SSL* ssl, int value) {
    if(value < 0) {
        return E_FAIL;
    }
    while(true) {
        if ((value & -128) == 0) {
            char t = (char)value;
            if(SSL_write(ssl, &t, sizeof(t)) != sizeof(t)) {
                return E_FAIL;
            }
            return S_OK;
        } else {
            char t = (char)((value & 0x7f) | 0x80);
            if(SSL_write(ssl, &t, sizeof(t)) != sizeof(t)) {
                return E_FAIL;
            }
            value >>= 7;
        }
    }
}

HRESULT RecvVarInt(SSL* ssl, int& value) {
    value = 0;
    for(int i = 0; ; i++) {
        char t = 0;
        if(SSL_read(ssl, &t, sizeof(t)) != sizeof(t)) {
            return E_FAIL;
        }
        value |= (t & 0x7f) << i * 7;
        if((t & 0x80) == 0) {
            return S_OK;
        }
    }
}

HRESULT SendMsg(SSL* ssl, int tag, const google::protobuf::MessageLite& msg) {
    char t = (char)tag;
    if(SSL_write(ssl, &t, sizeof(t)) != sizeof(t)) {
        return E_FAIL;
    }
    std::string body;
    if(!msg.SerializeToString(&body)) {
        return E_FAIL;
    }
    if(auto hr = SendVarInt(ssl, (int)body.size()); FAILED(hr)) {
        return hr;
    }
    if(SSL_write(ssl, body.data(), (int)body.size()) != (int)body.size()) {
        return E_FAIL;
    }
    return S_OK;
}

int SSL_readfull(SSL *ssl, void *buf, int num) {
    int read = 0;
    while(read < num) {
        int r = SSL_read(ssl, (char*)buf + read, num - read);
        if(r <= 0) {
            return r;
        }
        read += r;
    }
    return read;
}

HRESULT RecvMsg(SSL* ssl,  google::protobuf::MessageLite& msg) {
    int bodylen = 0;
    if(auto hr = RecvVarInt(ssl, bodylen); FAILED(hr)) {
        return hr;
    }
    std::string body;
    body.resize(bodylen);
    if(SSL_readfull(ssl, body.data(), (int)body.size()) != (int)body.size()) {
        return E_FAIL;
    }
    msg.ParseFromString(body);
    return S_OK;
}

HRESULT ListenMTalk(long androidId, long securityToken) {
    struct addrinfo hints, *result, *ptr;
	memset(&hints, 0, sizeof(addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

	if(getaddrinfo("mtalk.google.com", "5228", &hints, &result) != 0) {
        return E_FAIL;
    }
    int fd = -1;
    for(ptr=result; ptr != NULL ;ptr=ptr->ai_next) {
        int sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if(sock != -1) {
            if(connect(sock, ptr->ai_addr, (socklen_t)ptr->ai_addrlen) != -1) {
                fd = sock;
                break;
            }
        }
    }
    freeaddrinfo(result);
    if(fd == -1) {
        return E_FAIL;
    }

    auto ssl_ctx = SSL_CTX_new(TLS_client_method());
    auto ssl = SSL_new(ssl_ctx);
    SSL_set_fd(ssl, fd);

    int ret = SSL_connect(ssl);
    if(ret != 1) {
        int error = SSL_get_error(ssl, ret);
        SSL_free(ssl);
        return E_FAIL;
    }

    char version = 41;
    int written = SSL_write(ssl, &version, sizeof(version));
    if(written != sizeof(version)) {
        int error = SSL_get_error(ssl, written);
        SSL_free(ssl);
        return E_FAIL;
    }
    version = -1;
    int read = SSL_read(ssl, &version, sizeof(version));
    if(read != sizeof(version)) {
        int error = SSL_get_error(ssl, read);
        SSL_free(ssl);
        return E_FAIL;
    }

    auto req = mcs_proto::LoginRequest{};
    req.set_id("gms-22.48.14-000");
    req.set_domain("mcs.android.com");
    req.set_user(std::to_string(androidId));
    req.set_resource(std::to_string(androidId));
    req.set_auth_token(std::to_string(securityToken));
    req.set_device_id((std::stringstream() << std::hex << "android-" << androidId).str());
    auto s = req.add_setting();
    s->set_name("new_vc");
    s->set_value("1");
    s = req.add_setting();
    s->set_name("os_ver");
    s->set_value("android-14");
    s = req.add_setting();
    s->set_name("ERR");
    s->set_value("20");
    s = req.add_setting();
    s->set_name("CT");
    s->set_value("8");
    s = req.add_setting();
    s->set_name("CONOK");
    s->set_value("3");
    s = req.add_setting();
    s->set_name("u:f");
    s->set_value("0");
    s = req.add_setting();
    s->set_name("networkOn");
    s->set_value("0");
    // Do not have one on initial login
    req.add_received_persistent_id("");
    req.set_adaptive_heartbeat(false);
    req.set_use_rmq2(true);
    req.set_auth_service(mcs_proto::LoginRequest_AuthService_ANDROID_ID);
    req.set_network_type(1);
    SendMsg(ssl, 2, req);

    while(true) {
        int tag;
        RecvVarInt(ssl, tag);
        switch (tag)
        {
        case 0:
            {
                mcs_proto::HeartbeatPing msg;
                RecvMsg(ssl, msg);
                mcs_proto::HeartbeatAck ack;
                ack.set_status(msg.status());
                SendMsg(ssl, 1, ack);
            }
            break;
        case 1:
            {
                mcs_proto::HeartbeatAck msg;
                RecvMsg(ssl, msg);
            }
            break;
        case 3:
            {
                mcs_proto::LoginResponse msg;
                RecvMsg(ssl, msg);
            }
            break;
        case 4:
            {
                mcs_proto::Close msg;
                RecvMsg(ssl, msg);
            }
            break;
        case 7:
            {
                mcs_proto::IqStanza msg;
                RecvMsg(ssl, msg);
            }
            break;
        case 8:
            {
                mcs_proto::DataMessageStanza msg;
                RecvMsg(ssl, msg);
                for(int i = 0; i < msg.app_data_size(); i++) {
                    std::string kv = msg.app_data(i).key() + "=" + msg.app_data(i).value();
                    std::cout << kv << "\n";
                    if(msg.app_data(i).key() == "xbl") {
                        rapidjson::Document doc;
                        doc.Parse(msg.app_data(i).value().data());
                        JNIEnv* env;
                        _vm->GetEnv((void**)&env, JNI_VERSION_1_6);
                        Java_com_mojang_minecraftpe_NotificationListenerService_nativePushNotificationReceived(env, nullptr, 1, env->NewStringUTF((std::string("Invited by ") + doc["invite_handle"]["inviteInfo"]["sender"].GetString()).data()), env->NewStringUTF("You have been invited"), env->NewStringUTF(msg.app_data(i).value().c_str()));
                    }
                }
            }
            break;
        
        default:
            break;
        }
    }

    SSL_free(ssl);

    return S_OK;
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

#undef HRET
#define HRET(x) x

// Return true on success, false on failure
bool zlib_inflate(const std::vector<uint8_t>& compressed, std::string& out) {
    z_stream zs{};
    zs.next_in = const_cast<Bytef*>(compressed.data());
    zs.avail_in = compressed.size();

    if (inflateInit2(&zs, 15 + 32) != Z_OK) {
        IF_DEBUG(std::cout << "inflateInit2 failed" << std::endl);
        return false;
    }

    char buffer[32768]; // 32 KB chunks
    int ret;
    out.clear();

    do {
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        zs.avail_out = sizeof(buffer);

        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            IF_DEBUG(std::cout << "inflate failed with error code: " << ret << std::endl);
            return false;
        }

        // Append decompressed chunk
        out.append(buffer, sizeof(buffer) - zs.avail_out);
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return true;
}

playapi::http_response playapi::http_request::perform() {
    std::string method;
    switch (this->method)
    {
    case playapi::http_method::GET:
        method = (std::string)cryptstring("GET");
        break;
    case playapi::http_method::POST:
        method = (std::string)cryptstring("POST");
        break;
    case playapi::http_method::PUT:
        method = (std::string)cryptstring("PUT");
        break;
    default:
        method = (std::string)cryptstring("GET");
        break;
    }
    HCCallHandle handle;
    HRET(HCHttpCallCreate(&handle));
    defer __close([&]() {
        HCHttpCallCloseHandle(handle);
    });
    HRET(HCHttpCallSetTracing(handle, true));
    HRET(HCHttpCallRequestSetUrl(handle, method.data(), this->url.c_str()));
    if(body.size() > 0) {
        HCHttpCallRequestSetRequestBodyBytes(handle, (const uint8_t *)body.data(), body.size());
    }
    if(!this->user_agent.empty()) {
        HRET(HCHttpCallRequestSetHeader(handle, "User-Agent", this->user_agent.data(), true));
    }
    for (auto&& h : headers)
    {
        HRET(HCHttpCallRequestSetHeader(handle, h.first.c_str(), h.second.c_str(), true));
    }
    XAsyncBlock block{};
    HRET(HCHttpCallPerformAsync(handle, &block));
    HRET(XAsyncGetStatus(&block, true));
    uint32_t statusCode;
    HRET(HCHttpCallResponseGetStatusCode(handle, &statusCode));
    const char *responseString;
    size_t len;
    HCHttpCallResponseGetResponseBodyBytesSize(handle, &len);
    std::string responseStr;
    std::vector<uint8_t> compressedResponse(len);
    HCHttpCallResponseGetResponseBodyBytes(handle, len, (uint8_t*)compressedResponse.data(), NULL);

    if (!zlib_inflate(compressedResponse, responseStr)) {
        IF_DEBUG(std::cout << "Failed to decompress response" << std::endl);
        return playapi::http_response(false, statusCode, "");
    }

    return playapi::http_response(true, statusCode, responseStr);
}
