// RakNetPeerInterface Steam-mode overrides + ZNetClient/ZNetServer.
// Everything not overridden here is served by libZNetNoSteam64.so (our DT_NEEDED).
#include <arpa/inet.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "raknet_abi.h"
#include "znet.h"

using sc::vcall;
using rn::SystemAddress;

static const uint32_t MAGIC = 0xFEE1DEAD;
enum { MSG_VERSION = 1, MSG_SERVERINFO = 101, MSG_AUTHFAILED = 102, MSG_AUTHPASSED = 103, MSG_BEGINAUTH = 201 };

static rn::Peer peer() { return rn::Peer{g_peer}; }

// ------------------------------------------------------------------ Steam interface helpers
static void *cUser() { return sc::iface("SteamUser023"); }
static void *cNet() { return sc::iface("SteamNetworking006"); }
static void *cMMS() { return sc::iface("SteamMatchMakingServers002"); }
void *gsServer() { return sc::gsIface("SteamGameServer015"); }
static void *gsNet() { return sc::gsIface("SteamNetworking006"); }

static bool sendP2P(void *net, uint64_t id, const void *d, uint32_t n, int type) {
    return net && vcall<bool, uint64_t, const void *, uint32_t, int, int>(net, 0, id, d, n, type, 0);
}
static void closeP2P(void *net, uint64_t id) { if (net && id) vcall<bool, uint64_t>(net, 4, id); }

// ------------------------------------------------------------------ original (NoSteam) JNI entry points
static void *noSteam() {
    static void *h;
    if (!h) h = dlopen("libZNetNoSteam64.so", RTLD_NOW | RTLD_NOLOAD);
    return h;
}
#define ORIG(sym, ...) ((__VA_ARGS__)dlsym(noSteam(), #sym))
#define RNPI(name) Java_zombie_core_raknet_RakNetPeerInterface_##name

// ------------------------------------------------------------------ global state
static bool g_steamMode;
static int g_networkState;  // 2 = Steam server, 3 = Steam client
static int clientPort, serverPort, serverUDPPort;
static rn::SocketDescriptor sd[2];
static jobject g_cbObj;
static jmethodID g_cbMid;

static const char *stateName(int s) {
    static const char *n[] = {"Disconnected", "GettingServerInfo", "UDPConnecting", "SteamConnecting", "AuthPending",
                              "ConnectionPending", "Connected", "Disconnecting", "Failed", "ClientVersionMismatch",
                              "InvalidServerPassword"};
    return (s >= 0 && s <= 10) ? n[s] : "???";
}

static bool steamIdValid(uint64_t id) {
    unsigned universe = id >> 56, type = (id >> 52) & 15, inst = (id >> 32) & 0xFFFFF;
    uint32_t acct = (uint32_t)id;
    if (universe < 1 || universe > 4 || type < 1 || type > 10) return false;
    if (type == 1 && (acct == 0 || inst != 1)) return false;
    if (type == 3 && acct == 0) return false;
    if (type == 7 && (acct == 0 || inst != 0)) return false;
    return true;
}

static void putHdr(uint8_t *b, uint32_t type) { memcpy(b, &MAGIC, 4); memcpy(b + 4, &type, 4); }
static bool isMsg(const uint8_t *b, size_t n, uint32_t *type) {
    if (n < 8) return false;
    uint32_t m; memcpy(&m, b, 4);
    if (m != MAGIC) return false;
    memcpy(type, b + 4, 4);
    return true;
}

// ================================================================== ZNetClient
struct PingResponse {   // ISteamMatchmakingPingResponse: [ServerResponded, ServerFailedToRespond], no dtor slot
    virtual void ServerResponded(const uint8_t *item) = 0;
    virtual void ServerFailedToRespond() = 0;
};

struct QueueEntry { int type; SystemAddress addr; };  // 1 = Steam, 2 = UDP

class ZNetClient : public rn::Plugin, public rn::SocketOverride, public PingResponse {
public:
    std::recursive_mutex mu;
    std::string version, stateMessage, password;
    int state = 0;
    bool stateChanged = false, relay = false;
    uint64_t serverID = 0;
    uint32_t serverIP = 0, serverIPType = 0;  // ServerInfo public IP, host order
    int hQuery = -1;
    uint32_t hAuth = 0;
    SystemAddress serverAddr = rn::addrFrom(0, 0);
    uint16_t sockIdx = 0;
    QueueEntry queue[5]{};
    uint8_t cursor = 0xFF, count = 0, versionAttempts = 0;
    uint8_t auth[1300]{};

    ZNetClient() { putHdr(auth, MSG_BEGINAUTH); }

