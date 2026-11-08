#include "steamcore.h"

#include <dlfcn.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sc {

void *client;
int32_t pipe, user, gsPipe, gsUser;
JavaVM *jvm;

static void *lib;
static bool (*pBGetCallback)(int32_t, void *, int32_t *);
static void (*pFreeLastCallback)(int32_t);
static bool (*pGetAPICallResult)(int32_t, uint64_t, void *, int, int, bool *);
static bool (*pIsKnownInterface)(const char *);

static std::recursive_mutex mu;
static std::map<std::string, void *> cache;

struct CB { int id; bool gs; Handler h; };
static std::vector<CB> callbacks;
struct CR { int id; bool gs; ResultHandler h; };
static std::map<uint64_t, CR> results;

static int logLevel = 1;

void log(int level, const char *fmt, ...) {
    if (level < logLevel) return;
    va_list ap; va_start(ap, fmt);
    fputs("[znet-arm64] ", stderr); vfprintf(stderr, fmt, ap);
    va_end(ap);
}

JNIEnv *env() {
    if (!jvm) return nullptr;
    JNIEnv *e = nullptr;
    if (jvm->GetEnv((void **)&e, JNI_VERSION_1_6) == JNI_OK) return e;
    jvm->AttachCurrentThread((void **)&e, nullptr);
    return e;
}

bool loadClient(char *err, size_t n) {
    std::lock_guard<std::recursive_mutex> g(mu);
    if (client) return true;
    std::string path;
    if (const char *p = getenv("PZ_STEAMCLIENT")) path = p;
    else path = std::string(getenv("HOME") ? getenv("HOME") : "") + "/.local/share/Steam/linuxarm64/steamclient.so";
    lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { snprintf(err, n, "dlopen %s: %s", path.c_str(), dlerror()); return false; }
    auto ci = (void *(*)(const char *, int *))dlsym(lib, "CreateInterface");
    pBGetCallback = (decltype(pBGetCallback))dlsym(lib, "Steam_BGetCallback");
    pFreeLastCallback = (decltype(pFreeLastCallback))dlsym(lib, "Steam_FreeLastCallback");
    pGetAPICallResult = (decltype(pGetAPICallResult))dlsym(lib, "Steam_GetAPICallResult");
    pIsKnownInterface = (decltype(pIsKnownInterface))dlsym(lib, "Steam_IsKnownInterface");
    if (!ci || !pBGetCallback || !pFreeLastCallback || !pGetAPICallResult) {
        snprintf(err, n, "steamclient.so is missing expected exports"); return false;
    }
    client = ci("SteamClient021", nullptr);
    if (!client) { snprintf(err, n, "No SteamClient021"); return false; }
    return true;
}

int initClient(char *err, size_t n) {
    std::lock_guard<std::recursive_mutex> g(mu);
    if (pipe) return 0;
    if (!getenv("SteamAppId")) setenv("SteamAppId", "108600", 1);
    if (!getenv("SteamGameId")) setenv("SteamGameId", "108600", 1);
    if (!loadClient(err, n)) return 1;
    pipe = vcall<int32_t>(client, 0);  // CreateSteamPipe
    if (!pipe) { snprintf(err, n, "Cannot create IPC pipe to Steam client process.  Steam is probably not running."); return 2; }
    user = vcall<int32_t, int32_t>(client, 2, pipe);  // ConnectToGlobalUser
    if (!user) {
        snprintf(err, n, "ConnectToGlobalUser failed.");
        vcall<bool, int32_t>(client, 1, pipe); pipe = 0; return 1;
    }
    for (const char *v : {"SteamUtils010", "SteamUser023", "SteamFriends018", "STEAMUGC_INTERFACE_VERSION021",
                          "SteamMatchMakingServers002", "SteamNetworking006", "STEAMREMOTEPLAY_INTERFACE_VERSION003"}) {
        if (pIsKnownInterface && !pIsKnownInterface(v)) { snprintf(err, n, "No %s", v); shutdownClient(); return 3; }
    }
    void *utils = ifaceUser0("SteamUtils010");
    if (!utils || !iface("SteamUser023")) { snprintf(err, n, "No SteamUtils010/SteamUser023"); shutdownClient(); return 3; }
    uint32_t app = vcall<uint32_t>(utils, 9);  // GetAppID
    if (!app) { snprintf(err, n, "No appID found."); shutdownClient(); return 1; }
    vcall<void, void *>(client, 32, nullptr);  // Set_SteamAPI_CCheckCallbackRegisteredInProcess(NULL), as manual dispatch does
    log(1, "steam client ready: pipe=%d user=%d app=%u\n", pipe, user, app);
    return 0;
}

void shutdownClient() {
    std::lock_guard<std::recursive_mutex> g(mu);
    if (!client || !pipe) return;
    if (user) vcall<void, int32_t, int32_t>(client, 4, pipe, user);  // ReleaseUser
    vcall<bool, int32_t>(client, 1, pipe);                           // BReleaseSteamPipe
    for (auto it = cache.begin(); it != cache.end();)
        it = (it->first[0] == 'c' || it->first[0] == 'C') ? cache.erase(it) : std::next(it);
    pipe = user = 0;
}

#pragma pack(push, 4)
struct SteamIPAddress { uint8_t ip[16]; int32_t type; };
#pragma pack(pop)

