// SteamUtils / SteamUser / SteamRemotePlay / SteamWorkshop JNI.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "znet.h"

using sc::vcall;

// g_initialized / g_bServer live in libZNetNoSteam64 (shared).

static const uint32_t kAppId = 108600;

// ---------------------------------------------------------------- helpers
char *GetStandardUTFChars(JNIEnv *env, jstring s) {
    if (!s) { char *p = new char[1]; p[0] = 0; return p; }
    jclass sc = env->FindClass("java/lang/String");
    jmethodID gb = env->GetMethodID(sc, "getBytes", "(Ljava/lang/String;)[B");
    jstring utf8 = env->NewStringUTF("UTF-8");
    jbyteArray b = (jbyteArray)env->CallObjectMethod(s, gb, utf8);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe(); env->ExceptionClear();
        const char *m = "ExceptionOccurred"; char *p = new char[strlen(m) + 1]; strcpy(p, m); return p;
    }
    jsize n = env->GetArrayLength(b);
    char *p = new char[n + 1];
    env->GetByteArrayRegion(b, 0, n, (jbyte *)p);
    p[n] = 0;
    env->DeleteLocalRef(b); env->DeleteLocalRef(utf8); env->DeleteLocalRef(sc);
    return p;
}
void ReleaseStandardUTFChars(char *p) { delete[] p; }

jstring NewStandardString(JNIEnv *env, const char *s) {
    size_t n = strlen(s);
    jbyteArray b = env->NewByteArray((jsize)n);
    env->SetByteArrayRegion(b, 0, (jsize)n, (const jbyte *)s);
    jclass sc = env->FindClass("java/lang/String");
    jmethodID init = env->GetMethodID(sc, "<init>", "([BLjava/lang/String;)V");
    jstring utf8 = env->NewStringUTF("UTF-8");
    jstring r = (jstring)env->NewObject(sc, init, b, utf8);
    env->DeleteLocalRef(b); env->DeleteLocalRef(utf8); env->DeleteLocalRef(sc);
    return r;
}

void ThrowJava(JNIEnv *env, const char *cls, const char *msg) {
    jclass c = env->FindClass(cls);
    if (c) env->ThrowNew(c, msg);
}

static void *SteamUtils() { return g_bServer ? sc::gsIfaceUser0("SteamUtils010") : sc::ifaceUser0("SteamUtils010"); }
static void *ClientUtils() { return sc::ifaceUser0("SteamUtils010"); }
static void *UGC() { return g_bServer ? sc::gsIface("STEAMUGC_INTERFACE_VERSION021") : sc::iface("STEAMUGC_INTERFACE_VERSION021"); }

// ---------------------------------------------------------------- SteamUtils
static jclass g_class;
static jmethodID m_joinRequest, m_clientInitiate, m_validateOwner, m_gamepadDismissed, m_floatingDismissed;
static bool g_handlersRegistered;

static void checkExc(JNIEnv *e) { if (e->ExceptionCheck()) { e->ExceptionDescribe(); e->ExceptionClear(); } }

int SteamUtils_ClientInitiateConnectionCallback(uint64_t id) {
    JNIEnv *e = sc::env();
    if (!e || !g_class || !m_clientInitiate) { sc::log(2, "SteamUtils_ClientInitiateConnectionCallback: returning default 1\n"); return 1; }
    int r = e->CallStaticIntMethod(g_class, m_clientInitiate, (jlong)id);
    if (e->ExceptionCheck()) { checkExc(e); return 1; }
    return r;
}
int SteamUtils_ValidateOwnerCallback(uint64_t a, uint64_t b) {
    JNIEnv *e = sc::env();
    if (!e || !g_class || !m_validateOwner) { sc::log(2, "SteamUtils_ValidateOwnerCallback: returning default 1\n"); return 1; }
    int r = e->CallStaticIntMethod(g_class, m_validateOwner, (jlong)a, (jlong)b);
    if (e->ExceptionCheck()) { checkExc(e); return 1; }
    return r;
}