    void UpdateConnectionState(int s, const std::string &msg = "") {
        std::lock_guard<std::recursive_mutex> g(mu);
        if ((state == 9 || state == 10) && s == 8) { sc::log(2, "UpdateConnectionState: keeping %s\n", stateName(state)); return; }
        if (s != state) { state = s; stateChanged = true; stateMessage = msg; }
    }
    void CancelAuth() {
        if (hAuth && cUser()) { vcall<void, uint32_t>(cUser(), 17, hAuth); }
        hAuth = 0;
    }
    void CancelQuery() {
        if (hQuery != -1 && cMMS()) vcall<void, int>(cMMS(), 16, hQuery);
        hQuery = -1;
    }
    void Abort(const char *why) {
        CancelQuery();
        peer().CloseConnection(rn::aog(serverAddr), true, (uint8_t)sockIdx, rn::HIGH_PRIORITY);
        UpdateConnectionState(7);
        sc::log(2, "CloseConnection: %s\n", why);
        CancelAuth();
        closeP2P(cNet(), serverID);
    }
    void SendVersion() {
        uint8_t m[24] = {0};
        putHdr(m, MSG_VERSION);
        strncpy((char *)m + 8, version.c_str(), 16);
        sendP2P(cNet(), serverID, m, sizeof m, 2 /*reliable*/);
        versionAttempts++;
    }
    bool StartP2P(uint64_t id, const std::string &pw, bool rly) {
        if (!steamIdValid(id)) { sc::log(3, "Connection attempt failed because SteamID is invalid\n"); return false; }
        if (state != 0) Abort("Server ID is valid. Initialization (1202)");
        serverID = id; serverIP = 0; serverIPType = 0; relay = rly; password = pw; versionAttempts = 0;
        cursor = 0xFF; count = 0;
        UpdateConnectionState(1);
        SendVersion();
        return true;
    }
    bool DoNextConnect() {
        cursor++;
        if (cursor > 4 || cursor >= count || queue[cursor].type == 0) {
            if (state != 8 && state != 9 && state != 10) UpdateConnectionState(8);
            return false;
        }
        QueueEntry &e = queue[cursor];
        bool udp = e.type == 2;
        UpdateConnectionState(udp ? 2 : 3);
        serverAddr = e.addr;
        sockIdx = udp ? 1 : 0;
        char ip[32]; inet_ntop(AF_INET, &e.addr.addr, ip, sizeof ip);
        uint16_t port = ntohs(e.addr.port);
        sc::log(1, "%s connect to %s:%u\n", udp ? "UDP" : "Steam", ip, port);
        int r = peer().Connect(ip, port, password.data(), (int)password.size(), sockIdx);
        if (r != 0) {
            sc::log(3, "Connect failed %s (%d)\n", ip, r);
            if (state != 0) Abort("Connect failed (1203)");
            return false;
        }
        return true;
    }
    void OnServerInfo(const uint8_t *b, uint32_t n) {
        if (n < 0xB0) return;
        memcpy(&serverIP, b + 8, 4); memcpy(&serverIPType, b + 0x18, 4);
        char name[129] = {0}; memcpy(name, b + 0x1c, 128);
        uint16_t gp = (uint16_t)(b[0xAC] << 8 | b[0xAD]), up = (uint16_t)(b[0xAE] << 8 | b[0xAF]);
        uint32_t pub = htonl(serverIP), typed = serverAddr.addr;
        sc::log(1, "Got Server Info Message: %s (public %u.%u.%u.%u, game %u, udp %u)\n", name,
                serverIP >> 24, (serverIP >> 16) & 255, (serverIP >> 8) & 255, serverIP & 255, gp, up);
        count = 0; cursor = 0xFF;
        auto add = [&](int type, uint32_t ip, uint16_t port) { if (count < 5) queue[count++] = {type, rn::addrFrom(ip, port)}; };
        if (!relay) {
            if (typed) add(2, typed, up);
            if (pub != typed) add(2, pub, up);
            if (!typed) add(2, htonl(0x7f000001), up);
        }
        add(1, pub, gp);
        DoNextConnect();
    }

    // ---- SocketLayerOverride (socket 0 = Steam P2P to the server)
    int RakNetSendTo(const char *d, int len, const SystemAddress &to) override {
        if (to.addr != serverAddr.addr) return -1;
        return sendP2P(cNet(), serverID, d, (uint32_t)len, 0) ? len : 0;
    }
    int RakNetRecvFrom(char *buf, SystemAddress *from, bool) override {
        void *net = cNet();
        if (!net) return 0;
        uint32_t sz = 0; uint64_t remote = 0;
        if (!vcall<bool, uint32_t *, int>(net, 1, &sz, 0)) return 0;
        if (!vcall<bool, void *, uint32_t, uint32_t *, uint64_t *, int>(net, 2, buf, 1492, &sz, &remote, 0)) return 0;
        if (remote != serverID) return 0;
        uint32_t type;
        if (sz > 8 && isMsg((uint8_t *)buf, sz, &type)) {
            if (type == MSG_VERSION && strncmp(buf + 8, version.c_str(), version.size()) != 0) {
                char v[17] = {0}; memcpy(v, buf + 8, 16);
                sc::log(3, "Server version mismatch: %s\n", v);
                UpdateConnectionState(9, v);
            } else if (type == MSG_SERVERINFO) {
                OnServerInfo((uint8_t *)buf, sz);
            }
        }
        *from = serverAddr;
        return (int)sz;
    }

