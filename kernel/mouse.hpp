#pragma once

#include <wdm.h>
#include <ntddmou.h>
#include "../include/mouse_spoof_abi.hpp"

typedef VOID (*MouseClassServiceCallback)(
	PDEVICE_OBJECT DeviceObject,
	PMOUSE_INPUT_DATA InputDataStart,
	PMOUSE_INPUT_DATA InputDataEnd,
	PULONG InputDataConsumed);

typedef struct _MOUSE_OBJECT
{
	PDEVICE_OBJECT MouseDevice;
	MouseClassServiceCallback ServiceCallback;
	MouseClassServiceCallback* ServiceCallbackSlot;
	USHORT UnitId;
	USHORT MoveFlags;
	BOOLEAN HaveRules;
	BOOLEAN DeviceReferenced;
	BOOLEAN HookInstalled;
} MOUSE_OBJECT, *PMOUSE_OBJECT;

extern MOUSE_OBJECT MouseObject;

namespace MouseHandler
{
	void MoveMouse(MOUSE_OBJECT& MouseObject, long X, long Y, unsigned short ButtonFlags);
	NTSTATUS InitMouse(PMOUSE_OBJECT MouseObject);
	NTSTATUS EnsureMouse(PMOUSE_OBJECT MouseObject);
	void ReleaseMouse(PMOUSE_OBJECT MouseObject);

	void SetSuppressMask(ULONG suppress_down_flags);
	ULONG GetSuppressMask();
	ULONG GetSpoofedButtonsDown();
	BOOLEAN IsHookInstalled(const MOUSE_OBJECT& MouseObject);
	ULONG GetHookCount();
	void SamplePhysicalActivity(
		ULONG& move_out,
		ULONG& idle_ms_out,
		LONG& dx_out,
		LONG& dy_out);
}