static void SteamAPIDebugTextHook(int sev, const char *txt) { sc::log(3, "warning [%d]: %s\n", sev, txt); }

#pragma pack(push, 4)
struct GameRichPresenceJoinRequested_t { uint64_t friendId; char connect[256]; };
struct GamepadTextInputDismissed_t { bool submitted; uint32_t len; uint32_t appId; };
#pragma pack(pop)

static void registerUtilsHandlers() {
    if (g_handlersRegistered) return;
    g_handlersRegistered = true;
    sc::registerCallback(337, false, [](void *d) {
        auto *c = (GameRichPresenceJoinRequested_t *)d;
        JNIEnv *e = sc::env(); if (!e || !g_class || !m_joinRequest) return;
        jstring s = e->NewStringUTF(c->connect);
        e->CallStaticVoidMethod(g_class, m_joinRequest, (jlong)c->friendId, s);
        checkExc(e);
    });
    sc::registerCallback(714, false, [](void *d) {
        auto *c = (GamepadTextInputDismissed_t *)d;
        JNIEnv *e = sc::env(); if (!e || !g_class || !m_gamepadDismissed) return;
        if (c->submitted) {
            uint32_t len = c->len;
            char *buf = new char[len + 1];
            if (vcall<bool, char *, uint32_t>(ClientUtils(), 22, buf, len)) {  // GetEnteredGamepadTextInput
                buf[len] = 0;
                e->CallStaticVoidMethod(g_class, m_gamepadDismissed, e->NewStringUTF(buf));
                checkExc(e);
            }
            delete[] buf;
        } else {
            e->CallStaticVoidMethod(g_class, m_gamepadDismissed, (jstring) nullptr);
            checkExc(e);
        }
    });
    sc::registerCallback(738, false, [](void *) {
        JNIEnv *e = sc::env(); if (!e || !g_class || !m_floatingDismissed) return;
        e->CallStaticVoidMethod(g_class, m_floatingDismissed);
        checkExc(e);
    });
}

JNIFN(jboolean, SteamUtils, n_1Init)(JNIEnv *env, jclass cls, jboolean bServer) {
    if (!sc::jvm) env->GetJavaVM(&sc::jvm);
    g_class = (jclass)env->NewGlobalRef(cls);
    m_joinRequest = env->GetStaticMethodID(cls, "joinRequestCallback", "(JLjava/lang/String;)V");
    m_clientInitiate = env->GetStaticMethodID(cls, "clientInitiateConnectionCallback", "(J)I");
    m_validateOwner = env->GetStaticMethodID(cls, "validateOwnerCallback", "(JJ)I");
    m_gamepadDismissed = env->GetStaticMethodID(cls, "gamepadTextInputDismissedCallback", "(Ljava/lang/String;)V");
    m_floatingDismissed = env->GetStaticMethodID(cls, "floatingGamepadTextInputDismissedCallback", "()V");
    checkExc(env);
    sc::log(1, "Compiled with SERVER_STEAMAPI == 0 (arm64 port)\n");
    if (bServer) {
        sc::log(1, "Starting with bServer == true\nSteamAPI initialised successfully\n");
        g_initialized = true; g_bServer = true;
        return JNI_TRUE;
    }
    sc::log(1, "Starting with bServer == false\n");
    char err[1024] = {0};
    int r = sc::initClient(err, sizeof err);
    if (r != 0) {
        sc::log(2, "SteamAPI_InitEx() error: %s\n", err);
        g_initialized = false; g_bServer = false;
        return JNI_FALSE;
    }
    sc::log(1, "SteamAPI initialised successfully\n");
    g_initialized = true; g_bServer = false;
    registerUtilsHandlers();
    vcall<void, void (*)(int, const char *)>(sc::client, 22, SteamAPIDebugTextHook);   // SetWarningMessageHook
    vcall<void, void (*)(int, const char *)>(ClientUtils(), 16, SteamAPIDebugTextHook); // SetWarningMessageHook
    vcall<void, int>(ClientUtils(), 10, 1);  // SetOverlayNotificationPosition(TopRight)
    return JNI_TRUE;
}