    // ---- PluginInterface2
    void OnRakPeerStartup() override {
        rn::List<void *> l{};
        peer().GetSockets(l);
        if (l.list_size > 0) ((RakNet::RNS2_Linux *)l.listArray[0])->SetSocketLayerOverride((RakNet::SocketLayerOverride *)(rn::SocketOverride *)this);
        free(l.listArray);
    }
    int OnReceive(rn::Packet *p) override {
        uint32_t type;
        if (!isMsg(p->data, p->length, &type)) return rn::RR_CONTINUE_PROCESSING;
        if (type == MSG_AUTHFAILED && p->length >= 10) {
            uint8_t reason = p->data[8], detail = p->data[9];
            const char *s = reason == 1 ? "MessageTooSmall" : reason == 2 ? "InvalidServerPassword"
                          : reason == 3 ? (detail == 2 ? "NotInvited" : "SteamIDBanned")
                          : reason == 4 ? "BeginAuthSession" : reason == 5 ? "AlreadyAuthenticated" : "Unknown";
            sc::log(3, "ServerAuthFailed %d/%d: %s\n", reason, detail, s);
            UpdateConnectionState(8, s);
            CancelAuth();
            closeP2P(cNet(), serverID);
        } else if (type == MSG_AUTHPASSED) {
            sc::log(1, "Successfully authenticated\n");
            UpdateConnectionState(6);
        }
        return rn::RR_STOP_PROCESSING_AND_DEALLOCATE;
    }
    void OnNewConnection(const SystemAddress &a, rn::RakNetGUID, bool) override {
        if (!rn::sameAddr(a, serverAddr)) { sc::log(2, "Bogus 'new connection'\n"); return; }
        UpdateConnectionState(5);
        UpdateConnectionState(4);
        uint64_t me = cUser() ? vcall<uint64_t>(cUser(), 2) : 0;
        memcpy(auth + 8, &me, 8);
        memset(auth + 0x10, 0, 256);
        strncpy((char *)auth + 0x10, password.c_str(), 256);
        memset(auth + 0x110, 0, 4 + 1024);
        CancelAuth();
        if (cUser()) hAuth = vcall<uint32_t, void *, int, uint32_t *, const void *>(cUser(), 13, auth + 0x114, 1024, (uint32_t *)(auth + 0x110), nullptr);
        sc::log(1, "Requested auth ticket %u\n", hAuth);
    }
    void OnClosedConnection(const SystemAddress &a, rn::RakNetGUID, int reason) override {
        if (state == 4 && DoNextConnect()) return;
        if (rn::sameAddr(a, serverAddr)) {
            sc::log(2, "CloseConnection: reason %d (1208)\n", reason);
            CancelAuth();
            closeP2P(cNet(), serverID);
            serverID = 0;
            UpdateConnectionState(0);
        } else sc::log(2, "Bogus 'connection closed' notification\n");
    }
    void OnFailedConnectionAttempt(rn::Packet *, int reason) override {
        if (reason == 5) UpdateConnectionState(10);
        else DoNextConnect();
    }

    // ---- ISteamMatchmakingPingResponse
    void ServerResponded(const uint8_t *item) override {
        hQuery = -1;
        if (state != 0) return;
        uint64_t id; memcpy(&id, item + 0x16c, 8);
        StartP2P(id, password, relay);
    }
    void ServerFailedToRespond() override {
        hQuery = -1;
        sc::log(3, "Failed to retrieve server information by its IP\n");
        UpdateConnectionState(8, "ServerFailedToRespond");
    }

    void OnTicket(uint32_t h, int result) {
        if (result != 1) { sc::log(3, "GetAuthSessionTicket failed: %d\n", result); return; }
        if (h != hAuth) sc::log(3, "Incorect auth ticket\n");
        peer().Send((const char *)auth, sizeof auth, rn::HIGH_PRIORITY, rn::RELIABLE, 0, rn::aog(serverAddr), false);
        sc::log(1, "Sent ClientBeginAuth (ticket %u bytes)\n", *(uint32_t *)(auth + 0x110));
    }
    void OnP2PConnectFail(uint64_t remote, uint8_t err) {
        if (state == 1 && versionAttempts <= 3 && err == 4) { SendVersion(); return; }
        if (remote == serverID) {
            sc::log(3, "CloseConnection: P2PSessionConnectFail %d (1207)\n", err);
            CancelAuth();
            closeP2P(cNet(), serverID);
            UpdateConnectionState(8, "P2PSessionConnectFail");
        }
    }

    // Java-thread pump (UpdateSteamClientAndServer).
    void Pump() {
        int s; std::string msg; bool changed;
        {
            std::lock_guard<std::recursive_mutex> g(mu);
            if (state == 7) {
                int cs = peer().GetConnectionState(rn::aog(serverAddr));
                if (cs == 5 || cs == 6) { serverID = 0; UpdateConnectionState(0); }
            }
            changed = stateChanged; stateChanged = false; s = state; msg = stateMessage;
        }
        if (!changed) return;
        if (s == 8 || s == 9) {
            rn::Packet *p = peer().AllocatePacket(s == 8 ? 1 : 19);
            if (p) {
                if (s == 8) { p->data[0] = rn::ID_CONNECTION_ATTEMPT_FAILED; p->bitSize = 8; }
                else {
                    memset(p->data, 0, 19);
                    p->data[0] = rn::ID_INCOMPATIBLE_PROTOCOL_VERSION;
                    size_t n = msg.size() > 16 ? 16 : msg.size();
                    p->data[2] = (uint8_t)n; memcpy(p->data + 3, msg.data(), n);
                    p->bitSize = 152;
                }
                p->systemAddress = rn::addrFrom(0, 0);
                peer().PushBackPacket(p, false);
            }
        }
        JNIEnv *e = sc::env();
        if (e && g_cbObj && g_cbMid) {
            jstring a = e->NewStringUTF(stateName(s)), b = e->NewStringUTF(msg.c_str());
            e->CallVoidMethod(g_cbObj, g_cbMid, a, b);
            if (e->ExceptionCheck()) { e->ExceptionDescribe(); e->ExceptionClear(); }
            e->DeleteLocalRef(a); e->DeleteLocalRef(b);
        }
    }
};
static ZNetClient *g_steamClient;

