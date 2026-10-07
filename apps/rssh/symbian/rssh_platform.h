/*
 * rssh_platform.h: C++ entry points of the Symbian platform layer
 * (symbian/platform.cpp), for the UI.
 */
#ifndef RSSH_PLATFORM_H
#define RSSH_PLATFORM_H

/* Call once from the app UI's ConstructL, before using any PuTTY code. */
void RsshPlatformInitL();
/* Bracket every modal dialog shown while PuTTY may be mid-operation. */
void RsshPlatformSetModal(TBool aModal);
TBool RsshPlatformIsModal();
/* Call from the app UI's destructor. */
void RsshPlatformShutdown();

/* sock.cpp: close the socket server session (from RsshPlatformShutdown). */
void RsshSockShutdown();

#endif