JNIFN(void, SteamUtils, n_1Shutdown)(JNIEnv *env, jclass) {
    if (!g_initialized) return;
    if (g_class) { env->DeleteGlobalRef(g_class); g_class = nullptr; }
    if (!g_bServer) sc::shutdownClient();
    g_initialized = false;
}

void UpdateSteamClientAndServer();  // networking.cpp

JNIFN(void, SteamUtils, n_1RunLoop)(JNIEnv *, jclass) {
    if (!g_initialized) return;
    if (!g_bServer) sc::runFrameClient();
    UpdateSteamClientAndServer();
}

JNIFN(jboolean, SteamUtils, n_1IsOverlayEnabled)(JNIEnv *, jclass) {
    void *u = g_initialized ? SteamUtils() : nullptr;
    return u ? vcall<bool>(u, 17) : false;
}
JNIFN(jboolean, SteamUtils, n_1IsSteamRunningOnSteamDeck)(JNIEnv *, jclass) {
    void *u = g_initialized ? SteamUtils() : nullptr;
    return u ? vcall<bool>(u, 34) : false;
}
JNIFN(jboolean, SteamUtils, n_1ShowGamepadTextInput)(JNIEnv *env, jclass, jint mode, jint lineMode, jstring desc, jint max, jstring existing) {
    void *u = g_initialized ? SteamUtils() : nullptr;
    if (!u || (unsigned)mode > 1 || (unsigned)lineMode > 1) return false;
    char *d = GetStandardUTFChars(env, desc);
    char *x = existing ? GetStandardUTFChars(env, existing) : nullptr;
    bool r = vcall<bool, int, int, const char *, uint32_t, const char *>(u, 20, mode, lineMode, d, (uint32_t)max, x);
    ReleaseStandardUTFChars(d); if (x) ReleaseStandardUTFChars(x);
    return r;
}
JNIFN(jboolean, SteamUtils, n_1ShowFloatingGamepadTextInput)(JNIEnv *, jclass, jint mode, jint x, jint y, jint w, jint h) {
    void *u = g_initialized ? SteamUtils() : nullptr;
    if (!u || (unsigned)mode > 3) return false;
    bool r = vcall<bool, int, int, int, int, int>(u, 35, mode, x, y, w, h);
    sc::log(2, "ShowFloatingGamepadTextInput x,y,width,height=%d,%d,%d,%d result=%s\n", x, y, w, h, r ? "true" : "false");
    return r;
}

// ---------------------------------------------------------------- SteamUser / RemotePlay
JNIFN(jlong, SteamUser, n_1GetSteamID)(JNIEnv *, jclass) {
    void *u = (g_initialized && !g_bServer) ? sc::iface("SteamUser023") : nullptr;
    return u ? (jlong)vcall<uint64_t>(u, 2) : 0;
}
JNIFN(jint, SteamRemotePlay, n_1GetSessionCount)(JNIEnv *, jclass) {
    void *r = (g_initialized && !g_bServer) ? sc::iface("STEAMREMOTEPLAY_INTERFACE_VERSION003") : nullptr;
    return r ? (jint)vcall<uint32_t>(r, 0) : 0;
}

