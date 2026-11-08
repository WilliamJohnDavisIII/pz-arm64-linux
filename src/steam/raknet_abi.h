// Minimal ABI mirror of the Android arm64 libRakNet64.so types we touch.
// Layouts and vtable orders verified against that lib.
#pragma once
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include "steamcore.h"

namespace rn {

struct SystemAddress {            // 20 bytes, IPv4 only
    uint16_t family;              // AF_INET
    uint16_t port;                // network order
    uint32_t addr;                // network order
    uint8_t zero[8];
    uint16_t debugPort;
    uint16_t systemIndex;
};
static_assert(sizeof(SystemAddress) == 20, "SystemAddress");

struct RakNetGUID { uint64_t g; uint16_t systemIndex; };   // passed in 2 regs
static_assert(sizeof(RakNetGUID) == 16, "RakNetGUID");

struct AddressOrGUID { RakNetGUID guid; SystemAddress addr; };

struct Packet {
    SystemAddress systemAddress;  // +0
    RakNetGUID guid;              // +0x18
    uint32_t length;              // +0x28
    uint32_t bitSize;             // +0x2c
    uint8_t *data;                // +0x30
};
static_assert(offsetof(Packet, guid) == 0x18 && offsetof(Packet, data) == 0x30, "Packet");

struct SocketDescriptor {         // stride 0x34
    uint16_t port;
    char hostAddress[32];
    int16_t socketFamily;
    uint16_t remotePortPS3;
    int32_t chromeInstance;
    bool blockingSocket;
    uint32_t extraSocketOptions;
};
static_assert(sizeof(SocketDescriptor) == 0x34, "SocketDescriptor");

template <typename T> struct List { T *listArray; uint32_t list_size, allocation_size; };

inline SystemAddress addrFrom(uint32_t ipNet, uint16_t portHost) {
    SystemAddress a{}; a.family = AF_INET; a.port = htons(portHost); a.addr = ipNet;
    a.debugPort = portHost; a.systemIndex = 0xFFFF; return a;
}
inline bool sameAddr(const SystemAddress &a, const SystemAddress &b) { return a.addr == b.addr && a.port == b.port; }
inline AddressOrGUID aog(const SystemAddress &a) { AddressOrGUID x; x.guid = {~0ull, 0xFFFF}; x.addr = a; return x; }
inline AddressOrGUID aog(uint64_t guid) { AddressOrGUID x{}; x.guid = {guid, 0xFFFF}; x.addr = addrFrom(0, 0); return x; }

// RakNet::PluginInterface2: vtable [D1, D0, OnAttach, OnDetach, Update, OnReceive, OnRakPeerStartup,
// OnRakPeerShutdown, OnClosedConnection, OnNewConnection, OnFailedConnectionAttempt, UsesReliabilityLayer,
// OnDirectSocketSend, OnDirectSocketReceive, OnReliabilityLayerNotification, OnInternalPacket, OnAck,
// OnPushBackPacket]; fields {rakPeerInterface@8, tcpInterface@0x10}.
class Plugin {
public:
    virtual ~Plugin() {}
    virtual void OnAttach() {}
    virtual void OnDetach() {}
    virtual void Update() {}
    virtual int OnReceive(Packet *) { return 1; }                 // RR_CONTINUE_PROCESSING
    virtual void OnRakPeerStartup() {}
    virtual void OnRakPeerShutdown() {}
    virtual void OnClosedConnection(const SystemAddress &, RakNetGUID, int) {}
    virtual void OnNewConnection(const SystemAddress &, RakNetGUID, bool) {}
    virtual void OnFailedConnectionAttempt(Packet *, int) {}
    virtual bool UsesReliabilityLayer() const { return false; }
    virtual void OnDirectSocketSend(const char *, uint32_t, SystemAddress) {}
    virtual void OnDirectSocketReceive(const char *, uint32_t, SystemAddress) {}
    virtual void OnReliabilityLayerNotification(const char *, uint32_t, SystemAddress, bool) {}
    virtual void OnInternalPacket(void *, unsigned, SystemAddress, uint32_t, int) {}
    virtual void OnAck(unsigned, SystemAddress, uint32_t) {}
    virtual void OnPushBackPacket(const char *, uint32_t, SystemAddress) {}
    void *rakPeerInterface = nullptr;
    void *tcpInterface = nullptr;
};

// RakNet::SocketLayerOverride: [D1, D0, RakNetSendTo, RakNetRecvFrom]
class SocketOverride {
public:
    virtual ~SocketOverride() {}
    virtual int RakNetSendTo(const char *data, int len, const SystemAddress &to) = 0;
    virtual int RakNetRecvFrom(char *out /*1492*/, SystemAddress *from, bool mainThread) = 0;
};

enum { RR_STOP_PROCESSING_AND_DEALLOCATE = 0, RR_CONTINUE_PROCESSING = 1 };
enum { HIGH_PRIORITY = 1, LOW_PRIORITY = 3 };
enum { RELIABLE = 2 };
enum { ID_CONNECTION_ATTEMPT_FAILED = 0x11, ID_INCOMPATIBLE_PROTOCOL_VERSION = 0x19 };

// RakPeerInterface vtable calls.
struct Peer {
    void *p;
    int Startup(unsigned maxConn, SocketDescriptor *sd, unsigned n, int prio) { return sc::vcall<int, unsigned, SocketDescriptor *, unsigned, int>(p, 2, maxConn, sd, n, prio); }
    void SetIncomingPassword(const char *pw, int len) { sc::vcall<void, const char *, int>(p, 11, pw, len); }
    int Connect(const char *host, uint16_t port, const char *pw, int pwLen, unsigned sockIdx) {
        return sc::vcall<int, const char *, unsigned short, const char *, int, void *, unsigned, unsigned, unsigned, uint32_t>(p, 13, host, port, pw, pwLen, nullptr, sockIdx, 12, 500, 0);
    }
    void Shutdown(unsigned block, unsigned char ch, int prio) { sc::vcall<void, unsigned, unsigned char, int>(p, 15, block, ch, prio); }
    uint32_t Send(const char *d, int len, int prio, int rel, char ch, AddressOrGUID to, bool bc) {
        return sc::vcall<uint32_t, const char *, int, int, int, char, AddressOrGUID, bool, uint32_t>(p, 20, d, len, prio, rel, ch, to, bc, 0);
    }
    void CloseConnection(AddressOrGUID t, bool notify, unsigned char ch, int prio) { sc::vcall<void, AddressOrGUID, bool, unsigned char, int>(p, 27, t, notify, ch, prio); }
    int GetConnectionState(AddressOrGUID t) { return sc::vcall<int, AddressOrGUID>(p, 28, t); }
    SystemAddress GetSystemAddressFromGuid(RakNetGUID g) { return sc::vcall<SystemAddress, RakNetGUID>(p, 54, g); }
    void AttachPlugin(Plugin *pl) { sc::vcall<void, Plugin *>(p, 68, pl); }
    void DetachPlugin(Plugin *pl) { sc::vcall<void, Plugin *>(p, 69, pl); }
    void PushBackPacket(Packet *pk, bool head) { sc::vcall<void, Packet *, bool>(p, 70, pk, head); }
    Packet *AllocatePacket(unsigned size) { return sc::vcall<Packet *, unsigned>(p, 72, size); }
    void GetSockets(List<void *> &l) { sc::vcall<void, List<void *> &>(p, 74, l); }
};

}  // namespace rn

// Exported by the patched Android libRakNet64.so / libZNetNoSteam64.so.
namespace RakNet {
struct SocketLayerOverride;
class RNS2_Linux { public: void SetSocketLayerOverride(SocketLayerOverride *); };
uint32_t GetTimeMS();
class RakPeerInterface { public: static uint64_t Get64BitUniqueRandomNumber(); };
}
extern "C" void *g_peer;   // libZNetNoSteam64: RakNet::RakPeerInterface* g_peer
