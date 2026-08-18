#include "mouse.hpp"
#include "ntundoc.hpp"
#include <ntimage.h>

MOUSE_OBJECT MouseObject = {};

namespace
{
	volatile LONG g_mouse_init_once = 0;
	BOOLEAN g_mouse_ready = FALSE;

	volatile LONG g_hooks_banned = 0;
	volatile LONG g_inject_ok_count = 0;
	constexpr LONG k_hook_after_injects = 3;

	volatile LONG g_suppress_down = 0;
	volatile LONG g_spoofed_down = 0;
	volatile LONG g_phys_move_accum = 0;
	volatile LONG g_phys_dx_accum = 0;
	volatile LONG g_phys_dy_accum = 0;
	volatile LONG g_phys_last_tick = 0;

	constexpr int k_max_hooks = 8;
	constexpr int k_max_exec_ranges = 24;

	struct exec_range_t
	{
		ULONG_PTR lo = 0;
		ULONG_PTR hi = 0;
	};

	struct hook_slot_t
	{
		PDEVICE_OBJECT class_dev = nullptr;
		MouseClassServiceCallback* slot = nullptr;
		MouseClassServiceCallback original = nullptr;
	};

	hook_slot_t g_hooks[k_max_hooks]{};
	int g_hook_count = 0;
	hook_slot_t g_pending[k_max_hooks]{};
	int g_pending_count = 0;

	exec_range_t g_mouclass_exec[k_max_exec_ranges]{};
	int g_mouclass_exec_n = 0;

	VOID MouseSpoofServiceCallback(
		PDEVICE_OBJECT DeviceObject,
		PMOUSE_INPUT_DATA InputDataStart,
		PMOUSE_INPUT_DATA InputDataEnd,
		PULONG InputDataConsumed);

	LONG tick32()
	{
		LARGE_INTEGER t{};
		KeQueryTickCount(&t);
		return t.LowPart;
	}

	ULONG ticks_to_ms(LONG ticks)
	{
		ULONG inc = KeQueryTimeIncrement();
		if (inc == 0)
			inc = 10000;
		const ULONGLONG ns100 = static_cast<ULONGLONG>(ticks > 0 ? ticks : 0) * inc;
		ULONG ms = static_cast<ULONG>(ns100 / 10000ull);
		if (ms > 60000ul)
			ms = 60000ul;
		return ms;
	}

	BOOLEAN ptr_in_ranges(ULONG_PTR candidate, const exec_range_t* ranges, int n)
	{
		if (!candidate || !ranges || n <= 0)
			return FALSE;
		for (int i = 0; i < n; ++i)
		{
			if (candidate >= ranges[i].lo && candidate < ranges[i].hi)
				return TRUE;
		}
		return FALSE;
	}

