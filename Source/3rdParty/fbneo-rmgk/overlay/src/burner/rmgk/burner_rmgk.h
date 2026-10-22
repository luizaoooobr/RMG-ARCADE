// Platform shims for the headless "lib" frontend.
//
// This frontend does not create a window, play audio or read input by itself:
// it is compiled into the fbneo_core shared library that a host application
// (RMG-K) links against. It only needs to satisfy the OS-dependent type/def
// expectations of the burner/burn layer (RECT, MAX_PATH, __cdecl, ...).
#pragma once

#ifndef BUILD_LIB
#define BUILD_LIB
#endif

#ifndef _WIN32
typedef struct tagRECT
{
	int left;
	int top;
	int right;
	int bottom;
} RECT, * PRECT, * LPRECT;
typedef const RECT* LPCRECT;
typedef UINT32 DWORD;
typedef UINT8 BYTE;
#else
#include <windows.h>
#endif

#ifndef MAX_PATH
#define MAX_PATH    511
#endif

#ifndef __cdecl
#define __cdecl
#endif

// Application state normally owned by the frontend's drv.cpp / misc code.
// Defined in this frontend's frontend.cpp. lowpass2.cpp (which this frontend
// compiles) reads bRunPause, so it must be declared here — burner.h is included
// by that file but only the *definitions* live in frontend.cpp.
extern int  bDrvOkay;				// 1 if the driver has been initted okay
extern int  bRunPause;				// 1 while emulation is paused
bool AppProcessKeyboardInput();

// Synchronized-netplay determinism switch. The host (RMG-K) sets this before
// driver init for any session where two peers must stay bit-identical (rollback
// netplay, lobby, Kaillera). Defined in frontend.cpp; read by
// is_netgame_or_recording().
extern int  nRMGKNetgameMode;