// ---------------------------------------------------------------- SteamWorkshop
#pragma pack(push, 4)
struct CreateItemResult_t { int32_t eResult; uint64_t fileId; bool needsLegal; };
struct SubmitItemUpdateResult_t { int32_t eResult; bool needsLegal; uint64_t fileId; };
struct SubscribeResult_t { int32_t eResult; uint64_t fileId; };
struct DownloadItemResult_t { uint32_t appId; uint64_t fileId; int32_t eResult; };
struct SteamUGCQueryCompleted_t { uint64_t handle; int32_t eResult; uint32_t numResults; uint32_t total; bool cached; char cursor[256]; };
struct SteamParamStringArray_t { const char **strings; int32_t count; };
struct SteamUGCDetails_t {
    uint64_t fileId; int32_t eResult, fileType; uint32_t creatorApp, consumerApp;
    char title[129]; char description[8000];
    uint64_t owner; uint32_t created, updated, added; int32_t visibility;
    bool banned, accepted, tagsTruncated; char tags[1025];
    uint64_t hFile, hPreview; char fileName[260]; int32_t fileSize, previewSize; char url[256];
    uint32_t up, down; float score; uint32_t numChildren; uint64_t totalFilesSize;
};
#pragma pack(pop)
static_assert(offsetof(SteamUGCDetails_t, title) == 0x18, "title");
static_assert(offsetof(SteamUGCDetails_t, created) == 0x1fe4, "created");
static_assert(offsetof(SteamUGCDetails_t, fileSize) == 0x250c, "fileSize");
static_assert(offsetof(SteamUGCDetails_t, numChildren) == 0x2620, "numChildren");
static_assert(sizeof(SteamUGCDetails_t) == 0x262c, "size");
static_assert(sizeof(SteamUGCQueryCompleted_t) == 0x118, "query size");

struct WS {
    jobject obj = nullptr;
    jmethodID created, notCreated, updated, notUpdated, subscribed, notSubscribed, downloaded, notDownloaded, queryDone, queryFailed;
    uint64_t createdFileId = 0, updateHandle = ~0ull;
    bool creatingItem = false, updateStarted = false, updateSubmitted = false;
    uint64_t pendingSubscribe = 0, pendingDownload = 0;
    uint64_t createCall = 0, submitCall = 0, subscribeCall = 0;
};
static WS *ws;
static bool wsCallbacksRegistered;

#define WSCALL(m, ...) do { JNIEnv *e_ = sc::env(); if (ws && ws->m && e_) e_->CallVoidMethod(ws->obj, ws->m, ##__VA_ARGS__); } while (0)

static void onDownloadItemResult(void *d) {
    auto *r = (DownloadItemResult_t *)d;
    if (!ws) return;
    void *u = SteamUtils();
    uint32_t app = u ? vcall<uint32_t>(u, 9) : kAppId;
    if (r->appId != app) { sc::log(3, "DownloadItemResult_t for appID=%llu ignored\n", (unsigned long long)r->appId); return; }
    if (r->fileId != ws->pendingDownload) { sc::log(3, "DownloadItemResult_t for fileId=%llu ignored\n", (unsigned long long)r->fileId); return; }
    ws->pendingDownload = 0;
    if (r->eResult == 1) WSCALL(downloaded, (jlong)r->fileId);
    else WSCALL(notDownloaded, (jlong)r->fileId, (jint)r->eResult);
}

#define WS_GUARD(ret) \
    sc::log(0, "%s\n", __func__); \
    if (!ws) { ThrowJava(env, "java/lang/IllegalStateException", "ZNetWorkshop::instance == NULL"); return ret; }

JNIFN(void, SteamWorkshop, n_1Init)(JNIEnv *env, jobject self) {
    if (!g_initialized) return;
    WS *w = new WS();
    w->obj = env->NewGlobalRef(self);
    jclass c = env->GetObjectClass(self);
    w->created = env->GetMethodID(c, "onItemCreated", "(JZ)V");
    w->notCreated = env->GetMethodID(c, "onItemNotCreated", "(I)V");
    w->updated = env->GetMethodID(c, "onItemUpdated", "(Z)V");
    w->notUpdated = env->GetMethodID(c, "onItemNotUpdated", "(I)V");
    w->subscribed = env->GetMethodID(c, "onItemSubscribed", "(J)V");
    w->notSubscribed = env->GetMethodID(c, "onItemNotSubscribed", "(JI)V");
    w->downloaded = env->GetMethodID(c, "onItemDownloaded", "(J)V");
    w->notDownloaded = env->GetMethodID(c, "onItemNotDownloaded", "(JI)V");
    w->queryDone = env->GetMethodID(c, "onItemQueryCompleted", "(JI)V");
    w->queryFailed = env->GetMethodID(c, "onItemQueryNotCompleted", "(JI)V");
    checkExc(env);
    if (!wsCallbacksRegistered) {
        wsCallbacksRegistered = true;
        sc::registerCallback(3406, false, onDownloadItemResult);
        sc::registerCallback(3406, true, onDownloadItemResult);
    }
    ws = w;
}

