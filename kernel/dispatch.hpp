#pragma once

#include <wdm.h>

// Drop into your IRP_MJ_DEVICE_CONTROL handler:
//   if (MouseSpoofDispatchIoctl(Irp, code, input_length, &status, &info))
//       goto complete;
BOOLEAN MouseSpoofDispatchIoctl(
	PIRP Irp,
	ULONG ioctl,
	ULONG input_length,
	NTSTATUS* status_out,
	ULONG_PTR* info_out);

void MouseSpoofOnUnload();