// ================================================================== ZNetServer
struct ClientConn {
    int connType = 0;           // 0 none, 1 Steam, 2 UDP
    uint64_t steamID = 0, owner = 0;
    SystemAddress addr{};
    int state = 0;              // 1 accepted, 2 version ok, 3 authenticated, 4 closing
    uint32_t closeTime = 0;
};
struct Player { std::string name; int score = 0; time_t changed = 0; bool active = false; };

class ZNetServer : public rn::Plugin, public rn::SocketOverride {
public:
    std::string version, password;
    uint16_t gamePort = 0, udpPort = 0;
    uint8_t publicIP[20]{};     // SteamIPAddress_t
    int maxPlayers = 0, connectState = 0, playerCount = 0;
    std::recursive_mutex clientsLock, playersLock;
    std::vector<ClientConn> clients;
    Player players[512];
    void *socket0 = nullptr;

    void sendTo(const SystemAddress &a, const uint8_t *d, int n) {
        peer().Send((const char *)d, n, rn::HIGH_PRIORITY, rn::RELIABLE, 0, rn::aog(a), false);
    }
    void authFailed(const SystemAddress &a, uint8_t reason, uint8_t detail) {
        uint8_t m[10]; putHdr(m, MSG_AUTHFAILED); m[8] = reason; m[9] = detail;
        sc::log(3, "ServerAuthFailed reason=%d detail=%d\n", reason, detail);
        sendTo(a, m, sizeof m);
    }
    ClientConn *byId(uint64_t id) { for (auto &c : clients) if (c.steamID == id) return &c; return nullptr; }
    ClientConn *byAddr(const SystemAddress &a) { for (auto &c : clients) if (rn::sameAddr(c.addr, a)) return &c; return nullptr; }
    void closing(ClientConn &c) { c.state = 4; c.closeTime = RakNet::GetTimeMS(); }

    void OnRakPeerStartup() override {
        rn::List<void *> l{};
        peer().GetSockets(l);
        if (l.list_size > 0) {
            socket0 = l.listArray[0];
            if (void *nu = sc::gsIfaceUser0("SteamNetworkingUtils004")) vcall<bool, float>(nu, 7, 1e10f);  // InitRelayNetworkAccess
            ((RakNet::RNS2_Linux *)socket0)->SetSocketLayerOverride((RakNet::SocketLayerOverride *)(rn::SocketOverride *)this);
        }
        free(l.listArray);
    }
    int RakNetSendTo(const char *d, int len, const SystemAddress &to) override {
        if (len > 4 && !memcmp(d, "\xFF\xFF\xFF\xFF", 4)) return -1;
        std::lock_guard<std::recursive_mutex> g(clientsLock);
        ClientConn *c = byAddr(to);
        if (c && (c->state == 2 || c->state == 3)) {
            if (sendP2P(gsNet(), c->steamID, d, (uint32_t)len, 0)) return len;
            sc::log(3, "RakNetSendTo error\n");
        }
        return 0;
    }
    int RakNetRecvFrom(char *buf, SystemAddress *from, bool) override {
        void *net = gsNet();
        if (!net) return 0;
        uint32_t sz = 0; uint64_t remote = 0;
        if (!vcall<bool, uint32_t *, int>(net, 1, &sz, 0)) return 0;
        if (!vcall<bool, void *, uint32_t, uint32_t *, uint64_t *, int>(net, 2, buf, 1492, &sz, &remote, 0)) return 0;
        std::lock_guard<std::recursive_mutex> g(clientsLock);
        uint32_t type;
        if (sz > 8 && isMsg((uint8_t *)buf, sz, &type) && type == MSG_VERSION) {
            ClientConn *c = byId(remote);
            char v[17] = {0}; memcpy(v, buf + 8, 16);
            if (!strcmp(v, version.c_str())) {
                uint8_t m[0xB0] = {0};
                putHdr(m, MSG_SERVERINFO);
                memcpy(m + 8, publicIP, 20);
                strcpy((char *)m + 0x1c, "Zomboid Server");
                strncpy((char *)m + 0x9c, version.empty() ? "UNVERSIONED" : version.c_str(), 16);
                m[0xAC] = gamePort >> 8; m[0xAD] = gamePort & 255; m[0xAE] = udpPort >> 8; m[0xAF] = udpPort & 255;
                sendP2P(net, remote, m, sizeof m, 2);
                if (c) c->state = 2;
            } else {
                sc::log(3, "Client version %s != server %s\n", v, version.c_str());
                uint8_t m[24] = {0}; putHdr(m, MSG_VERSION); strncpy((char *)m + 8, version.c_str(), 16);
                sendP2P(net, remote, m, sizeof m, 2);
                if (c) closing(*c);
            }
            return 0;
        }
        ClientConn *c = byId(remote);
        if (!c) { sc::log(2, "Unknown sender %llu\n", (unsigned long long)remote); return 0; }
        if (c->state == 2 || c->state == 3) { *from = c->addr; return (int)sz; }
        if (c->state != 4) { sc::log(2, "Disconnect: Unknown sender (1101)\n"); closing(*c); }
        return 0;
    }

