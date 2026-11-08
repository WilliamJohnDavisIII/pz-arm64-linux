// ServerBrowser / SteamFriends / SteamGameServer JNI.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "znet.h"

using sc::vcall;

static void *MMS() { return sc::iface("SteamMatchMakingServers002"); }
static void *Friends() { return sc::iface("SteamFriends018"); }
static void *CUtils() { return sc::ifaceUser0("SteamUtils010"); }
void *gsServer();  // networking.cpp

static void excCheck(JNIEnv *e) { if (e->ExceptionCheck()) { e->ExceptionDescribe(); e->ExceptionClear(); } }

// ================================================================== ServerBrowser
#pragma pack(push, 4)
struct gameserveritem_t {
    uint16_t connPort, queryPort; uint32_t ip; int ping; bool hadResponse, doNotRefresh;
    char gameDir[32], map[32], gameDescription[64];
    uint32_t appId; int players, maxPlayers, botPlayers; bool password, secure; uint32_t lastPlayed; int serverVersion;
    char serverName[64], gameTags[128]; uint64_t steamID;
};
#pragma pack(pop)
static_assert(sizeof(gameserveritem_t) == 0x174, "gameserveritem_t");
static_assert(offsetof(gameserveritem_t, steamID) == 0x16c, "steamID");
static_assert(offsetof(gameserveritem_t, serverName) == 0xac, "serverName");

struct MatchMakingKeyValuePair_t { char key[256]; char value[256]; };

struct ServerListResponse {  // ISteamMatchmakingServerListResponse
    virtual void ServerResponded(void *req, int i) = 0;
    virtual void ServerFailedToRespond(void *req, int i) = 0;
    virtual void RefreshComplete(void *req, int resp) = 0;
};
struct PingResp {             // ISteamMatchmakingPingResponse
    virtual void ServerResponded(gameserveritem_t &item) = 0;
    virtual void ServerFailedToRespond() = 0;
};
struct RulesResp {            // ISteamMatchmakingRulesResponse
    virtual void RulesResponded(const char *rule, const char *value) = 0;
    virtual void RulesFailedToRespond() = 0;
    virtual void RulesRefreshComplete() = 0;
};

static jclass sbClass;
static jmethodID sbResponded, sbFailed, sbRefreshDone, sbRespondedAddr, sbFailedAddr, sbRules;

