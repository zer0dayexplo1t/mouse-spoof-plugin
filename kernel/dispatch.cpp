#include "dispatch.hpp"
#include "mouse.hpp"
#include "../include/mouse_spoof_abi.hpp"

BOOLEAN MouseSpoofDispatchIoctl(
	PIRP Irp,
	ULONG ioctl,
	ULONG input_length,
	NTSTATUS* status_out,
	ULONG_PTR* info_out)
{
	if (!Irp || !status_out || !info_out)
		return FALSE;

	if (ioctl == MOUSE_SPOOF_IOCTL_MOVE)
	{
		if (input_length != sizeof(MOUSE_SPOOF_MOVE_REQUEST))
		{
			*status_out = STATUS_INFO_LENGTH_MISMATCH;
			*info_out = 0;
			return TRUE;
		}
		auto* req = static_cast<PMOUSE_SPOOF_MOVE_REQUEST>(Irp->AssociatedIrp.SystemBuffer);
		if (!NT_SUCCESS(MouseHandler::EnsureMouse(&MouseObject)))
		{
			*status_out = STATUS_DEVICE_NOT_READY;
			*info_out = 0;
			return TRUE;
		}
		MouseHandler::MoveMouse(
			MouseObject,
			req->X - MOUSE_SPOOF_MOVE_BIAS,
			req->Y - MOUSE_SPOOF_MOVE_BIAS,
			req->ButtonFlags);
		*status_out = STATUS_SUCCESS;
		*info_out = sizeof(MOUSE_SPOOF_MOVE_REQUEST);
		return TRUE;
	}

	if (ioctl == MOUSE_SPOOF_IOCTL_SPOOF)
	{
		if (input_length < sizeof(MOUSE_SPOOF_REQUEST))
		{
			*status_out = STATUS_INFO_LENGTH_MISMATCH;
			*info_out = 0;
			return TRUE;
		}
		auto* req = static_cast<PMOUSE_SPOOF_REQUEST>(Irp->AssociatedIrp.SystemBuffer);
		(void)MouseHandler::EnsureMouse(&MouseObject);
		if (req->SetMask)
			MouseHandler::SetSuppressMask(req->SuppressMask);
		req->SuppressMask = MouseHandler::GetSuppressMask();
		req->ButtonsDown = MouseHandler::GetSpoofedButtonsDown();
		req->HookInstalled = MouseHandler::IsHookInstalled(MouseObject) ? 1u : 0u;
		{
			const ULONG hooks = MouseHandler::GetHookCount() & 0xFFu;
			const ULONG hooked_bit = req->HookInstalled ? 1u : 0u;
			req->HookInstalled = hooked_bit | (hooks << 8) | 0x4D530000u;
		}
		req->HookCount = MouseHandler::GetHookCount();
		req->Abi = MOUSE_SPOOF_ABI;
		ULONG phys = 0, idle = 60000;
		LONG pdx = 0, pdy = 0;
		MouseHandler::SamplePhysicalActivity(phys, idle, pdx, pdy);
		req->PhysicalMove = phys;
		req->PhysicalIdleMs = idle;
		req->PhysicalDx = pdx;
		req->PhysicalDy = pdy;
		*status_out = STATUS_SUCCESS;
		*info_out = sizeof(MOUSE_SPOOF_REQUEST);
		return TRUE;
	}

	return FALSE;
}

void MouseSpoofOnUnload()
{
	MouseHandler::ReleaseMouse(&MouseObject);
}