JNIFN(void, SteamWorkshop, n_1Shutdown)(JNIEnv *env, jobject) {
    if (!ws) return;
    sc::cancelCallResult(ws->createCall); sc::cancelCallResult(ws->submitCall); sc::cancelCallResult(ws->subscribeCall);
    env->DeleteGlobalRef(ws->obj);
    delete ws; ws = nullptr;
}

JNIFN(jboolean, SteamWorkshop, n_1CreateItem)(JNIEnv *env, jobject) {
    WS_GUARD(false);
    if (ws->creatingItem) { ThrowJava(env, "java/lang/RuntimeException", "already creating an item"); return false; }
    if (ws->updateHandle != ~0ull) {
        if (ws->updateStarted) { ThrowJava(env, "java/lang/RuntimeException", "already started item update"); return false; }
        if (ws->updateSubmitted) { ThrowJava(env, "java/lang/RuntimeException", "already submitted item update"); return false; }
    }
    uint64_t h = vcall<uint64_t, uint32_t, int>(UGC(), 44, kAppId, 0);
    if (!h) { sc::log(3, "SteamUGC.CreateItem() result=invalid\n"); return false; }
    ws->createCall = h;
    sc::bindCallResult(h, 3403, g_bServer, [](void *d, bool) {
        auto *r = (CreateItemResult_t *)d;
        if (!ws) return;
        ws->creatingItem = false;
        if (r->eResult == 1) { ws->createdFileId = r->fileId; WSCALL(created, (jlong)r->fileId, (jboolean)r->needsLegal); }
        else WSCALL(notCreated, (jint)r->eResult);
    });
    ws->creatingItem = true;
    return true;
}

JNIFN(jboolean, SteamWorkshop, n_1StartItemUpdate)(JNIEnv *env, jobject, jlong fileId) {
    WS_GUARD(false);
    if (ws->updateHandle != ~0ull) {
        if (ws->updateStarted) { ThrowJava(env, "java/lang/RuntimeException", "already started item update"); return false; }
        if (ws->updateSubmitted) { ThrowJava(env, "java/lang/RuntimeException", "already submitted item update"); return false; }
    }
    if (ws->creatingItem) return false;
    uint64_t h = vcall<uint64_t, uint32_t, uint64_t>(UGC(), 45, kAppId, (uint64_t)fileId);
    ws->updateHandle = h;
    ws->updateStarted = (h != ~0ull);
    return ws->updateStarted;
}

static bool updateReady(JNIEnv *env) {
    if (ws->updateHandle != ~0ull && ws->updateStarted) return true;
    ThrowJava(env, "java/lang/RuntimeException", "item update wasn't started");
    return false;
}

static jboolean setString(JNIEnv *env, jstring s, int slot) {
    WS_GUARD(false);
    if (!updateReady(env)) return false;
    char *p = GetStandardUTFChars(env, s);
    bool r = false;
    if (ws->updateHandle != ~0ull && ws->updateStarted) r = vcall<bool, uint64_t, const char *>(UGC(), slot, ws->updateHandle, p);
    ReleaseStandardUTFChars(p);
    return r;
}
JNIFN(jboolean, SteamWorkshop, n_1SetItemTitle)(JNIEnv *env, jobject, jstring s) { return setString(env, s, 46); }
JNIFN(jboolean, SteamWorkshop, n_1SetItemDescription)(JNIEnv *env, jobject, jstring s) { return setString(env, s, 47); }
JNIFN(jboolean, SteamWorkshop, n_1SetItemContent)(JNIEnv *env, jobject, jstring s) { return setString(env, s, 52); }
JNIFN(jboolean, SteamWorkshop, n_1SetItemPreview)(JNIEnv *env, jobject, jstring s) { return setString(env, s, 53); }