static jobject makeDetails(JNIEnv *e, const gameserveritem_t &it) {
    jclass cls = e->FindClass("zombie/core/znet/GameServerDetails");
    if (!cls) return nullptr;
    jmethodID ctor = e->GetMethodID(cls, "<init>",
        "(Ljava/lang/String;IJLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IIIZI)V");
    char addr[20], name[64];
    uint32_t ip = it.ip;
    snprintf(addr, sizeof addr, "%d.%d.%d.%d", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    const char *nm = it.serverName;
    if (!nm[0]) { snprintf(name, sizeof name, "%u.%u.%u.%u:%i", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, it.connPort); nm = name; }
    return e->NewObject(cls, ctor, e->NewStringUTF(addr), (jint)it.queryPort, (jlong)it.steamID, e->NewStringUTF(nm),
                        e->NewStringUTF(it.gameDir), e->NewStringUTF(it.map), e->NewStringUTF(it.gameDescription),
                        e->NewStringUTF(it.gameTags), (jint)it.ping, (jint)it.players, (jint)it.maxPlayers,
                        (jboolean)it.password, (jint)it.serverVersion);
}

struct Browser;
static Browser *g_browser;

struct Query : PingResp {
    std::string address; uint16_t port; int hQuery = -1;
    gameserveritem_t item{}; bool responded = false, failed = false;
    void ServerResponded(gameserveritem_t &it) override;
    void ServerFailedToRespond() override;
};
struct Rules : RulesResp {
    std::string address; uint32_t ip = 0; uint16_t port = 0; int hQuery = -1;
    bool done = false, failed = false;
    std::map<std::string, std::string> rules;
    void RulesResponded(const char *k, const char *v) override { rules[k] = v ? v : ""; }
    void RulesFailedToRespond() override;
    void RulesRefreshComplete() override;
};
struct Browser : ServerListResponse {
    std::vector<Query *> queries;
    std::vector<Rules *> rules;
    bool refreshing = false;
    void *hRequest = nullptr;
    void ServerResponded(void *, int i) override {
        JNIEnv *e = sc::env(); if (!e || !sbResponded) return;
        e->CallStaticVoidMethod(sbClass, sbResponded, (jint)i); excCheck(e);
    }
    void ServerFailedToRespond(void *, int i) override {
        JNIEnv *e = sc::env(); if (!e || !sbFailed) return;
        e->CallStaticVoidMethod(sbClass, sbFailed, (jint)i); excCheck(e);
    }
    void RefreshComplete(void *, int) override {
        refreshing = false;
        JNIEnv *e = sc::env(); if (!e || !sbRefreshDone) return;
        e->CallStaticVoidMethod(sbClass, sbRefreshDone); excCheck(e);
    }
    Query *findQuery(const char *a, uint16_t p, int *idx = nullptr) {
        for (int i = (int)queries.size() - 1; i >= 0; i--)
            if (queries[i]->address == a && queries[i]->port == p) { if (idx) *idx = i; return queries[i]; }
        return nullptr;
    }
    Rules *findRules(const std::string &a, uint16_t p, int *idx = nullptr) {
        for (int i = (int)rules.size() - 1; i >= 0; i--)
            if (rules[i]->address == a && rules[i]->port == p) { if (idx) *idx = i; return rules[i]; }
        return nullptr;
    }
};

void Query::ServerResponded(gameserveritem_t &it) {
    item = it; responded = true; failed = false;
    JNIEnv *e = sc::env(); if (!e || !g_browser || !sbRespondedAddr) return;
    e->CallStaticVoidMethod(sbClass, sbRespondedAddr, e->NewStringUTF(address.c_str()), (jint)port); excCheck(e);
}
void Query::ServerFailedToRespond() {
    responded = true; failed = true;
    JNIEnv *e = sc::env(); if (!e || !g_browser || !sbFailedAddr) return;
    e->CallStaticVoidMethod(sbClass, sbFailedAddr, e->NewStringUTF(address.c_str()), (jint)port); excCheck(e);
}
void Rules::RulesFailedToRespond() {
    done = true; failed = true;
    sc::log(2, "RulesFailedToRespond: %s %u\n", address.c_str(), port);
    if (!g_browser) return;
    int i; if (g_browser->findRules(address, port, &i) == this) { g_browser->rules.erase(g_browser->rules.begin() + i); delete this; }
}
static jstring utf8(JNIEnv *e, const std::string &s) { return NewStandardString(e, s.c_str()); }
void Rules::RulesRefreshComplete() {
    done = true;
    Browser *b = g_browser;
    JNIEnv *e = sc::env();
    if (!b || !e || !sbRules || rules.empty()) return;
    std::map<std::string, std::vector<std::string>> parts;
    for (auto &kv : rules) {
        char name[256]; int idx, cnt;
        if (sscanf(kv.first.c_str(), "%255[^:]:%d/%d", name, &idx, &cnt) == 3) {
            if (idx < 1 || idx > 32 || cnt < 1 || cnt > 32 || idx > cnt) { sc::log(3, "BADLY FORMATTED server rule key: '%s'\n", kv.first.c_str()); continue; }
            auto &v = parts[name]; v.resize(cnt); v[idx - 1] = kv.second;
        } else parts[kv.first].push_back(kv.second);
    }
    jobjectArray arr = e->NewObjectArray((jsize)(2 * parts.size()), e->FindClass("java/lang/String"), nullptr);
    int i = 0;
    for (auto &p : parts) {
        std::string val; for (auto &s : p.second) val += s;
        e->SetObjectArrayElement(arr, i++, utf8(e, p.first));
        e->SetObjectArrayElement(arr, i++, utf8(e, val));
    }
    std::string addr = address; uint16_t prt = port;
    e->CallStaticVoidMethod(sbClass, sbRules, e->NewStringUTF(addr.c_str()), (jint)prt, arr); excCheck(e);
    e->DeleteLocalRef(arr);
    int k; if (g_browser && g_browser->findRules(addr, prt, &k) == this) { g_browser->rules.erase(g_browser->rules.begin() + k); delete this; }
}

JNIFN(jboolean, ServerBrowser, n_1Init)(JNIEnv *e, jclass cls) {
    if (!g_initialized) return false;
    sbClass = (jclass)e->NewGlobalRef(cls);
    sbResponded = e->GetStaticMethodID(cls, "onServerRespondedCallback", "(I)V");
    sbFailed = e->GetStaticMethodID(cls, "onServerFailedToRespondCallback", "(I)V");
    sbRefreshDone = e->GetStaticMethodID(cls, "onRefreshCompleteCallback", "()V");
    sbRespondedAddr = e->GetStaticMethodID(cls, "onServerRespondedCallback", "(Ljava/lang/String;I)V");
    sbFailedAddr = e->GetStaticMethodID(cls, "onServerFailedToRespondCallback", "(Ljava/lang/String;I)V");
    sbRules = e->GetStaticMethodID(cls, "onRulesRefreshComplete", "(Ljava/lang/String;I[Ljava/lang/String;)V");
    excCheck(e);
    g_browser = new Browser();
    return true;
}
JNIFN(void, ServerBrowser, n_1Shutdown)(JNIEnv *, jclass) {
    Browser *b = g_browser; if (!b) return;
    g_browser = nullptr;
    if (b->hRequest && MMS()) { if (b->refreshing) vcall<void, void *>(MMS(), 8, b->hRequest); vcall<void, void *>(MMS(), 6, b->hRequest); }
    for (auto *q : b->queries) if (!q->responded && MMS()) vcall<void, int>(MMS(), 16, q->hQuery);
    for (auto *r : b->rules) if (!r->done && MMS()) vcall<void, int>(MMS(), 16, r->hQuery);
    // listener objects are intentionally leaked: steamclient may still hold them
}
JNIFN(void, ServerBrowser, n_1RefreshInternetServers)(JNIEnv *, jclass) {
    Browser *b = g_browser; if (!b || b->refreshing || !MMS()) return;
    if (b->hRequest) { vcall<void, void *>(MMS(), 6, b->hRequest); b->hRequest = nullptr; }
    b->refreshing = true;
    static MatchMakingKeyValuePair_t kv; strcpy(kv.key, "gamedir"); strcpy(kv.value, "zomboid");
    MatchMakingKeyValuePair_t *filter = &kv;
    uint32_t app = vcall<uint32_t>(CUtils(), 9);
    b->hRequest = vcall<void *, uint32_t, MatchMakingKeyValuePair_t **, uint32_t, ServerListResponse *>(MMS(), 0, app, &filter, 1, b);
    sc::log(1, "Requesting internet server list...\n");
}
JNIFN(jint, ServerBrowser, n_1GetServerCount)(JNIEnv *, jclass) {
    return (g_browser && g_browser->hRequest) ? vcall<int, void *>(MMS(), 11, g_browser->hRequest) : 0;
}
JNIFN(jobject, ServerBrowser, n_1GetServerDetails__I)(JNIEnv *e, jclass, jint i) {
    if (!g_browser || !g_browser->hRequest) return nullptr;
    auto *p = vcall<gameserveritem_t *, void *, int>(MMS(), 7, g_browser->hRequest, i);
    if (!p) return nullptr;
    gameserveritem_t copy = *p;
    return makeDetails(e, copy);
}
JNIFN(void, ServerBrowser, n_1Release)(JNIEnv *, jclass) {
    Browser *b = g_browser; if (!b) return;
    if (b->hRequest) { if (b->refreshing) vcall<void, void *>(MMS(), 8, b->hRequest); vcall<void, void *>(MMS(), 6, b->hRequest); b->hRequest = nullptr; }
    b->refreshing = false;
}
JNIFN(jboolean, ServerBrowser, n_1IsRefreshing)(JNIEnv *, jclass) { return g_browser ? g_browser->refreshing : false; }

static bool parseIP(const char *s, uint32_t *ip) {
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    *ip = (uint32_t)a << 24 | (b & 255) << 16 | (c & 255) << 8 | (d & 255);
    return true;
}
JNIFN(jboolean, ServerBrowser, n_1QueryServer)(JNIEnv *e, jclass, jstring host, jint port) {
    if (!host) { ThrowJava(e, "java/lang/NullPointerException", "host is NULL"); return false; }
    Browser *b = g_browser; if (!b || !MMS()) return false;
    const char *k = e->GetStringUTFChars(host, nullptr);
    jboolean r = false;
    int idx; Query *q = b->findQuery(k, (uint16_t)port, &idx);
    if (q && !q->responded) r = true;
    else {
        if (q) { delete q; b->queries.erase(b->queries.begin() + idx); }
        uint32_t ip;
        if (parseIP(k, &ip)) {
            q = new Query(); q->address = k; q->port = (uint16_t)port;
            q->hQuery = vcall<int, uint32_t, uint16_t, PingResp *>(MMS(), 13, ip, (uint16_t)port, q);
            b->queries.push_back(q); r = true;
        }
    }
    e->ReleaseStringUTFChars(host, k);
    return r;
}
JNIFN(jobject, ServerBrowser, n_1GetServerDetails__Ljava_lang_String_2I)(JNIEnv *e, jclass, jstring host, jint port) {
    if (!g_browser || !host) return nullptr;
    const char *k = e->GetStringUTFChars(host, nullptr);
    Query *q = g_browser->findQuery(k, (uint16_t)port);
    e->ReleaseStringUTFChars(host, k);
    return (q && q->responded) ? makeDetails(e, q->item) : nullptr;
}
JNIFN(void, ServerBrowser, n_1ReleaseServerQuery)(JNIEnv *e, jclass, jstring host, jint port) {
    if (!g_browser || !host) return;
    const char *k = e->GetStringUTFChars(host, nullptr);
    int idx; Query *q = g_browser->findQuery(k, (uint16_t)port, &idx);
    e->ReleaseStringUTFChars(host, k);
    if (!q) return;
    if (!q->responded && MMS()) vcall<void, int>(MMS(), 16, q->hQuery);
    g_browser->queries.erase(g_browser->queries.begin() + idx);
    delete q;
}
JNIFN(jboolean, ServerBrowser, n_1RequestServerRules)(JNIEnv *e, jclass, jstring host, jint port) {
    Browser *b = g_browser; if (!b || !host || !MMS()) return false;
    const char *k = e->GetStringUTFChars(host, nullptr);
    uint32_t ip; bool ok = parseIP(k, &ip);
    if (ok) {
        Rules *r = new Rules(); r->address = k; r->ip = ip; r->port = (uint16_t)port;
        b->rules.push_back(r);
        r->hQuery = vcall<int, uint32_t, uint16_t, RulesResp *>(MMS(), 15, ip, (uint16_t)port, r);
    }
    e->ReleaseStringUTFChars(host, k);
    return ok;
}

// ================================================================== SteamFriends
static jclass frClass;
static jmethodID frStatus, frAvatar, frName;
static jmethodID g_BufferPosition;
static bool frRegistered, frActive;

JNIFN(void, SteamFriends, n_1Init)(JNIEnv *e, jclass cls) {
    if (!g_initialized) return;
    frClass = (jclass)e->NewGlobalRef(cls);
    frStatus = e->GetStaticMethodID(cls, "onStatusChangedCallback", "(J)V");
    frAvatar = e->GetStaticMethodID(cls, "onAvatarChangedCallback", "(J)V");
    frName = e->GetStaticMethodID(cls, "onProfileNameChanged", "(J)V");
    excCheck(e);
    frActive = true;
    if (!frRegistered) {
        frRegistered = true;
        sc::registerCallback(304, false, [](void *d) {
            uint64_t id; int flags; memcpy(&id, d, 8); memcpy(&flags, (uint8_t *)d + 8, 4);
            JNIEnv *e = sc::env(); if (!e || !frActive) return;
            if ((flags & 0x2) && frStatus) { e->CallStaticVoidMethod(frClass, frStatus, (jlong)id); excCheck(e); }
            if ((flags & 0x40) && frAvatar) { e->CallStaticVoidMethod(frClass, frAvatar, (jlong)id); excCheck(e); }
            if ((flags & 0x401) && frName) { e->CallStaticVoidMethod(frClass, frName, (jlong)id); excCheck(e); }
        });
    }
}
JNIFN(void, SteamFriends, n_1Shutdown)(JNIEnv *, jclass) { frActive = false; }
JNIFN(jstring, SteamFriends, GetPersonaName)(JNIEnv *e, jclass) {
    if (!g_initialized || !Friends()) return nullptr;
    return NewStandardString(e, vcall<const char *>(Friends(), 0));
}
JNIFN(jint, SteamFriends, GetFriendCount)(JNIEnv *, jclass) {
    return (g_initialized && Friends()) ? vcall<int, int>(Friends(), 2, 4) : 0;
}
JNIFN(jlong, SteamFriends, GetFriendByIndex)(JNIEnv *, jclass, jint i) {
    return (g_initialized && Friends()) ? (jlong)vcall<uint64_t, int, int>(Friends(), 3, i, 4) : 0;
}
JNIFN(jstring, SteamFriends, GetFriendPersonaName)(JNIEnv *e, jclass, jlong id) {
    if (!g_initialized || !Friends()) return nullptr;
    return NewStandardString(e, vcall<const char *, uint64_t>(Friends(), 6, (uint64_t)id));
}
JNIFN(jint, SteamFriends, GetFriendPersonaState)(JNIEnv *, jclass, jlong id) {
    return (g_initialized && Friends()) ? vcall<int, uint64_t>(Friends(), 5, (uint64_t)id) : -1;
}
JNIFN(jboolean, SteamFriends, InviteUserToGame)(JNIEnv *e, jclass, jlong id, jstring s) {
    if (!g_initialized || !Friends()) return false;
    const char *c = e->GetStringUTFChars(s, nullptr);
    vcall<bool, uint64_t, const char *>(Friends(), 47, (uint64_t)id, c);
    e->ReleaseStringUTFChars(s, c);
    return false;  // original discards the Steam result
}
JNIFN(void, SteamFriends, ActivateGameOverlay)(JNIEnv *e, jclass, jstring s) {
    if (!g_initialized || !Friends()) return;
    const char *c = e->GetStringUTFChars(s, nullptr);
    vcall<void, const char *>(Friends(), 27, c);
    e->ReleaseStringUTFChars(s, c);
}
JNIFN(void, SteamFriends, ActivateGameOverlayToUser)(JNIEnv *e, jclass, jstring s, jlong id) {
    if (!g_initialized || !Friends()) return;
    const char *c = e->GetStringUTFChars(s, nullptr);
    vcall<void, const char *, uint64_t>(Friends(), 28, c, (uint64_t)id);
    e->ReleaseStringUTFChars(s, c);
}
JNIFN(void, SteamFriends, ActivateGameOverlayToWebPage)(JNIEnv *e, jclass, jstring s) {
    if (!g_initialized || !Friends()) return;
    const char *c = e->GetStringUTFChars(s, nullptr);
    vcall<void, const char *, int>(Friends(), 29, c, 0);
    e->ReleaseStringUTFChars(s, c);
}
JNIFN(void, SteamFriends, SetPlayedWith)(JNIEnv *, jclass, jlong id) {
    if (g_initialized && Friends()) vcall<void, uint64_t>(Friends(), 31, (uint64_t)id);
}
JNIFN(jint, SteamFriends, CreateSteamAvatar)(JNIEnv *e, jclass, jlong id, jobject buf) {
    if (!buf) { ThrowJava(e, "java/lang/NullPointerException", "buffer is NULL"); return 0; }
    if (!g_BufferPosition) g_BufferPosition = e->GetMethodID(e->GetObjectClass(buf), "position", "(I)Ljava/nio/Buffer;");
    if (!g_BufferPosition || !Friends() || !CUtils()) return 0;
    uint8_t *dst = (uint8_t *)e->GetDirectBufferAddress(buf);
    jlong cap = e->GetDirectBufferCapacity(buf);
    int h = vcall<int, uint64_t>(Friends(), 33, (uint64_t)id);
    if (h == 0 || h == -1) return 0;
    uint32_t w = 0, ht = 0;
    if (!vcall<bool, int, uint32_t *, uint32_t *>(CUtils(), 5, h, &w, &ht) || !w || !ht) return 0;
    if (cap >= 0 && (jlong)w * ht * 4 > cap) return 0;  // bounds check the original lacked
    vcall<bool, int, uint8_t *, int>(CUtils(), 6, h, dst, (int)(w * ht * 4));
    jobject r = e->CallObjectMethod(buf, g_BufferPosition, (jint)(w * ht * 4));
    if (r) e->DeleteLocalRef(r);
    return (jint)w;
}
JNIFN(void, SteamFriends, UpdateRichPresenceConnectionInfo)(JNIEnv *e, jclass, jstring status, jstring connect) {
    if (!g_initialized || !Friends()) return;
    const char *s = e->GetStringUTFChars(status, nullptr), *c = e->GetStringUTFChars(connect, nullptr);
    if (!vcall<bool, const char *, const char *>(Friends(), 41, "status", s)) sc::log(3, "SetRichPresence(status) failed\n");
    if (!vcall<bool, const char *, const char *>(Friends(), 41, "connect", c)) sc::log(3, "SetRichPresence(connect) failed\n");
    e->ReleaseStringUTFChars(status, s); e->ReleaseStringUTFChars(connect, c);
}

// ================================================================== SteamGameServer
struct ZNetServer;
ZNetServer *ZNetServer_Create(uint16_t gamePort, uint16_t udpPort);
void ZNetServer_SetMaxPlayers(int n);
int ZNetServer_ConnectState();
bool ZNetServer_Exists();
void ZNetServer_AddPlayer(int idx, const char *name, int score);
void ZNetServer_RemovePlayer(int idx);
void ZNetServer_UpdatePlayer(int idx, int score);
extern ZNetServer *g_znetServer;

void GameServerShutdown() { sc::shutdownGameServer(); }
void RegisterNetworkingCallbacks();

static void SteamAPIDebugTextHook(int sev, const char *t) { sc::log(3, "warning [%d]: %s\n", sev, t); }

#define GS_GUARD(ret) \
    if (!ZNetServer_Exists()) { ThrowJava(env, "java/lang/IllegalStateException", "ZNetServer::instance == NULL"); return ret; } \
    void *gs = gsServer(); if (!gs) return ret;

static jboolean gsString(JNIEnv *env, jstring s, int slot, const char *nullMsg) {
    GS_GUARD(false);
    if (!s) { ThrowJava(env, "java/lang/NullPointerException", nullMsg); return false; }
    char *p = GetStandardUTFChars(env, s);
    vcall<void, const char *>(gs, slot, p);
    ReleaseStandardUTFChars(p);
    return true;
}

JNIFN(jboolean, SteamGameServer, Init)(JNIEnv *env, jclass, jstring ipStr, jint gamePort, jint udpPort, jint mode, jstring version) {
    if (ZNetServer_Exists()) { ThrowJava(env, "java/lang/IllegalStateException", "ZNetServer::instance != NULL"); return false; }
    if (!version) { ThrowJava(env, "java/lang/NullPointerException", "version is null"); return false; }
    if (!sc::jvm) env->GetJavaVM(&sc::jvm);
    uint32_t ip = 0;
    if (ipStr) {
        const char *s = env->GetStringUTFChars(ipStr, nullptr);
        bool ok = parseIP(s, &ip) && ip != 0;
        if (!ok) sc::log(4, "ERROR: couldn't parse IP='%s'\n", s);
        env->ReleaseStringUTFChars(ipStr, s);
        if (!ok) return false;
    }
    if (mode < 1 || mode > 3) { ThrowJava(env, "java/lang/IllegalArgumentException", "invalid serverMode"); return false; }

    RegisterNetworkingCallbacks();
    char *ver = GetStandardUTFChars(env, version);
    char err[1024] = {0};
    int r = sc::initGameServer(ip, (uint16_t)gamePort, 0xFFFF, mode, ver, err, sizeof err);
    ReleaseStandardUTFChars(ver);
    if (r != 0) {
        sc::log(4, "SteamGameServer_Init() failed ip=%u.%u.%u.%u gamePort=%d UDPPort=%d: %s\n",
                ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, gamePort, udpPort, err);
        return false;
    }
    ZNetServer_Create((uint16_t)gamePort, (uint16_t)udpPort);
    sc::log(1, "SteamGameServer seems to be initialized\n");
    if (void *u = sc::gsIfaceUser0("SteamUtils010")) vcall<void, void (*)(int, const char *)>(u, 16, SteamAPIDebugTextHook);
    return true;
}
JNIFN(void, SteamGameServer, SetProduct)(JNIEnv *env, jclass, jstring s) { gsString(env, s, 1, "product is null"); }
JNIFN(void, SteamGameServer, SetGameDescription)(JNIEnv *env, jclass, jstring s) { gsString(env, s, 2, "description is null"); }
JNIFN(void, SteamGameServer, SetModDir)(JNIEnv *env, jclass, jstring s) { gsString(env, s, 3, "modDir is null"); }
JNIFN(void, SteamGameServer, SetServerName)(JNIEnv *env, jclass, jstring s) { gsString(env, s, 14, "serverName is null"); }
JNIFN(void, SteamGameServer, SetMapName)(JNIEnv *env, jclass, jstring s) { gsString(env, s, 15, "mapName is null"); }
JNIFN(void, SteamGameServer, SetGameTags)(JNIEnv *env, jclass, jstring s) { gsString(env, s, 21, "gameTags is null"); }
JNIFN(void, SteamGameServer, SetRegion)(JNIEnv *env, jclass, jstring s) { gsString(env, s, 23, "region is null"); }
JNIFN(void, SteamGameServer, SetDedicatedServer)(JNIEnv *env, jclass, jboolean b) { GS_GUARD(); vcall<void, bool>(gs, 4, b == 1); }
JNIFN(void, SteamGameServer, LogOnAnonymous)(JNIEnv *env, jclass) { GS_GUARD(); vcall<void>(gs, 6); }
JNIFN(void, SteamGameServer, EnableHeartBeats)(JNIEnv *env, jclass, jboolean b) { GS_GUARD(); vcall<void, bool>(gs, 24, b == 1); }
JNIFN(void, SteamGameServer, SetMaxPlayerCount)(JNIEnv *env, jclass, jint n) {
    GS_GUARD(); ZNetServer_SetMaxPlayers(n); vcall<void, int>(gs, 12, n);
}
JNIFN(void, SteamGameServer, SetKeyValue)(JNIEnv *env, jclass, jstring k, jstring v) {
    GS_GUARD();
    if (!k) { ThrowJava(env, "java/lang/NullPointerException", "key is null"); return; }
    char *key = GetStandardUTFChars(env, k), *val = GetStandardUTFChars(env, v);
    size_t len = strlen(val);
    if (len + 1 <= 128) vcall<void, const char *, const char *>(gs, 20, key, val);
    else {
        // split into code-point-aligned chunks of <= 127 bytes, "key:i/N" (fixed: keeps the final code point)
        std::vector<std::pair<size_t, size_t>> b;
        size_t start = 0, pos = 0;
        while (pos < len && b.size() < 31) {
            size_t next = pos + 1;
            while (next < len && ((uint8_t)val[next] & 0xC0) == 0x80) next++;
            if (next - start > 127) { b.push_back({start, pos}); start = pos; }
            pos = next;
        }
        if (b.size() < 31 && start < len) b.push_back({start, len});
        for (size_t i = 0; i < b.size(); i++) {
            char kk[128]; snprintf(kk, sizeof kk, "%s:%zu/%zu", key, i + 1, b.size());
            std::string chunk(val + b[i].first, b[i].second - b[i].first);
            vcall<void, const char *, const char *>(gs, 20, kk, chunk.c_str());
        }
    }
    ReleaseStandardUTFChars(key); ReleaseStandardUTFChars(val);
}
JNIFN(jboolean, SteamGameServer, BUpdateUserData)(JNIEnv *env, jclass, jlong id, jstring name, jint score) {
    GS_GUARD(true);
    if (!name) { ThrowJava(env, "java/lang/NullPointerException", "playerName is null"); return true; }
    char *n = GetStandardUTFChars(env, name);
    bool r = vcall<bool, uint64_t, const char *, uint32_t>(gs, 41, (uint64_t)id, n, (uint32_t)score);
    ReleaseStandardUTFChars(n);
    return r;
}
JNIFN(jint, SteamGameServer, GetSteamServersConnectState)(JNIEnv *env, jclass) {
    if (!ZNetServer_Exists()) { ThrowJava(env, "java/lang/IllegalStateException", "ZNetServer::instance == NULL"); return -1; }
    return ZNetServer_ConnectState();
}
JNIFN(jlong, SteamGameServer, GetSteamID)(JNIEnv *, jclass) {
    void *gs = gsServer();
    return gs ? (jlong)vcall<uint64_t>(gs, 10) : 0;
}
JNIFN(void, SteamGameServer, AddPlayer)(JNIEnv *env, jclass, jshort idx, jstring name, jint score) {
    if (!ZNetServer_Exists()) { ThrowJava(env, "java/lang/IllegalStateException", "ZNetServer::instance == NULL"); return; }
    if (!name) { ThrowJava(env, "java/lang/NullPointerException", "player name is null"); return; }
    char *n = GetStandardUTFChars(env, name);
    ZNetServer_AddPlayer(idx, n, score);
    ReleaseStandardUTFChars(n);
}
JNIFN(void, SteamGameServer, RemovePlayer)(JNIEnv *, jclass, jshort idx) { ZNetServer_RemovePlayer(idx); }
JNIFN(void, SteamGameServer, UpdatePlayer)(JNIEnv *, jclass, jshort idx, jint score) { ZNetServer_UpdatePlayer(idx, score); }
