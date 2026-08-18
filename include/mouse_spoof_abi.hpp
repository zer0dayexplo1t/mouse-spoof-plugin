#pragma once

// Shared kernel <-> usermode ABI. Include from both sides.

#ifdef _KERNEL_MODE
#include <wdm.h>
#include <ntddmou.h>
#else
#include <Windows.h>
#include <winioctl.h>
#endif

#ifndef MOUSE_BUTTON_4_DOWN
#define MOUSE_BUTTON_4_DOWN 0x0040
#define MOUSE_BUTTON_4_UP   0x0080
#define MOUSE_BUTTON_5_DOWN 0x0100
#define MOUSE_BUTTON_5_UP   0x0200
#endif
#ifndef MOUSE_LEFT_BUTTON_DOWN
#define MOUSE_LEFT_BUTTON_DOWN 0x0001
#define MOUSE_LEFT_BUTTON_UP   0x0002
#endif

#define MOUSE_SPOOF_IOCTL_MOVE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0xE5B9, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#define MOUSE_SPOOF_IOCTL_SPOOF CTL_CODE(FILE_DEVICE_UNKNOWN, 0x9B18, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)

// Added to dx/dy on the wire so zero-move clicks are not a raw {0,0} packet.
#define MOUSE_SPOOF_MOVE_BIAS 52347

// Layout stamp only. Do not bump for discovery/hook-policy changes.
constexpr ULONG MOUSE_SPOOF_ABI = 0x4D530003u; // 'MS' | 3

typedef struct _MOUSE_SPOOF_MOVE_REQUEST
{
	long X;
	long Y;
	unsigned short ButtonFlags;
} MOUSE_SPOOF_MOVE_REQUEST, *PMOUSE_SPOOF_MOVE_REQUEST;

typedef struct _MOUSE_SPOOF_REQUEST
{
	ULONG SetMask;       // 1 = apply SuppressMask
	ULONG SuppressMask;  // MOUSE_BUTTON_4_DOWN and/or MOUSE_BUTTON_5_DOWN
	ULONG ButtonsDown;   // out: held suppressed X buttons (DOWN flags)
	ULONG HookInstalled; // out: bit0 hooked, bits8-15 hook count, high word 0x4D53 ABI tag
	ULONG PhysicalMove;  // out: abs HID LastX/LastY sum since last sample
	ULONG PhysicalIdleMs;
	LONG PhysicalDx;
	LONG PhysicalDy;
	ULONG HookCount;
	ULONG Abi;
} MOUSE_SPOOF_REQUEST, *PMOUSE_SPOOF_REQUEST;