JNIFN(jboolean, SteamWorkshop, n_1SetItemVisibility)(JNIEnv *env, jobject, jint v) {
    WS_GUARD(false);
    if (!updateReady(env)) return false;
    if ((unsigned)v > 3) { ThrowJava(env, "java/lang/RuntimeException", "invalid visibility specified"); return false; }
    return vcall<bool, uint64_t, int>(UGC(), 50, ws->updateHandle, v);
}

JNIFN(jboolean, SteamWorkshop, n_1SetItemTags)(JNIEnv *env, jobject, jobjectArray tags) {
    WS_GUARD(false);
    if (!updateReady(env)) return false;
    int n = tags ? env->GetArrayLength(tags) : 0;
    if (n > 48) n = 48;
    const char *ptrs[48]; jstring strs[48];
    for (int i = 0; i < n; i++) {
        strs[i] = (jstring)env->GetObjectArrayElement(tags, i);
        ptrs[i] = env->GetStringUTFChars(strs[i], nullptr);
    }
    SteamParamStringArray_t a{ptrs, n};
    bool r = vcall<bool, uint64_t, const SteamParamStringArray_t *, bool>(UGC(), 51, ws->updateHandle, &a, false);
    for (int i = 0; i < n; i++) { env->ReleaseStringUTFChars(strs[i], ptrs[i]); env->DeleteLocalRef(strs[i]); }
    return r;
}

JNIFN(jboolean, SteamWorkshop, n_1SubmitItemUpdate)(JNIEnv *env, jobject, jstring note) {
    WS_GUARD(false);
    if (!updateReady(env)) return false;
    char *p = GetStandardUTFChars(env, note);
    uint64_t call = vcall<uint64_t, uint64_t, const char *>(UGC(), 66, ws->updateHandle, p);
    ws->submitCall = call;
    sc::bindCallResult(call, 3404, g_bServer, [](void *d, bool) {
        auto *r = (SubmitItemUpdateResult_t *)d;
        if (!ws) return;
        ws->createdFileId = 0; ws->updateHandle = ~0ull;
        if (r->eResult == 1) WSCALL(updated, (jboolean)r->needsLegal);
        else WSCALL(notUpdated, (jint)r->eResult);
    });
    ws->updateStarted = false; ws->updateSubmitted = true;
    ReleaseStandardUTFChars(p);
    return true;
}

JNIFN(jboolean, SteamWorkshop, n_1GetItemUpdateProgress)(JNIEnv *env, jobject, jlongArray arr) {
    WS_GUARD(false);
    if (!arr) { ThrowJava(env, "java/lang/NullPointerException", "bytes is NULL"); return false; }
    if (env->GetArrayLength(arr) != 2) { ThrowJava(env, "java/lang/IllegalArgumentException", "bytes should be long[2]"); return false; }
    if (ws->updateHandle == ~0ull || !ws->updateSubmitted) { ThrowJava(env, "java/lang/RuntimeException", "item update wasn't submitted"); return false; }
    uint64_t done = 0, total = 0;
    int st = vcall<int, uint64_t, uint64_t *, uint64_t *>(UGC(), 67, ws->updateHandle, &done, &total);
    if (st == 0) return false;
    jlong v[2] = {(jlong)done, (jlong)total};
    env->SetLongArrayRegion(arr, 0, 2, v);
    return true;
}

JNIFN(jlong, SteamWorkshop, n_1GetItemState)(JNIEnv *env, jobject, jlong id) {
    WS_GUARD(0);
    return (jlong)vcall<uint32_t, uint64_t>(UGC(), 76, (uint64_t)id);
}