int initGameServer(uint32_t ip, uint16_t gamePort, uint16_t queryPort, int mode, const char *ver, char *err, size_t n) {
    std::lock_guard<std::recursive_mutex> g(mu);
    if (!getenv("SteamAppId")) setenv("SteamAppId", "108600", 1);
    if (!getenv("SteamGameId")) setenv("SteamGameId", "108600", 1);
    if (!loadClient(err, n)) return 1;
    SteamIPAddress bind{}; memcpy(bind.ip, &ip, 4); bind.type = 0;
    vcall<void, const SteamIPAddress *, uint16_t>(client, 7, &bind, 0);   // SetLocalIPBinding
    int32_t p = 0;
    int32_t u = vcall<int32_t, int32_t *, int>(client, 3, &p, 3);         // CreateLocalUser(&pipe, GameServer)
    if (!u || !p) { snprintf(err, n, "CreateLocalUser failed"); return 1; }
    gsPipe = p; gsUser = u;
    void *gs = gsIface("SteamGameServer015");
    void *utils = gsIfaceUser0("SteamUtils010");
    if (!gs || !utils) { snprintf(err, n, "No SteamGameServer015/SteamUtils010"); shutdownGameServer(); return 3; }
    uint32_t flags = mode == 3 ? 2 : mode == 1 ? 0x20 : 0;
    uint32_t app = vcall<uint32_t>(utils, 9);
    if (!app) { snprintf(err, n, "Can't determine AppID"); shutdownGameServer(); return 1; }
    if (!vcall<bool, uint32_t, uint16_t, uint16_t, uint32_t, uint32_t, const char *>(gs, 0, ip, gamePort, queryPort, flags, app, ver)) {
        snprintf(err, n, "InitGameServer failed"); shutdownGameServer(); return 1;
    }
    log(1, "game server ready: pipe=%d user=%d app=%u\n", gsPipe, gsUser, app);
    return 0;
}

void shutdownGameServer() {
    std::lock_guard<std::recursive_mutex> g(mu);
    if (!client || !gsPipe) return;
    void *gs = gsIface("SteamGameServer015");
    if (gs && vcall<bool>(gs, 8)) vcall<void>(gs, 7);   // BLoggedOn -> LogOff
    vcall<void, int32_t, int32_t>(client, 4, gsPipe, gsUser);
    vcall<bool, int32_t>(client, 1, gsPipe);
    for (auto it = cache.begin(); it != cache.end();)
        it = (it->first[0] == 'g' || it->first[0] == 'G') ? cache.erase(it) : std::next(it);
    gsPipe = gsUser = 0;
}

static void *generic(char kind, int32_t u, int32_t p, const char *ver) {
    if (!client || !p) return nullptr;
    std::lock_guard<std::recursive_mutex> g(mu);
    std::string key = std::string(1, kind) + ":" + ver;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    void *i = vcall<void *, int32_t, int32_t, const char *>(client, 12, u, p, ver);  // GetISteamGenericInterface
    if (i) cache[key] = i;
    return i;
}
void *iface(const char *ver) { return generic('c', user, pipe, ver); }
void *ifaceUser0(const char *ver) { return generic('C', 0, pipe, ver); }
void *gsIface(const char *ver) { return generic('g', gsUser, gsPipe, ver); }
void *gsIfaceUser0(const char *ver) { return generic('G', 0, gsPipe, ver); }

void registerCallback(int id, bool gs, Handler h) {
    std::lock_guard<std::recursive_mutex> g(mu);
    callbacks.push_back({id, gs, std::move(h)});
}
void bindCallResult(uint64_t call, int id, bool gs, ResultHandler h) {
    if (!call) return;
    std::lock_guard<std::recursive_mutex> g(mu);
    results[call] = {id, gs, std::move(h)};
}
void cancelCallResult(uint64_t call) {
    std::lock_guard<std::recursive_mutex> g(mu);
    results.erase(call);
}

struct CallbackMsg { int32_t user; int32_t id; uint8_t *param; int32_t size; };
#pragma pack(push, 4)
struct APICallCompleted { uint64_t call; int32_t id; uint32_t size; };  // 703
#pragma pack(pop)

static void dispatch(int32_t p, bool gs) {
    if (!p) return;
    CallbackMsg msg;
    int32_t hcall = 0;
    while (pBGetCallback(p, &msg, &hcall)) {
        if (msg.id == 703) {
            auto *c = (APICallCompleted *)msg.param;
            std::vector<uint8_t> buf(c->size ? c->size : 1);
            bool failed = false;
            ResultHandler h; bool found = false;
            {
                std::lock_guard<std::recursive_mutex> g(mu);
                auto it = results.find(c->call);
                if (it != results.end() && it->second.gs == gs) { h = std::move(it->second.h); results.erase(it); found = true; }
            }
            if (found && pGetAPICallResult(p, c->call, buf.data(), (int)c->size, c->id, &failed)) h(buf.data(), failed);
        } else {
            std::vector<Handler> hs;
            {
                std::lock_guard<std::recursive_mutex> g(mu);
                for (auto &cb : callbacks) if (cb.id == msg.id && cb.gs == gs) hs.push_back(cb.h);
            }
            for (auto &h : hs) h(msg.param);
        }
        pFreeLastCallback(p);
    }
}

void runFrameClient() {
    if (!pipe) return;
    if (void *u = ifaceUser0("SteamUtils010")) vcall<void>(u, 14);  // RunFrame
    dispatch(pipe, false);
}
void runFrameServer() { dispatch(gsPipe, true); }

}  // namespace sc