	int collect_exec_ranges(PVOID image_base, exec_range_t* out, int max_out)
	{
		if (!image_base || !out || max_out <= 0)
			return 0;

		int n = 0;
		__try
		{
			const auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(image_base);
			if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
				return 0;
			if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
				return 0;

			const auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
				reinterpret_cast<PUCHAR>(image_base) + dos->e_lfanew);
			if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
				return 0;

			const USHORT sections = nt->FileHeader.NumberOfSections;
			if (sections == 0 || sections > 96)
				return 0;

			auto* sec = IMAGE_FIRST_SECTION(nt);
			const auto base = reinterpret_cast<ULONG_PTR>(image_base);
			for (USHORT i = 0; i < sections && n < max_out; ++i)
			{
				if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
					continue;
				const ULONG_PTR lo = base + sec[i].VirtualAddress;
				ULONG size = sec[i].Misc.VirtualSize;
				if (size == 0)
					size = sec[i].SizeOfRawData;
				if (size == 0 || size > 0x4000000)
					continue;
				out[n].lo = lo;
				out[n].hi = lo + size;
				++n;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
		return n;
	}

	BOOLEAN callback_is_safe(ULONG_PTR candidate)
	{
		if (!candidate)
			return FALSE;
		if (candidate == reinterpret_cast<ULONG_PTR>(&MouseSpoofServiceCallback))
			return TRUE;
		return ptr_in_ranges(candidate, g_mouclass_exec, g_mouclass_exec_n);
	}

	BOOLEAN set_mouclass_exec_ranges(PDRIVER_OBJECT ClassDriverObject)
	{
		g_mouclass_exec_n = 0;
		if (!ClassDriverObject || !ClassDriverObject->DriverStart)
			return FALSE;
		g_mouclass_exec_n = collect_exec_ranges(
			ClassDriverObject->DriverStart, g_mouclass_exec, k_max_exec_ranges);
		return g_mouclass_exec_n > 0;
	}

	USHORT strip_mask_from_suppress(LONG suppress_down)
	{
		USHORT strip = 0;
		if (suppress_down & MOUSE_BUTTON_4_DOWN)
			strip = static_cast<USHORT>(strip | MOUSE_BUTTON_4_DOWN | MOUSE_BUTTON_4_UP);
		if (suppress_down & MOUSE_BUTTON_5_DOWN)
			strip = static_cast<USHORT>(strip | MOUSE_BUTTON_5_DOWN | MOUSE_BUTTON_5_UP);
		return strip;
	}

	VOID MouseSpoofServiceCallback(
		PDEVICE_OBJECT DeviceObject,
		PMOUSE_INPUT_DATA InputDataStart,
		PMOUSE_INPUT_DATA InputDataEnd,
		PULONG InputDataConsumed)
	{
		const LONG suppress = InterlockedCompareExchange(&g_suppress_down, 0, 0);
		const USHORT strip = strip_mask_from_suppress(suppress);

		if (InputDataStart && InputDataEnd && InputDataEnd > InputDataStart)
		{
			InterlockedExchange(&g_phys_last_tick, tick32());

			for (PMOUSE_INPUT_DATA p = InputDataStart; p < InputDataEnd; ++p)
			{
				const USHORT f = p->ButtonFlags;
				if (f & MOUSE_BUTTON_4_DOWN)
					InterlockedOr(&g_spoofed_down, MOUSE_BUTTON_4_DOWN);
				if (f & MOUSE_BUTTON_4_UP)
					InterlockedAnd(&g_spoofed_down, ~static_cast<LONG>(MOUSE_BUTTON_4_DOWN));
				if (f & MOUSE_BUTTON_5_DOWN)
					InterlockedOr(&g_spoofed_down, MOUSE_BUTTON_5_DOWN);
				if (f & MOUSE_BUTTON_5_UP)
					InterlockedAnd(&g_spoofed_down, ~static_cast<LONG>(MOUSE_BUTTON_5_DOWN));

				const LONG dx = p->LastX;
				const LONG dy = p->LastY;
				const LONG ax = dx >= 0 ? dx : -dx;
				const LONG ay = dy >= 0 ? dy : -dy;
				if (ax > 0 || ay > 0)
				{
					LONG add = ax + ay;
					if (add > 4096)
						add = 4096;
					InterlockedAdd(&g_phys_move_accum, add);
					InterlockedAdd(&g_phys_dx_accum, dx);
					InterlockedAdd(&g_phys_dy_accum, dy);
				}

				if (strip)
					p->ButtonFlags = static_cast<USHORT>(f & ~strip);
			}
		}

		MouseClassServiceCallback original = nullptr;
		for (int i = 0; i < g_hook_count; ++i)
		{
			if (g_hooks[i].class_dev == DeviceObject && g_hooks[i].original)
			{
				original = g_hooks[i].original;
				break;
			}
		}
		if (!original)
			original = MouseObject.ServiceCallback;

		if (original && callback_is_safe(reinterpret_cast<ULONG_PTR>(original)))
			original(DeviceObject, InputDataStart, InputDataEnd, InputDataConsumed);
		else if (InputDataConsumed)
			*InputDataConsumed = 0;
	}

	void uninstall_all_hooks()
	{
		for (int i = 0; i < g_hook_count; ++i)
		{
			if (g_hooks[i].slot && g_hooks[i].original)
			{
				InterlockedExchangePointer(
					reinterpret_cast<PVOID volatile*>(g_hooks[i].slot),
					reinterpret_cast<PVOID>(g_hooks[i].original));
			}
			g_hooks[i] = {};
		}
		g_hook_count = 0;
		if (MouseObject.HookInstalled)
			MouseObject.HookInstalled = FALSE;
	}

	void ban_hooks_keep_inject()
	{
		InterlockedExchange(&g_hooks_banned, 1);
		uninstall_all_hooks();
		g_pending_count = 0;
		InterlockedExchange(&g_suppress_down, 0);
	}

	void clear_mouse_object(PMOUSE_OBJECT MouseObj)
	{
		if (!MouseObj)
			return;

		uninstall_all_hooks();
		g_pending_count = 0;

		if (MouseObj->DeviceReferenced && MouseObj->MouseDevice)
		{
			ObDereferenceObject(MouseObj->MouseDevice);
			MouseObj->DeviceReferenced = FALSE;
		}
		MouseObj->MouseDevice = nullptr;
		MouseObj->ServiceCallback = nullptr;
		MouseObj->ServiceCallbackSlot = nullptr;
		MouseObj->UnitId = 0;
		MouseObj->MoveFlags = MOUSE_MOVE_RELATIVE;
		MouseObj->HaveRules = FALSE;
		MouseObj->HookInstalled = FALSE;
	}

	USHORT unit_id_from_device(PDEVICE_OBJECT DeviceObject)
	{
		if (!DeviceObject)
			return 0;

		UCHAR nameBuf[sizeof(OBJECT_NAME_INFORMATION) + 256]{};
		auto* nameInfo = reinterpret_cast<POBJECT_NAME_INFORMATION>(nameBuf);
		ULONG ret = 0;
		const NTSTATUS st = ObQueryNameString(DeviceObject, nameInfo, sizeof(nameBuf), &ret);
		if (!NT_SUCCESS(st) || !nameInfo->Name.Buffer || nameInfo->Name.Length < sizeof(WCHAR))
			return 0;

		const USHORT nChars = static_cast<USHORT>(nameInfo->Name.Length / sizeof(WCHAR));
		if (nChars == 0)
			return 0;

		LONG end = static_cast<LONG>(nChars) - 1;
		while (end >= 0)
		{
			const WCHAR c = nameInfo->Name.Buffer[end];
			if (c < L'0' || c > L'9')
				break;
			--end;
		}
		USHORT unit = 0;
		for (LONG j = end + 1; j < static_cast<LONG>(nChars); ++j)
			unit = static_cast<USHORT>(unit * 10u + static_cast<USHORT>(nameInfo->Name.Buffer[j] - L'0'));
		return unit;
	}

	BOOLEAN is_mouclass_device(PDRIVER_OBJECT ClassDriverObject, PDEVICE_OBJECT candidate)
	{
		if (!ClassDriverObject || !candidate)
			return FALSE;
		for (PDEVICE_OBJECT d = ClassDriverObject->DeviceObject; d; d = d->NextDevice)
		{
			if (d == candidate)
				return TRUE;
		}
		return FALSE;
	}

	void remember_pending(
		PDEVICE_OBJECT class_dev,
		MouseClassServiceCallback* slot,
		MouseClassServiceCallback original,
		PDEVICE_OBJECT* matchedDevice,
		MouseClassServiceCallback* matchedCb,
		MouseClassServiceCallback** matchedSlot)
	{
		if (!class_dev || !slot || !original || g_pending_count >= k_max_hooks)
			return;
		if (!callback_is_safe(reinterpret_cast<ULONG_PTR>(original)))
			return;
		for (int h = 0; h < g_pending_count; ++h)
		{
			if (g_pending[h].slot == slot)
				return;
		}
		g_pending[g_pending_count].class_dev = class_dev;
		g_pending[g_pending_count].slot = slot;
		g_pending[g_pending_count].original = original;
		++g_pending_count;

		if (matchedDevice && !*matchedDevice)
			*matchedDevice = class_dev;
		if (matchedCb && !*matchedCb)
			*matchedCb = original;
		if (matchedSlot && !*matchedSlot)
			*matchedSlot = slot;
	}

	void install_pending_hooks(PMOUSE_OBJECT MouseObj)
	{
		if (!MouseObj || InterlockedCompareExchange(&g_hooks_banned, 0, 0) != 0)
			return;
		if (g_hook_count > 0 || g_pending_count <= 0)
			return;

		g_hook_count = 0;
		for (int h = 0; h < g_pending_count && g_hook_count < k_max_hooks; ++h)
		{
			if (!g_pending[h].slot || !g_pending[h].original)
				continue;
			if (!callback_is_safe(reinterpret_cast<ULONG_PTR>(g_pending[h].original)))
				continue;

			const auto cur = reinterpret_cast<MouseClassServiceCallback>(
				InterlockedCompareExchangePointer(
					reinterpret_cast<PVOID volatile*>(g_pending[h].slot),
					nullptr,
					nullptr));
			if (cur != g_pending[h].original)
				continue;

			g_hooks[g_hook_count] = g_pending[h];
			InterlockedExchangePointer(
				reinterpret_cast<PVOID volatile*>(g_hooks[g_hook_count].slot),
				reinterpret_cast<PVOID>(&MouseSpoofServiceCallback));
			++g_hook_count;
		}

		MouseObj->HookInstalled = (g_hook_count > 0) ? TRUE : FALSE;
	}

	void scan_hid_connect_data(
		PDEVICE_OBJECT hid_dev,
		PDRIVER_OBJECT ClassDriverObject,
		PDEVICE_OBJECT* matchedDevice,
		MouseClassServiceCallback* matchedCb,
		MouseClassServiceCallback** matchedSlot)
	{
		if (!hid_dev || !hid_dev->DeviceExtension || !ClassDriverObject)
			return;

		PDEVICE_OBJECT upper = nullptr;
		__try
		{
			upper = hid_dev->AttachedDevice;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return;
		}

		__try
		{
			auto* DeviceExtension = reinterpret_cast<PULONG_PTR>(hid_dev->DeviceExtension);
			constexpr ULONG_PTR k_max_index = 512;
			for (ULONG_PTR i = 0; i + 1 < k_max_index; ++i)
			{
				ULONG_PTR maybe_dev = 0;
				ULONG_PTR maybe_cb = 0;
				__try
				{
					maybe_dev = DeviceExtension[i];
					maybe_cb = DeviceExtension[i + 1];
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					break;
				}

				auto* class_dev = reinterpret_cast<PDEVICE_OBJECT>(maybe_dev);
				if (!class_dev)
					continue;

				const BOOLEAN upper_match = (upper && class_dev == upper);
				const BOOLEAN class_match = is_mouclass_device(ClassDriverObject, class_dev);
				if (!upper_match && !class_match)
					continue;
				if (!callback_is_safe(maybe_cb))
					continue;

				remember_pending(
					class_dev,
					reinterpret_cast<MouseClassServiceCallback*>(&DeviceExtension[i + 1]),
					reinterpret_cast<MouseClassServiceCallback>(maybe_cb),
					matchedDevice,
					matchedCb,
					matchedSlot);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	NTSTATUS init_mouse_locked(PMOUSE_OBJECT MouseObj)
	{
		if (!MouseObj)
			return STATUS_INVALID_PARAMETER;

		clear_mouse_object(MouseObj);
		g_mouclass_exec_n = 0;
		InterlockedExchange(&g_inject_ok_count, 0);

		if (!IoDriverObjectType || !*IoDriverObjectType)
			return STATUS_UNSUCCESSFUL;

		UNICODE_STRING ClassString = RTL_CONSTANT_STRING(L"\\Driver\\MouClass");
		UNICODE_STRING HidString = RTL_CONSTANT_STRING(L"\\Driver\\MouHID");

		PDRIVER_OBJECT ClassDriverObject = nullptr;
		NTSTATUS Status = ObReferenceObjectByName(
			&ClassString, OBJ_CASE_INSENSITIVE, nullptr, 0,
			*IoDriverObjectType, KernelMode, nullptr, reinterpret_cast<PVOID*>(&ClassDriverObject));
		if (!NT_SUCCESS(Status) || !ClassDriverObject)
			return Status;

		if (!set_mouclass_exec_ranges(ClassDriverObject))
		{
			ObDereferenceObject(ClassDriverObject);
			return STATUS_UNSUCCESSFUL;
		}

		PDRIVER_OBJECT HidDriverObject = nullptr;
		Status = ObReferenceObjectByName(
			&HidString, OBJ_CASE_INSENSITIVE, nullptr, 0,
			*IoDriverObjectType, KernelMode, nullptr, reinterpret_cast<PVOID*>(&HidDriverObject));
		if (!NT_SUCCESS(Status) || !HidDriverObject)
		{
			ObDereferenceObject(ClassDriverObject);
			return Status;
		}

		PDEVICE_OBJECT matchedDevice = nullptr;
		MouseClassServiceCallback matchedCb = nullptr;
		MouseClassServiceCallback* matchedSlot = nullptr;
		g_pending_count = 0;
		g_hook_count = 0;

		for (PDEVICE_OBJECT hid = HidDriverObject->DeviceObject; hid; hid = hid->NextDevice)
		{
			scan_hid_connect_data(
				hid, ClassDriverObject,
				&matchedDevice, &matchedCb, &matchedSlot);
		}

		if (!matchedDevice)
		{
			for (PDEVICE_OBJECT cls = ClassDriverObject->DeviceObject; cls; cls = cls->NextDevice)
			{
				if (!cls->NextDevice)
					matchedDevice = cls;
			}
		}

		ObDereferenceObject(ClassDriverObject);
		ObDereferenceObject(HidDriverObject);

		if (!matchedCb
			|| !matchedDevice
			|| !callback_is_safe(reinterpret_cast<ULONG_PTR>(matchedCb)))
		{
			g_pending_count = 0;
			g_mouclass_exec_n = 0;
			return STATUS_NOT_FOUND;
		}

		ObReferenceObject(matchedDevice);
		MouseObj->MouseDevice = matchedDevice;
		MouseObj->DeviceReferenced = TRUE;
		MouseObj->ServiceCallback = matchedCb;
		MouseObj->ServiceCallbackSlot = matchedSlot;
		MouseObj->UnitId = unit_id_from_device(matchedDevice);
		MouseObj->MoveFlags = MOUSE_MOVE_RELATIVE;
		MouseObj->HaveRules = TRUE;
		MouseObj->HookInstalled = FALSE;

		return STATUS_SUCCESS;
	}
}

NTSTATUS MouseHandler::InitMouse(PMOUSE_OBJECT MouseObj)
{
	return init_mouse_locked(MouseObj);
}

void MouseHandler::ReleaseMouse(PMOUSE_OBJECT MouseObj)
{
	clear_mouse_object(MouseObj);
	g_mouse_ready = FALSE;
	g_mouclass_exec_n = 0;
	InterlockedExchange(&g_mouse_init_once, 0);
	InterlockedExchange(&g_inject_ok_count, 0);
	InterlockedExchange(&g_spoofed_down, 0);
	InterlockedExchange(&g_phys_move_accum, 0);
	InterlockedExchange(&g_phys_dx_accum, 0);
	InterlockedExchange(&g_phys_dy_accum, 0);
	InterlockedExchange(&g_phys_last_tick, 0);
	InterlockedExchange(&g_suppress_down, 0);
}

NTSTATUS MouseHandler::EnsureMouse(PMOUSE_OBJECT MouseObj)
{
	if (!MouseObj)
		return STATUS_INVALID_PARAMETER;
	if (g_mouse_ready
		&& MouseObj->ServiceCallback
		&& MouseObj->MouseDevice
		&& MouseObj->DeviceReferenced
		&& MouseObj->HaveRules
		&& callback_is_safe(reinterpret_cast<ULONG_PTR>(MouseObj->ServiceCallback)))
		return STATUS_SUCCESS;

	if (InterlockedCompareExchange(&g_mouse_init_once, 1, 0) != 0)
	{
		return (g_mouse_ready && MouseObj->ServiceCallback && MouseObj->MouseDevice)
			? STATUS_SUCCESS
			: STATUS_DEVICE_NOT_READY;
	}

	const NTSTATUS st = init_mouse_locked(MouseObj);
	if (NT_SUCCESS(st))
		g_mouse_ready = TRUE;
	else
	{
		clear_mouse_object(MouseObj);
		InterlockedExchange(&g_mouse_init_once, 0);
	}
	return st;
}

void MouseHandler::SetSuppressMask(ULONG suppress_down_flags)
{
	LONG mask = 0;
	if (MouseObject.HookInstalled && MouseObject.ServiceCallbackSlot
		&& InterlockedCompareExchange(&g_hooks_banned, 0, 0) == 0)
	{
		mask = static_cast<LONG>(
			suppress_down_flags & (MOUSE_BUTTON_4_DOWN | MOUSE_BUTTON_5_DOWN));
	}
	InterlockedExchange(&g_suppress_down, mask);
	LONG down = InterlockedCompareExchange(&g_spoofed_down, 0, 0);
	const LONG keep_bits = (mask != 0)
		? mask
		: static_cast<LONG>(MOUSE_BUTTON_4_DOWN | MOUSE_BUTTON_5_DOWN);
	InterlockedExchange(&g_spoofed_down, down & keep_bits);
}

ULONG MouseHandler::GetSuppressMask()
{
	return static_cast<ULONG>(InterlockedCompareExchange(&g_suppress_down, 0, 0));
}

ULONG MouseHandler::GetSpoofedButtonsDown()
{
	return static_cast<ULONG>(InterlockedCompareExchange(&g_spoofed_down, 0, 0));
}

BOOLEAN MouseHandler::IsHookInstalled(const MOUSE_OBJECT& MouseObj)
{
	return (MouseObj.HookInstalled && g_hook_count > 0 && MouseObj.ServiceCallback
		&& InterlockedCompareExchange(&g_hooks_banned, 0, 0) == 0)
		? TRUE : FALSE;
}

ULONG MouseHandler::GetHookCount()
{
	if (InterlockedCompareExchange(&g_hooks_banned, 0, 0) != 0)
		return 0;
	return static_cast<ULONG>(g_hook_count > 0 ? g_hook_count : 0);
}

void MouseHandler::SamplePhysicalActivity(
	ULONG& move_out,
	ULONG& idle_ms_out,
	LONG& dx_out,
	LONG& dy_out)
{
	const LONG accum = InterlockedExchange(&g_phys_move_accum, 0);
	move_out = accum > 0 ? static_cast<ULONG>(accum) : 0ul;
	dx_out = InterlockedExchange(&g_phys_dx_accum, 0);
	dy_out = InterlockedExchange(&g_phys_dy_accum, 0);

	const LONG last = InterlockedCompareExchange(&g_phys_last_tick, 0, 0);
	if (last == 0)
	{
		idle_ms_out = 60000ul;
		return;
	}
	const LONG now = tick32();
	LONG delta = now - last;
	if (delta < 0)
		delta = 0;
	idle_ms_out = ticks_to_ms(delta);
}

void MouseHandler::MoveMouse(MOUSE_OBJECT& MouseObj, long x, long y, unsigned short button_flags)
{
	if (!MouseObj.ServiceCallback || !MouseObj.MouseDevice || !MouseObj.DeviceReferenced)
	{
		if (!NT_SUCCESS(EnsureMouse(&MouseObj)))
			return;
	}
	if (!MouseObj.ServiceCallback || !MouseObj.MouseDevice || !MouseObj.DeviceReferenced)
		return;

	if (!callback_is_safe(reinterpret_cast<ULONG_PTR>(MouseObj.ServiceCallback)))
	{
		ban_hooks_keep_inject();
		ReleaseMouse(&MouseObj);
		return;
	}

	MOUSE_INPUT_DATA mid{};
	mid.UnitId = MouseObj.UnitId;
	mid.Flags = MouseObj.MoveFlags;
	mid.LastX = x;
	mid.LastY = y;
	mid.ButtonFlags = button_flags;
	mid.ButtonData = 0;
	mid.RawButtons = 0;
	mid.ExtraInformation = 0;

	KIRQL oldIrql = PASSIVE_LEVEL;
	KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);
	ULONG inputDataConsumed = 0;
	BOOLEAN failed = FALSE;

	ObReferenceObject(MouseObj.MouseDevice);
	__try
	{
		MouseObj.ServiceCallback(
			MouseObj.MouseDevice,
			&mid,
			&mid + 1,
			&inputDataConsumed);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		failed = TRUE;
	}
	ObDereferenceObject(MouseObj.MouseDevice);
	KeLowerIrql(oldIrql);

	if (failed)
	{
		ban_hooks_keep_inject();
		ReleaseMouse(&MouseObj);
		return;
	}

	const LONG oks = InterlockedIncrement(&g_inject_ok_count);
	if (oks == k_hook_after_injects
		&& InterlockedCompareExchange(&g_hooks_banned, 0, 0) == 0
		&& !MouseObj.HookInstalled)
	{
		install_pending_hooks(&MouseObj);
	}
}