JNIFN(jboolean, SteamWorkshop, n_1SubscribeItem)(JNIEnv *env, jobject, jlong id) {
    WS_GUARD(false);
    if (ws->pendingSubscribe) { ThrowJava(env, "java/lang/RuntimeException", "subscribe in progress already"); return false; }
    uint64_t call = vcall<uint64_t, uint64_t>(UGC(), 72, (uint64_t)id);
    ws->subscribeCall = call;
    sc::bindCallResult(call, 1313, g_bServer, [](void *d, bool) {
        auto *r = (SubscribeResult_t *)d;
        if (!ws) return;
        if (r->fileId != ws->pendingSubscribe) { sc::log(3, "RemoteStorageSubscribePublishedFileResult_t for fileId=%llu ignored\n", (unsigned long long)r->fileId); return; }
        ws->pendingSubscribe = 0;
        if (r->eResult == 1) WSCALL(subscribed, (jlong)r->fileId);
        else WSCALL(notSubscribed, (jlong)r->fileId, (jint)r->eResult);
    });
    ws->pendingSubscribe = (uint64_t)id;
    return true;
}

JNIFN(jboolean, SteamWorkshop, n_1DownloadItem)(JNIEnv *env, jobject, jlong id, jboolean high) {
    WS_GUARD(false);
    if (ws->pendingDownload) { ThrowJava(env, "java/lang/RuntimeException", "download in progress already"); return false; }
    if (!vcall<bool, uint64_t, bool>(UGC(), 79, (uint64_t)id, high == 1)) return false;
    ws->pendingDownload = (uint64_t)id;
    return true;
}

static bool installInfo(uint64_t id, char *folder, uint32_t *ts) {
    uint64_t size = 0;
    return vcall<bool, uint64_t, uint64_t *, char *, uint32_t, uint32_t *>(UGC(), 77, id, &size, folder, 0x400, ts);
}

JNIFN(jstring, SteamWorkshop, n_1GetItemInstallFolder)(JNIEnv *env, jobject, jlong id) {
    WS_GUARD(nullptr);
    char folder[1024] = {0}; uint32_t ts = 0;
    return installInfo((uint64_t)id, folder, &ts) ? env->NewStringUTF(folder) : nullptr;
}
JNIFN(jlong, SteamWorkshop, n_1GetItemInstallTimeStamp)(JNIEnv *env, jobject, jlong id) {
    WS_GUARD(0);
    char folder[1024] = {0}; uint32_t ts = 0;
    return installInfo((uint64_t)id, folder, &ts) ? (jlong)ts : 0;
}
JNIFN(jboolean, SteamWorkshop, n_1GetItemDownloadInfo)(JNIEnv *env, jobject, jlong id, jlongArray arr) {
    WS_GUARD(false);
    if (!arr) { ThrowJava(env, "java/lang/NullPointerException", "progress is NULL"); return false; }
    if (env->GetArrayLength(arr) != 2) { ThrowJava(env, "java/lang/IllegalArgumentException", "progress should be long[2]"); return false; }
    uint64_t done = 0, total = 0;
    if (!vcall<bool, uint64_t, uint64_t *, uint64_t *>(UGC(), 78, (uint64_t)id, &done, &total)) return false;
    jlong v[2] = {(jlong)done, (jlong)total};
    env->SetLongArrayRegion(arr, 0, 2, v);
    return true;
}

JNIFN(jlong, SteamWorkshop, n_1CreateQueryUGCDetailsRequest)(JNIEnv *env, jobject, jlongArray ids) {
    WS_GUARD(0);
    if (!ids) { ThrowJava(env, "java/lang/NullPointerException", "itemIDs is NULL"); return 0; }
    jsize n = env->GetArrayLength(ids);
    std::vector<uint64_t> v(n ? n : 1);
    env->GetLongArrayRegion(ids, 0, n, (jlong *)v.data());
    uint64_t q = vcall<uint64_t, uint64_t *, uint32_t>(UGC(), 3, v.data(), (uint32_t)n);
    if (q == ~0ull) return 0;
    uint64_t call = vcall<uint64_t, uint64_t>(UGC(), 4, q);
    sc::bindCallResult(call, 3401, g_bServer, [](void *d, bool) {
        auto *r = (SteamUGCQueryCompleted_t *)d;
        if (r->eResult == 1) WSCALL(queryDone, (jlong)r->handle, (jint)r->numResults);
        else WSCALL(queryFailed, (jlong)r->handle, (jint)r->eResult);
    });
    return (jlong)q;
}

