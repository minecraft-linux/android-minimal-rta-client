#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <httpClient/async.h>
#include <Xal/xal.h>

#if defined(_WIN32)
#define MINIMAL_INVITE_RTA_EXPORT extern "C" __declspec(dllexport)
#else
#define MINIMAL_INVITE_RTA_EXPORT extern "C" __attribute__((visibility("default")))
#endif

typedef struct MinimalInviteRtaClientHandle MinimalInviteRtaClientHandle;

typedef struct MinimalInviteRtaSessionReference
{
    const char* scid;
    const char* templateName;
    const char* sessionName;
} MinimalInviteRtaSessionReference;

typedef struct MinimalInviteRtaInviteEvent
{
    uint64_t invitedXboxUserId;
    uint64_t senderXboxUserId;

    const char* senderGamertag;
    const char* senderModernGamertag;
    const char* senderModernGamertagSuffix;
    const char* senderUniqueModernGamertag;
    const char* senderImageUrl;

    const char* inviteHandle;
    const char* inviteHandleId;
    const char* inviteProtocol;
    const char* inviteContext;

    int64_t expiration;
    MinimalInviteRtaSessionReference sessionReference;
} MinimalInviteRtaInviteEvent;

typedef struct MinimalInviteRtaConfig
{
    XalUserHandle user;
    XTaskQueueHandle queue;
    uint64_t xuid;
    uint32_t titleId;
    const char* locale;
    const char* userAgent;
    bool autoReconnect;
} MinimalInviteRtaConfig;

typedef void (*MinimalInviteRtaInviteCallback)(
    void* context,
    const MinimalInviteRtaInviteEvent* invite);

typedef void (*MinimalInviteRtaStateCallback)(
    void* context,
    bool connected);

MINIMAL_INVITE_RTA_EXPORT uint32_t MinimalInviteRtaGetAbiVersion(void);

MINIMAL_INVITE_RTA_EXPORT MinimalInviteRtaClientHandle* MinimalInviteRtaCreate(
    const MinimalInviteRtaConfig* config);

MINIMAL_INVITE_RTA_EXPORT void MinimalInviteRtaDestroy(
    MinimalInviteRtaClientHandle* handle);

MINIMAL_INVITE_RTA_EXPORT void MinimalInviteRtaSetInviteCallback(
    MinimalInviteRtaClientHandle* handle,
    MinimalInviteRtaInviteCallback callback,
    void* context);

MINIMAL_INVITE_RTA_EXPORT void MinimalInviteRtaSetStateCallback(
    MinimalInviteRtaClientHandle* handle,
    MinimalInviteRtaStateCallback callback,
    void* context);

MINIMAL_INVITE_RTA_EXPORT void MinimalInviteRtaSetAutoReconnect(
    MinimalInviteRtaClientHandle* handle,
    bool enabled);

MINIMAL_INVITE_RTA_EXPORT HRESULT MinimalInviteRtaConnect(
    MinimalInviteRtaClientHandle* handle);

MINIMAL_INVITE_RTA_EXPORT HRESULT MinimalInviteRtaDisconnect(
    MinimalInviteRtaClientHandle* handle);

MINIMAL_INVITE_RTA_EXPORT HRESULT MinimalInviteRtaRegisterFcmToken(
    MinimalInviteRtaClientHandle* handle,
    const char* fcmToken);

MINIMAL_INVITE_RTA_EXPORT const char* MinimalInviteRtaGetResourceUri(
    MinimalInviteRtaClientHandle* handle);
