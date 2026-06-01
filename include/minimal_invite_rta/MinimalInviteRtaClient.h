#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <ctime>

#include <httpClient/async.h>
#include <Xal/xal.h>

namespace minimal_invite_rta
{

struct SessionReference
{
    std::string scid;
    std::string templateName;
    std::string sessionName;
};

struct InviteEvent
{
    uint64_t invitedXboxUserId{};
    uint64_t senderXboxUserId{};

    std::string senderGamertag;
    std::string senderModernGamertag;
    std::string senderModernGamertagSuffix;
    std::string senderUniqueModernGamertag;
    std::string senderImageUrl;

    std::string inviteHandle;
    std::string inviteHandleId;
    std::string inviteProtocol;
    std::string inviteContext;

    time_t expiration{};
    SessionReference sessionReference;
};

class MinimalInviteRtaClient : public std::enable_shared_from_this<MinimalInviteRtaClient>
{
public:
    using InviteHandler = std::function<void(const InviteEvent&)>;
    using StateHandler = std::function<void(bool connected)>;

    struct Config
    {
        XalUserHandle user{};
        XTaskQueueHandle queue{};
        uint64_t xuid{};
        uint32_t titleId{};
        const char* locale{"en-US"};
        const char* userAgent{"MinimalInviteRtaClient/1.0"};
        bool autoReconnect{true};
    };

    static std::shared_ptr<MinimalInviteRtaClient> Create(const Config& config);

    ~MinimalInviteRtaClient();

    MinimalInviteRtaClient(const MinimalInviteRtaClient&) = delete;
    MinimalInviteRtaClient& operator=(const MinimalInviteRtaClient&) = delete;

    void SetInviteHandler(InviteHandler handler);
    void SetStateHandler(StateHandler handler);
    void SetAutoReconnect(bool enabled);

    HRESULT Connect();
    HRESULT Disconnect();
    HRESULT RegisterFcmToken(const char* fcmToken);

    std::string ResourceUri() const;

private:
    explicit MinimalInviteRtaClient(const Config& config);
    void SendSubscribe();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace minimal_invite_rta
