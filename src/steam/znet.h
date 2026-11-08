// Shared state for the arm64 libZNetJNI64 port.
#pragma once
#include <jni.h>
#include <cstdint>
#include "steamcore.h"

extern "C" {
extern bool g_initialized;  // Steam usable (set by SteamUtils.n_Init)
extern bool g_bServer;      // dedicated-server mode
}

// Java String -> UTF-8 (String.getBytes("UTF-8")), as the original GetStandardUTFChars.
char *GetStandardUTFChars(JNIEnv *env, jstring s);
void ReleaseStandardUTFChars(char *p);
jstring NewStandardString(JNIEnv *env, const char *s);   // UTF-8 bytes -> String via new String(bytes,"UTF-8")
void ThrowJava(JNIEnv *env, const char *cls, const char *msg);

// Java static callbacks on zombie/core/znet/SteamUtils (used by the networking code).
int SteamUtils_ClientInitiateConnectionCallback(uint64_t steamID);
int SteamUtils_ValidateOwnerCallback(uint64_t a, uint64_t b);

#define JNIFN(ret, cls, name) extern "C" JNIEXPORT ret JNICALL Java_zombie_core_znet_##cls##_##name