    // A2S: let Steam answer server queries that RakNet passes up (FF FF FF FF ...).
    void HandleQuery(rn::Packet *p) {
        void *gs = gsServer();
        if (!gs || !socket0) return;
        vcall<bool, const void *, int, uint32_t, uint16_t>(gs, 34, p->data, (int)p->length, ntohl(p->systemAddress.addr), ntohs(p->systemAddress.port));
        static uint8_t out[0x4000];
        uint32_t ip; uint16_t port; int n;
        while ((n = vcall<int, void *, int, uint32_t *, uint16_t *>(gs, 35, out, sizeof out, &ip, &port)) > 0) {
            if (n > 5 && out[4] == 'I') patchInfo(out, n);
            if (n > 5 && out[4] == 'D') n = playerList(out, sizeof out);
            struct { char *data; int length; SystemAddress addr; int ttl; } sp{(char *)out, n, rn::addrFrom(htonl(ip), port), 0};
            vcall<int, void *, const char *, unsigned>(socket0, 2, &sp, "ZNetServer.cpp", __LINE__);
        }
    }
    void patchInfo(uint8_t *b, int n) {  // header(4) 'I' proto name\0 map\0 folder\0 game\0 id(2) players
        int i = 6;
        for (int s = 0; s < 4 && i < n; s++) { while (i < n && b[i]) i++; i++; }
        i += 2;
        if (i < n) b[i] = (uint8_t)playerCount;
    }
    int playerList(uint8_t *b, int cap) {
        std::lock_guard<std::recursive_mutex> g(playersLock);
        int i = 0; memcpy(b, "\xFF\xFF\xFF\xFF" "D", 5); i = 6;
        uint8_t cnt = 0; time_t now = time(nullptr);
        for (int k = 0; k < 512 && cnt < 255; k++) {
            Player &pl = players[k];
            if (!pl.active || i + (int)pl.name.size() + 10 > cap) continue;
            b[i++] = cnt; memcpy(b + i, pl.name.c_str(), pl.name.size() + 1); i += pl.name.size() + 1;
            int32_t sc_ = pl.score; memcpy(b + i, &sc_, 4); i += 4;
            float dur = (float)(now - pl.changed); memcpy(b + i, &dur, 4); i += 4;
            cnt++;
        }
        b[5] = cnt;
        return i;
    }

    int OnReceive(rn::Packet *p) override {
        if (p->length > 4 && !memcmp(p->data, "\xFF\xFF\xFF\xFF", 4)) { HandleQuery(p); return rn::RR_STOP_PROCESSING_AND_DEALLOCATE; }
        uint32_t type;
        if (!isMsg(p->data, p->length, &type)) return rn::RR_CONTINUE_PROCESSING;
        if (type == MSG_BEGINAUTH) OnClientBeginAuth(p->systemAddress, p->data, p->length);
        else sc::log(3, "Wrong message type %u\n", type);
        return rn::RR_STOP_PROCESSING_AND_DEALLOCATE;
    }
    void OnClientBeginAuth(const SystemAddress &a, const uint8_t *d, uint32_t n) {
        if (n < 1300) { authFailed(a, 1, 0xC9); return; }
        uint64_t id; memcpy(&id, d + 8, 8);
        char pw[257] = {0}; memcpy(pw, d + 0x10, 256);
        if (!password.empty() && password != pw) { authFailed(a, 2, 0); return; }
        int r = SteamUtils_ClientInitiateConnectionCallback(id);
        if (r != 0) { authFailed(a, 3, (uint8_t)r); return; }
        std::lock_guard<std::recursive_mutex> g(clientsLock);
        ClientConn *c = byId(id);
        if (!c || c->state != 2) {
            // Direct-UDP clients never opened a P2P session; track them by address.
            clients.push_back(ClientConn{}); c = &clients.back(); c->steamID = id; c->state = 2;
        }
        uint32_t len; memcpy(&len, d + 0x110, 4);
        if (len > 1024) len = 1024;
        int br = gsServer() ? vcall<int, const void *, int, uint64_t>(gsServer(), 26, d + 0x114, (int)len, id) : 1;
        if (br != 0) { authFailed(a, 4, (uint8_t)br); closing(*c); return; }
        c->addr = a;
        c->connType = (a.port == htons(0x0100)) ? 1 : 2;  // fake P2P address has raw port bytes 00 01
        sc::log(1, "BeginAuthSession ok for %llu (%s)\n", (unsigned long long)id, c->connType == 1 ? "Steam" : "UDP");
    }
    void OnValidateAuth(uint64_t id, int resp, uint64_t owner) {
        std::lock_guard<std::recursive_mutex> g(clientsLock);
        ClientConn *c = byId(id);
        if (!c) return;
        if (resp == 0) {
            if (owner != id) {
                sc::log(2, "Game is borrowed from another account %llu\n", (unsigned long long)owner);
                int r = SteamUtils_ValidateOwnerCallback(id, owner);
                if (r != 0) { authFailed(c->addr, 3, (uint8_t)r); closing(*c); return; }
            }
            c->state = 3; c->owner = owner;
            uint8_t m[8]; putHdr(m, MSG_AUTHPASSED);
            sendTo(c->addr, m, sizeof m);
            sc::log(1, "Auth succeeded for %llu\n", (unsigned long long)id);
        } else {
            authFailed(c->addr, 6, (uint8_t)resp);
            closing(*c);
        }
    }
    void OnP2PSessionRequest(uint64_t remote) {
        {
            std::lock_guard<std::recursive_mutex> g(clientsLock);
            ClientConn c; c.steamID = remote; c.state = 1;
            c.addr = rn::addrFrom((uint32_t)RakNet::RakPeerInterface::Get64BitUniqueRandomNumber(), 0x0100);  // raw port bytes 00 01
            clients.push_back(c);
        }
        if (gsNet()) vcall<bool, uint64_t>(gsNet(), 3, remote);
    }
    void OnClosedConnection(const SystemAddress &a, rn::RakNetGUID, int reason) override {
        std::lock_guard<std::recursive_mutex> g(clientsLock);
        ClientConn *c = byAddr(a);
        if (c && c->state != 4) { sc::log(2, "Disconnect: reason %d (1105)\n", reason); closing(*c); }
    }
    void Update() {
        {
            std::lock_guard<std::recursive_mutex> g(clientsLock);
            uint32_t now = RakNet::GetTimeMS();
            for (size_t i = 0; i < clients.size();) {
                ClientConn &c = clients[i];
                if (c.state == 4 && c.closeTime + 1000 < now) {
                    sc::log(1, "Finally disconnected client %llu\n", (unsigned long long)c.steamID);
                    closeP2P(gsNet(), c.steamID);
                    if (gsServer()) vcall<void, uint64_t>(gsServer(), 27, c.steamID);  // EndAuthSession
                    clients.erase(clients.begin() + i);
                } else i++;
            }
        }
        sc::runFrameServer();
    }
};
ZNetServer *g_znetServer;
void GameServerShutdown();

