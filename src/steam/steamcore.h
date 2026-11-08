// Minimal Steamworks client layer for the arm64 libZNetJNI64 port.
// Talks to Valve's native arm64 steamclient.so directly (no libsteam_api):
// interfaces are raw vtable pointers, slots are taken from the x86 lib's call
// sites, and callbacks are dispatched manually.
#pragma once
#include <jni.h>
#include <cstdint>
#include <functional>

namespace sc {

// Call vtable slot `slot` on interface `o`.
template <typename R, typename... A>
static inline R vcall(void *o, int slot, A... a) {
    return ((R(*)(void *, A...))(*(void ***)o)[slot])(o, a...);
}

extern void *client;              // ISteamClient (SteamClient021)
extern int32_t pipe, user;        // client pipe/user (0 if not initialised)
extern int32_t gsPipe, gsUser;    // game-server pipe/user (0 if not initialised)
extern JavaVM *jvm;

bool loadClient(char *err, size_t errLen);           // dlopen + CreateInterface
int initClient(char *err, size_t errLen);            // 0 ok, 1 fail, 2 no steam, 3 version
void shutdownClient();
// Game server (SteamInternal_GameServer_Init_V2 equivalent). ip host order (0 = any).
int initGameServer(uint32_t ip, uint16_t gamePort, uint16_t queryPort, int mode, const char *version, char *err, size_t errLen);
void shutdownGameServer();

// Interface from the client user (or hUser 0 for utils), cached per version.
void *iface(const char *ver);
void *ifaceUser0(const char *ver);                    // GetISteamGenericInterface(0, pipe, ver)
void *gsIface(const char *ver);                       // game-server pipe/user
void *gsIfaceUser0(const char *ver);

// Callbacks: handler gets the raw callback struct.
using Handler = std::function<void(void *data)>;
void registerCallback(int id, bool gameServer, Handler h);
// Call results: handler gets (data, ioFailure). Replaces any earlier binding of `call`.
using ResultHandler = std::function<void(void *data, bool ioFailure)>;
void bindCallResult(uint64_t call, int expectedId, bool gameServer, ResultHandler h);
void cancelCallResult(uint64_t call);

void runFrameClient();     // ISteamUtils::RunFrame + dispatch on the client pipe
void runFrameServer();     // dispatch on the game-server pipe

JNIEnv *env();             // attach current thread if needed
void log(int level, const char *fmt, ...);
}  // namespace sc
