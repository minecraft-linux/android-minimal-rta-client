# Minimal Invite RTA Client

Small Android NDK shared library that exposes a `minimal_invite_rta::MinimalInviteRtaClient` built on:

- `Xal` headers
- `libHttpClient` websocket APIs
- `libminecraftpe.so` for the `Xal*` symbols
- `libHttpClient.Android.so` for the `HCWebSocket*` symbols

It does not use `XUser` or any `XGameRuntime` / `XGameruntime` code.

## Layout

- `include/minimal_invite_rta/MinimalInviteRtaClient.h`
- `src/MinimalInviteRtaClient.cpp`
- `CMakeLists.txt`

## Build assumptions

You still need these pieces from the host app:

- An initialized `XalUserHandle`
- A live `XTaskQueueHandle`
- `HCInitialize(...)` / `XalInitialize(...)` already handled by the host

This library intentionally does not create its own task queue, because the shipped Android binaries here do not export `XTaskQueueCreate` / `XTaskQueueDispatch`. The caller must pass a queue that is already being dispatched.

## Default local paths

The `CMakeLists.txt` defaults to the paths currently present on this machine:

- Android XAL compat includes: `.../xbox-live-api/External/Xal/Source/Xal/Include`
  - Used only as a fallback for Android-specific XAL headers such as `Xal/xal_android.h` when the selected GDK snapshot is incomplete.
- Android libHttpClient compat includes: `.../xbox-live-api/External/Xal/External/libHttpClient/Include`
  - Used only as a fallback for `httpClient/async_jvm.h` when the selected GDK snapshot does not ship it.
- rapidjson includes: `.../xbox-live-api/External/rapidjson/include`
- Android libs: `.../1.26.30.31/lib/arm64-v8a`

Override any of them with `-D...=...` at configure time.

## Example configure

```bash
cmake -S android-minimal-invite-rta -B build-android-rta \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
```

## Usage sketch

```cpp
using minimal_invite_rta::MinimalInviteRtaClient;

MinimalInviteRtaClient::Config config{};
config.user = xalUser;
config.queue = taskQueue;
config.titleId = titleId;
config.xuid = xuid; // optional, 0 means call XalUserGetId

auto client = MinimalInviteRtaClient::Create(config);
client->RegisterFcmToken(nullptr); // registers the RTA resource URI using Win32-style payload semantics
client->SetStateHandler([](bool connected) {
    // observe websocket state
});
client->SetInviteHandler([](const minimal_invite_rta::InviteEvent& invite) {
    // consume invite
});
client->Connect();
```

The C ABI exposes the same registration flow through `MinimalInviteRtaRegisterFcmToken(handle, nullptr)`.