// ------------------------------------------------------------------ hooks used by gameserver.cpp
ZNetServer *ZNetServer_Create(uint16_t gamePort, uint16_t udpPort) {
    auto *s = new ZNetServer(); s->gamePort = gamePort; s->udpPort = udpPort; g_znetServer = s; return s;
}
void ZNetServer_SetMaxPlayers(int n) { if (g_znetServer) g_znetServer->maxPlayers = n; }
int ZNetServer_ConnectState() { return g_znetServer ? g_znetServer->connectState : -1; }
bool ZNetServer_Exists() { return g_znetServer != nullptr; }
void ZNetServer_AddPlayer(int idx, const char *name, int score) {
    if (!g_znetServer || idx < 0 || idx > 511) return;
    std::lock_guard<std::recursive_mutex> g(g_znetServer->playersLock);
    Player &p = g_znetServer->players[idx];
    if (p.active) { sc::log(4, "AddPlayer: player-id=%d, active=1, skip\n", idx); return; }
    p.name = name; p.score = score; p.changed = time(nullptr); p.active = true; g_znetServer->playerCount++;
}
void ZNetServer_RemovePlayer(int idx) {
    if (!g_znetServer || idx < 0 || idx > 511) return;
    std::lock_guard<std::recursive_mutex> g(g_znetServer->playersLock);
    Player &p = g_znetServer->players[idx];
    if (!p.active) return;
    p.name.clear(); p.score = 0; p.changed = time(nullptr); p.active = false; g_znetServer->playerCount--;
}
void ZNetServer_UpdatePlayer(int idx, int score) {
    if (!g_znetServer || idx < 0 || idx > 511) return;
    std::lock_guard<std::recursive_mutex> g(g_znetServer->playersLock);
    if (g_znetServer->players[idx].active) g_znetServer->players[idx].score = score;
}

#pragma pack(push, 4)
struct SteamIPAddress_t { uint8_t ip[16]; int32_t type; };
#pragma pack(pop)

void RegisterNetworkingCallbacks() {
    static bool done;
    if (done) return;
    done = true;
    // client pipe
    sc::registerCallback(163, false, [](void *d) { if (g_steamClient) g_steamClient->OnTicket(*(uint32_t *)d, *(int32_t *)((uint8_t *)d + 4)); });
    sc::registerCallback(1202, false, [](void *d) { if (cNet()) vcall<bool, uint64_t>(cNet(), 3, *(uint64_t *)d); });
    sc::registerCallback(1203, false, [](void *d) { if (g_steamClient) g_steamClient->OnP2PConnectFail(*(uint64_t *)d, ((uint8_t *)d)[8]); });
    sc::registerCallback(113, false, [](void *d) { sc::log(3, "ClientGameServerDeny reason %u\n", *(uint32_t *)((uint8_t *)d + 0xc)); });
    // game-server pipe
    sc::registerCallback(101, true, [](void *) {
        if (!g_znetServer || !gsServer()) return;
        vcall<void, int>(gsServer(), 12, g_znetServer->maxPlayers);
        SteamIPAddress_t ip = ((SteamIPAddress_t(*)(void *))(*(void ***)gsServer())[33])(gsServer());  // sret in x8
        memcpy(g_znetServer->publicIP, &ip, 20);
        uint32_t v4; memcpy(&v4, ip.ip, 4);
        sc::log(2, "Public IP: %u.%u.%u.%u\n", v4 >> 24, (v4 >> 16) & 255, (v4 >> 8) & 255, v4 & 255);
        g_znetServer->connectState = 1;
    });
    sc::registerCallback(102, true, [](void *d) { sc::log(3, "SteamServerConnectFailure %d\n", *(int *)d); if (g_znetServer) g_znetServer->connectState = 2; });
    sc::registerCallback(103, true, [](void *d) { sc::log(3, "SteamServersDisconnected %d\n", *(int *)d); if (g_znetServer) g_znetServer->connectState = 0; });
    sc::registerCallback(115, true, [](void *) {
        bool secure = gsServer() && vcall<bool>(gsServer(), 9);
        sc::log(2, "Zomboid Server is %sVAC Secure\n", secure ? "" : "not ");
    });
    sc::registerCallback(143, true, [](void *d) {
        uint8_t *b = (uint8_t *)d; uint64_t id, owner; int resp;
        memcpy(&id, b, 8); memcpy(&resp, b + 8, 4); memcpy(&owner, b + 0xc, 8);
        if (g_znetServer) g_znetServer->OnValidateAuth(id, resp, owner);
    });
    sc::registerCallback(1202, true, [](void *d) { if (g_znetServer) g_znetServer->OnP2PSessionRequest(*(uint64_t *)d); });
    sc::registerCallback(1203, true, [](void *d) { sc::log(2, "server P2PSessionConnectFail %d\n", ((uint8_t *)d)[8]); });
}