static jlongArray children(JNIEnv *env, uint64_t q, uint32_t idx, uint32_t n) {
    std::vector<uint64_t> buf(n);
    if (!vcall<bool, uint64_t, uint32_t, uint64_t *, uint32_t>(UGC(), 11, q, idx, buf.data(), n)) return nullptr;
    jlongArray a = env->NewLongArray((jsize)n);
    if (a) env->SetLongArrayRegion(a, 0, (jsize)n, (const jlong *)buf.data());
    return a;
}

JNIFN(jobject, SteamWorkshop, n_1GetQueryUGCResult)(JNIEnv *env, jobject, jlong q, jint idx) {
    WS_GUARD(nullptr);
    auto *d = (SteamUGCDetails_t *)calloc(1, sizeof(SteamUGCDetails_t));
    jobject res = nullptr;
    if (vcall<bool, uint64_t, uint32_t, SteamUGCDetails_t *>(UGC(), 5, (uint64_t)q, (uint32_t)idx, d)) {
        jclass cls = env->FindClass("zombie/core/znet/SteamUGCDetails");
        if (cls) {
            jlongArray ch = d->numChildren ? children(env, (uint64_t)q, (uint32_t)idx, d->numChildren) : nullptr;
            jmethodID ctor = env->GetMethodID(cls, "<init>", "(JLjava/lang/String;JJI[J)V");
            res = env->NewObject(cls, ctor, (jlong)d->fileId, env->NewStringUTF(d->title), (jlong)d->created,
                                 (jlong)d->updated, (jint)d->fileSize, ch);
        }
    }
    free(d);
    return res;
}

JNIFN(jlongArray, SteamWorkshop, n_1GetQueryUGCChildren)(JNIEnv *env, jobject, jlong q, jint idx) {
    WS_GUARD(nullptr);
    auto *d = (SteamUGCDetails_t *)calloc(1, sizeof(SteamUGCDetails_t));
    jlongArray r = nullptr;
    if (vcall<bool, uint64_t, uint32_t, SteamUGCDetails_t *>(UGC(), 5, (uint64_t)q, (uint32_t)idx, d) && d->numChildren)
        r = children(env, (uint64_t)q, (uint32_t)idx, d->numChildren);
    free(d);
    return r;
}

JNIFN(jboolean, SteamWorkshop, n_1ReleaseQueryUGCRequest)(JNIEnv *env, jobject, jlong q) {
    WS_GUARD(false);
    return vcall<bool, uint64_t>(UGC(), 21, (uint64_t)q);
}

JNIFN(jobjectArray, SteamWorkshop, n_1GetInstalledItemFolders)(JNIEnv *env, jobject) {
    WS_GUARD(nullptr);
    if (g_bServer) { ThrowJava(env, "java/lang/IllegalStateException", "not available to SteamGameServer"); return nullptr; }
    void *ugc = sc::iface("STEAMUGC_INTERFACE_VERSION021");
    uint32_t n = vcall<uint32_t, bool>(ugc, 74, false);
    if (!n) return nullptr;
    std::vector<uint64_t> ids(n);
    vcall<uint32_t, uint64_t *, uint32_t, bool>(ugc, 75, ids.data(), n, false);
    std::vector<std::string> folders;
    for (uint64_t id : ids) {
        uint32_t st = vcall<uint32_t, uint64_t>(ugc, 76, id);
        if (!(st & 4)) { sc::log(3, "workshop item %llu is not installed\n", (unsigned long long)id); continue; }
        char folder[1024] = {0}; uint32_t ts = 0;
        if (installInfo(id, folder, &ts)) folders.push_back(folder);
        else sc::log(3, "GetItemInstallInfo() failed for workshop item %llu\n", (unsigned long long)id);
    }
    if (folders.empty()) return nullptr;
    jobjectArray a = env->NewObjectArray((jsize)folders.size(), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < folders.size(); i++) env->SetObjectArrayElement(a, (jsize)i, env->NewStringUTF(folders[i].c_str()));
    return a;
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    sc::jvm = vm;
    return JNI_VERSION_1_6;
}
