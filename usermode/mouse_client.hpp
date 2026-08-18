#pragma once

#include "../include/mouse_spoof_abi.hpp"

#include <Windows.h>

// DeviceIoControl wrapper. Pass the already-opened driver HANDLE.
namespace mouse_client
{
	inline bool ioctl(HANDLE driver, DWORD code, void* in_out, DWORD size, void* out = nullptr, DWORD out_size = 0)
	{
		if (!driver || driver == INVALID_HANDLE_VALUE)
			return false;
		DWORD bytes = 0;
		return DeviceIoControl(
			driver, code, in_out, size,
			out ? out : in_out, out ? out_size : size,
			&bytes, nullptr) == TRUE;
	}

	inline bool move(HANDLE driver, int dx, int dy, unsigned short button_flags = 0)
	{
		MOUSE_SPOOF_MOVE_REQUEST req{};
		req.X = static_cast<long>(dx) + MOUSE_SPOOF_MOVE_BIAS;
		req.Y = static_cast<long>(dy) + MOUSE_SPOOF_MOVE_BIAS;
		req.ButtonFlags = button_flags;
		return ioctl(driver, MOUSE_SPOOF_IOCTL_MOVE, &req, sizeof(req), nullptr, 0);
	}

	inline bool left_click(HANDLE driver, int hold_ms = 12)
	{
		if (!move(driver, 0, 0, MOUSE_LEFT_BUTTON_DOWN))
			return false;
		if (hold_ms > 0)
		{
			if (hold_ms < 1) hold_ms = 1;
			if (hold_ms > 80) hold_ms = 80;
			Sleep(static_cast<DWORD>(hold_ms));
		}
		return move(driver, 0, 0, MOUSE_LEFT_BUTTON_UP);
	}

	inline bool spoof_set(HANDLE driver, ULONG suppress_mask, MOUSE_SPOOF_REQUEST* out = nullptr)
	{
		MOUSE_SPOOF_REQUEST req{};
		req.SetMask = 1;
		req.SuppressMask = suppress_mask & (MOUSE_BUTTON_4_DOWN | MOUSE_BUTTON_5_DOWN);
		if (!ioctl(driver, MOUSE_SPOOF_IOCTL_SPOOF, &req, sizeof(req)))
			return false;
		if (out)
			*out = req;
		return true;
	}

	inline bool spoof_query(HANDLE driver, MOUSE_SPOOF_REQUEST& out)
	{
		out = {};
		out.SetMask = 0;
		return ioctl(driver, MOUSE_SPOOF_IOCTL_SPOOF, &out, sizeof(out));
	}

	inline bool hooked(const MOUSE_SPOOF_REQUEST& r)
	{
		return (r.HookInstalled & 1u) != 0;
	}

	inline ULONG hook_count(const MOUSE_SPOOF_REQUEST& r)
	{
		if (((r.HookInstalled >> 16) & 0xFFFFu) == 0x4D53u)
			return (r.HookInstalled >> 8) & 0xFFu;
		return r.HookCount;
	}

	inline bool abi_ok(const MOUSE_SPOOF_REQUEST& r)
	{
		if (((r.HookInstalled >> 16) & 0xFFFFu) == 0x4D53u)
			return true;
		return r.Abi == MOUSE_SPOOF_ABI;
	}
}