void UpdateSteamClientAndServer() {
    if (g_znetServer) g_znetServer->Update();
    if (g_steamClient) g_steamClient->Pump();
}

// ================================================================== JNI overrides
extern "C" {

JNIEXPORT void JNICALL RNPI(Init)(JNIEnv *e, jobject t, jboolean steam) {
    g_steamMode = steam == 1;
    if (!sc::jvm) e->GetJavaVM(&sc::jvm);
    RegisterNetworkingCallbacks();
    ORIG(Java_zombie_core_raknet_RakNetPeerInterface_Init, void (*)(JNIEnv *, jobject, jboolean))(e, t, JNI_FALSE);
}

JNIEXPORT void JNICALL RNPI(SetClientPort)(JNIEnv *e, jobject t, jint p) {
    clientPort = p; sd[0].port = p; sd[1].port = p + 1;
    ORIG(Java_zombie_core_raknet_RakNetPeerInterface_SetClientPort, void (*)(JNIEnv *, jobject, jint))(e, t, p);
}
JNIEXPORT void JNICALL RNPI(SetServerPort)(JNIEnv *e, jobject t, jint p, jint udp) {
    serverPort = p; serverUDPPort = udp; sd[0].port = p; sd[1].port = udp;
    ORIG(Java_zombie_core_raknet_RakNetPeerInterface_SetServerPort, void (*)(JNIEnv *, jobject, jint, jint))(e, t, p, udp);
}
JNIEXPORT void JNICALL RNPI(SetServerIP)(JNIEnv *e, jobject t, jstring ip) {
    const char *s = e->GetStringUTFChars(ip, nullptr);
    unsigned a, b, c, d;
    if (s && sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        strncpy(sd[0].hostAddress, s, 31); strncpy(sd[1].hostAddress, s, 31);
    }
    if (s) e->ReleaseStringUTFChars(ip, s);
    ORIG(Java_zombie_core_raknet_RakNetPeerInterface_SetServerIP, void (*)(JNIEnv *, jobject, jstring))(e, t, ip);
}

JNIEXPORT jint JNICALL RNPI(Startup)(JNIEnv *e, jobject t, jint maxConn, jstring ver, jboolean isServer) {
    if (!g_steamMode)
        return ORIG(Java_zombie_core_raknet_RakNetPeerInterface_Startup, jint (*)(JNIEnv *, jobject, jint, jstring, jboolean))(e, t, maxConn, ver, isServer);
    const char *v = e->GetStringUTFChars(ver, nullptr);
    sc::log(1, "Startup version %s\n", v);
    sd[0].socketFamily = sd[1].socketFamily = AF_INET;
    if (!isServer) {
        sd[0].port = clientPort; sd[1].port = clientPort + 1;
        g_steamClient = new ZNetClient();
        g_steamClient->version = v;
        if (g_cbObj) e->DeleteGlobalRef(g_cbObj);
        g_cbObj = e->NewGlobalRef(t);
        jclass c = e->GetObjectClass(t);
        g_cbMid = e->GetMethodID(c, "connectionStateChangedCallback", "(Ljava/lang/String;Ljava/lang/String;)V");
        peer().AttachPlugin(g_steamClient);
        g_networkState = 3;
    } else {
        sd[0].port = serverPort; sd[1].port = serverUDPPort;
        if (!g_znetServer) {
            ThrowJava(e, "java/lang/IllegalStateException", "SteamGameServer.Init() was not called");
            e->ReleaseStringUTFChars(ver, v);
            return 11;
        }
        g_znetServer->version = v;
        peer().AttachPlugin(g_znetServer);
        g_networkState = 2;
        sc::log(2, "Zomboid Steam Server started, ports %d and %d must be open on the router\n", g_znetServer->udpPort, g_znetServer->gamePort);
    }
    int r = peer().Startup(maxConn, sd, 2, -99999);
    sc::log(1, "RakPeer Startup -> %d\n", r);
    e->ReleaseStringUTFChars(ver, v);
    return r;
}

JNIEXPORT void JNICALL RNPI(Shutdown)(JNIEnv *e, jobject t) {
    if (g_znetServer) {
        if (void *gs = gsServer()) { vcall<void, bool>(gs, 24, false); vcall<void>(gs, 7); }
        peer().DetachPlugin(g_znetServer);
    }
    if (ZNetClient *c = g_steamClient) {
        c->CancelAuth();
        closeP2P(cNet(), c->serverID);
        peer().DetachPlugin(c);
    }
    peer().Shutdown(250, 0, rn::LOW_PRIORITY);
    if (g_znetServer) {

        GameServerShutdown();
        delete g_znetServer; g_znetServer = nullptr;
    }
    if (g_steamClient) { g_steamClient->CancelQuery(); delete g_steamClient; g_steamClient = nullptr; }
}

JNIEXPORT void JNICALL RNPI(SetIncomingPassword)(JNIEnv *e, jobject t, jstring pw) {
    const char *s = e->GetStringUTFChars(pw, nullptr);
    peer().SetIncomingPassword(s, (int)strlen(s));
    if (g_znetServer) {
        g_znetServer->password = s;
        if (void *gs = gsServer()) vcall<void, bool>(gs, 16, s[0] != 0);
    }
    e->ReleaseStringUTFChars(pw, s);
}

JNIEXPORT jint JNICALL RNPI(Connect)(JNIEnv *e, jobject t, jstring host, jint port, jstring pw, jboolean relay) {
    if (!g_steamMode)
        return ORIG(Java_zombie_core_raknet_RakNetPeerInterface_Connect, jint (*)(JNIEnv *, jobject, jstring, jint, jstring, jboolean))(e, t, host, port, pw, relay);
    if (g_networkState != 3 || !g_steamClient) return 1;
    ZNetClient *c = g_steamClient;
    const char *h = e->GetStringUTFChars(host, nullptr);
    const char *p = e->GetStringUTFChars(pw, nullptr);
    unsigned a, b, cc, d; jint r = 1;
    if (sscanf(h, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4) {
        uint32_t ip = a << 24 | b << 16 | cc << 8 | d;
        c->CancelQuery();
        if (c->state != 0) c->UpdateConnectionState(0);
        c->relay = relay; c->password = p;
        c->hQuery = cMMS() ? vcall<int, uint32_t, uint16_t, PingResponse *>(cMMS(), 13, ip, (uint16_t)port, (PingResponse *)c) : -1;
        c->serverAddr = rn::addrFrom(htonl(ip), (uint16_t)port);
        sc::log(1, "Connect %s:%d via Steam ping query %d\n", h, port, c->hQuery);
        r = 0;
    }
    e->ReleaseStringUTFChars(host, h); e->ReleaseStringUTFChars(pw, p);
    return r;
}

JNIEXPORT jint JNICALL RNPI(ConnectToSteamServer)(JNIEnv *e, jobject t, jlong id, jstring pw, jboolean relay) {
    if (!g_steamMode || !g_steamClient) return 1;
    ZNetClient *c = g_steamClient;
    c->serverAddr = rn::addrFrom(0, 0);
    const char *p = e->GetStringUTFChars(pw, nullptr);
    bool ok = c->StartP2P((uint64_t)id, p, relay);
    e->ReleaseStringUTFChars(pw, p);
    return ok ? 0 : 1;
}

JNIEXPORT jstring JNICALL RNPI(GetServerIP)(JNIEnv *e, jobject) {
    if (!g_steamMode) return nullptr;
    uint32_t ip = 0, type = 0;
    if (g_networkState == 2 && g_znetServer) { memcpy(&ip, g_znetServer->publicIP, 4); memcpy(&type, g_znetServer->publicIP + 16, 4); }
    else if (g_networkState == 3 && g_steamClient) { ip = g_steamClient->serverIP; type = g_steamClient->serverIPType; }
    if (!ip) return nullptr;
    char buf[32]; snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return e->NewStringUTF(buf);
}

static ClientConn *authedByGuid(jlong guid) {
    SystemAddress a = peer().GetSystemAddressFromGuid(rn::RakNetGUID{(uint64_t)guid, 0xFFFF});
    for (auto &c : g_znetServer->clients) if (c.state == 3 && rn::sameAddr(c.addr, a)) return &c;
    return nullptr;
}
JNIEXPORT jlong JNICALL RNPI(GetClientSteamID)(JNIEnv *, jobject, jlong guid) {
    if (!g_steamMode || !g_znetServer) return -1;
    std::lock_guard<std::recursive_mutex> g(g_znetServer->clientsLock);
    ClientConn *c = authedByGuid(guid);
    return c ? (jlong)c->steamID : -1;
}
JNIEXPORT jlong JNICALL RNPI(GetClientOwnerSteamID)(JNIEnv *, jobject, jlong guid) {
    if (!g_steamMode || !g_znetServer) return -1;
    std::lock_guard<std::recursive_mutex> g(g_znetServer->clientsLock);
    ClientConn *c = authedByGuid(guid);
    return c ? (jlong)c->owner : -1;
}

JNIEXPORT void JNICALL RNPI(disconnect)(JNIEnv *e, jobject t, jlong guid, jstring reason) {
    if (!g_steamClient) {
        ORIG(Java_zombie_core_raknet_RakNetPeerInterface_disconnect, void (*)(JNIEnv *, jobject, jlong, jstring))(e, t, guid, reason);
        return;
    }
    if (g_steamClient->state == 0) return;
    const char *r = reason ? e->GetStringUTFChars(reason, nullptr) : nullptr;
    std::string why = std::string(r ? r : "") + " (1201)";
    if (r) e->ReleaseStringUTFChars(reason, r);
    g_steamClient->Abort(why.c_str());
}

JNIEXPORT jbyte JNICALL RNPI(GetConnectionType)(JNIEnv *, jobject, jlong guid) {
    if (g_steamClient) return g_steamClient->state == 6 ? (g_steamClient->sockIdx == 1 ? 1 : 2) : 0;
    if (!g_steamMode || !g_znetServer) return 0;
    std::lock_guard<std::recursive_mutex> g(g_znetServer->clientsLock);
    ClientConn *c = authedByGuid(guid);
    return (!c || c->connType == 0) ? 0 : (c->connType == 2 ? 1 : 2);
}

}  // extern "C"
