#pragma once

#include "mouse_client.hpp"

#include <algorithm>
#include <cmath>

// Physical HID sample + XBUTTON hold when the kernel hook is stripping those buttons.
namespace mouse_spoof
{
	inline HANDLE g_driver = nullptr;
	inline ULONG g_suppress_mask = 0;

	struct physical_sample_t
	{
		float activity = 0.55f;
		ULONG idle_ms = 60000;
		float dx = 0.f;
		float dy = 0.f;
		float mag = 0.f;
		bool live = false;
		bool hooked = false;
		bool queried = false;
		ULONG hook_count = 0;
		ULONG abi = 0;
		bool abi_ok = false;
	};

	inline void bind(HANDLE driver)
	{
		g_driver = driver;
	}

	inline void set_suppress_mask(ULONG mask)
	{
		g_suppress_mask = mask & (MOUSE_BUTTON_4_DOWN | MOUSE_BUTTON_5_DOWN);
	}

	inline void sync()
	{
		if (!g_driver)
			return;
		(void)mouse_client::spoof_set(g_driver, g_suppress_mask);
	}

	inline physical_sample_t sample_physical()
	{
		physical_sample_t out{};
		static float hold_dx = 0.f;
		static float hold_dy = 0.f;
		static ULONGLONG hold_until = 0;

		const ULONGLONG now = GetTickCount64();
		if (!g_driver)
			return out;

		MOUSE_SPOOF_REQUEST req{};
		if (!mouse_client::spoof_query(g_driver, req))
			return out;

		out.queried = true;
		out.hooked = mouse_client::hooked(req);
		out.hook_count = mouse_client::hook_count(req);
		out.abi_ok = mouse_client::abi_ok(req);
		out.abi = out.abi_ok ? MOUSE_SPOOF_ABI : req.Abi;
		out.idle_ms = req.PhysicalIdleMs;

		auto apply_cursor_fallback = [&]()
		{
			POINT pt{};
			if (!GetCursorPos(&pt))
				return;
			static LONG lx = 0;
			static LONG ly = 0;
			static bool have = false;
			if (!have)
			{
				lx = pt.x;
				ly = pt.y;
				have = true;
				return;
			}
			const LONG cdx = pt.x - lx;
			const LONG cdy = pt.y - ly;
			lx = pt.x;
			ly = pt.y;
			const LONG ax = (cdx >= 0 ? cdx : -cdx) + (cdy >= 0 ? cdy : -cdy);
			if (ax <= 0)
				return;
			hold_dx = static_cast<float>(cdx);
			hold_dy = static_cast<float>(cdy);
			hold_until = now + 48ull;
			out.idle_ms = 0;
			out.dx = hold_dx;
			out.dy = hold_dy;
			out.live = true;
			out.mag = std::sqrt(out.dx * out.dx + out.dy * out.dy);
			out.activity = (std::min)(1.f, out.mag / 18.f);
		};

		if (!out.hooked)
		{
			out.activity = 0.55f;
			apply_cursor_fallback();
			return out;
		}

		if (req.PhysicalMove > 0 || req.PhysicalDx != 0 || req.PhysicalDy != 0)
		{
			hold_dx = static_cast<float>(req.PhysicalDx);
			hold_dy = static_cast<float>(req.PhysicalDy);
			hold_until = now + 48ull;
		}

		if (now <= hold_until)
		{
			out.dx = hold_dx;
			out.dy = hold_dy;
			out.live = true;
		}

		out.mag = std::sqrt(out.dx * out.dx + out.dy * out.dy);
		float hot = static_cast<float>(req.PhysicalMove) / 28.f;
		if (hot < 0.f) hot = 0.f;
		if (hot > 1.f) hot = 1.f;
		if (out.live && out.mag > 0.5f)
			hot = (std::max)(hot, (std::min)(1.f, out.mag / 18.f));

		float idle_f = 1.f;
		if (out.idle_ms > 90)
		{
			idle_f = 1.f - static_cast<float>(out.idle_ms - 90) / 420.f;
			if (idle_f < 0.f) idle_f = 0.f;
			if (idle_f > 1.f) idle_f = 1.f;
		}
		out.activity = out.live ? (std::max)(hot, idle_f * 0.35f) : 0.f;

		if ((!out.abi_ok || out.idle_ms >= 50000u) && !out.live)
			apply_cursor_fallback();

		return out;
	}

	inline bool key_down(int vk)
	{
		if (vk <= 0)
			return true;

		const bool async = (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (vk != VK_XBUTTON1 && vk != VK_XBUTTON2)
			return async;
		if (!g_driver)
			return async;

		MOUSE_SPOOF_REQUEST req{};
		if (!mouse_client::spoof_query(g_driver, req))
			return async;

		const ULONG bit = (vk == VK_XBUTTON1) ? MOUSE_BUTTON_4_DOWN : MOUSE_BUTTON_5_DOWN;
		const bool spoofed = (req.ButtonsDown & bit) != 0;
		const bool hooked = mouse_client::hooked(req);
		if (hooked && (req.SuppressMask & bit))
			return spoofed || async;
		return async || spoofed;
	}
}
