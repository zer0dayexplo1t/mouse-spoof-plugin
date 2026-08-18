# Mouse spoof plugin

Standalone kernel + usermode component for **class-driver mouse injection** and **selective physical-button spoofing** on Windows.

It talks to the same path the HID mouse stack already uses (`MouHID` → `MouClass` → `MouseClassServiceCallback`). Applications and games see injected packets as ordinary mouse input. Selected side-buttons can be stripped from that stream so the rest of the OS never receives them, while this component still knows whether those buttons are held.

This is not `SendInput`, not a WH_MOUSE hook, and not a usermode raw-input filter.

---

## What it does

Windows mouse input normally flows:

1. USB/HID interrupt → `MouHID.sys`
2. `MouHID` looks up a `CONNECT_DATA` record in its device extension: a `MouClass` device object plus a `ClassService` function pointer
3. `MouClass`’s service callback consumes `MOUSE_INPUT_DATA` packets (`LastX` / `LastY` / `ButtonFlags`)
4. Win32 / DirectInput / Raw Input / the foreground app observe the result

This plugin inserts itself at step 2–3.

### 1. Discover the live MouClass callback

On first use (not at `DriverEntry`) it:

- Resolves `\Driver\MouClass` and `\Driver\MouHID` via `ObReferenceObjectByName`
- Collects **executable PE sections** of `MouClass` (not the whole image — NX data is ignored)
- Walks each MouHID device extension looking for a `{device, ClassService}` pair where:
  - the device is the attached MouClass DO, or any MouClass DO
  - `ClassService` points into those executable sections
- References the matched class device, learns `UnitId` from the object name, and stores the original callback for injection

If discovery fails, inject is unavailable until the next successful `EnsureMouse`.

### 2. Inject relative moves and buttons

Usermode sends `{dx, dy, ButtonFlags}` over IOCTL. The kernel:

- Adds a wire bias so a click is never a raw `{0,0}` packet
- Builds a `MOUSE_INPUT_DATA` (relative move, optional `MOUSE_LEFT_BUTTON_DOWN/UP`, etc.)
- Raises IRQL to `DISPATCH_LEVEL` (the class service is called at dispatch)
- Calls the **original** `MouseClassServiceCallback` with that single packet

The cursor and click land in the same place as a real HID report. No `SendInput`, no `mouse_event`, no injected usermode messages.

### 3. Spoof physical XBUTTON1 / XBUTTON2

After **three successful injects**, pending `CONNECT_DATA.ClassService` slots are patched to a trampoline:

- Incoming HID packets still update internal hold state for mouse4 / mouse5
- Absolute motion (`LastX`/`LastY`) is accumulated for a physical-activity sample
- If a suppress mask is set, `MOUSE_BUTTON_4_*` and/or `MOUSE_BUTTON_5_*` flags are **cleared** before the original callback runs

Effect:

| Who | Mouse4 / Mouse5 |
|---|---|
| This component (`key_down` / `ButtonsDown`) | sees the real hold |
| Games, overlays, Win32 `GetAsyncKeyState`, Raw Input | do **not** see the button while it is suppressed |

Left/right/middle buttons and movement are not stripped. Injected LMB still goes through the original callback.

The suppress mask is applied only while the hook is actually installed. Querying without `SetMask` returns current mask, hold bits, hook status, and physical deltas.

### 4. Physical HID sample

Every real packet (before strip) contributes:

- `PhysicalMove` — abs(`LastX`)+abs(`LastY`) since last query (clamped)
- `PhysicalDx` / `PhysicalDy` — signed deltas since last query (consumed on read)
- `PhysicalIdleMs` — time since last non-zero HID packet

Usermode turns that into a short-lived activity sample (`live`, `mag`, `activity` 0..1). If the hook is missing or the ABI looks stale, it falls back to `GetCursorPos` deltas so a blend path still has a motion signal.

Typical use: scale synthetic corrections with **real** mouse motion so injected movement does not look like a cursor sliding with a dead physical mouse.

### 5. Safety policy

- **No hook at load.** Discovery stores pending slots; patching waits until inject has succeeded three times.
- Callback targets are only accepted inside MouClass executable sections (or the trampoline itself).
- If the original callback pointer is no longer in those ranges, or the inject `__try` faults: uninstall every hook, clear suppress, **ban re-hook for the rest of this driver map**. Inject may still be retried after a full re-init; CONNECT_DATA patching will not.
- Unload restores original `ClassService` pointers and drops the device reference.

---

## What it can be used for

Anything that needs **kernel-class mouse packets** and/or **side-buttons that your code sees but the target application does not**.

- **Bind keys that the target must not receive.** Map a feature to mouse4/mouse5, suppress those buttons, keep reading hold via `key_down`. The game never gets a “mouse4 pressed” event; your overlay still does.
- **Click / flick injection that bypasses usermode input APIs.** Useful when `SendInput` is filtered, rate-limited, or attributed to the injecting process.
- **Relative aim/assist that coexists with the user.** Sample physical HID, then inject only a fraction of a correction while the user is actually moving the mouse. When they stop, activity decays and inject can be gated.
- **Hold-to-activate features** (trigger, stabilizer, overlay modifiers) on XBUTTON without stealing those buttons from Windows globally via a LL hook.
- **Health check of the mapped driver.** `Abi` / hook-count / `hooked` tell usermode whether this build of the driver is live, versus an old image that does not speak the spoof IOCTL.

It is **not** a full mouse filter driver, not a PS/2 i8042 hook, and not a replacement for Raw Input in your own process. It only sees what MouHID delivers to MouClass.

---

## Kernel integration

Add to the driver project:

- `kernel/mouse.cpp`
- `kernel/dispatch.cpp`
- include paths: `kernel/` and `include/`

In `IRP_MJ_DEVICE_CONTROL`:

```cpp
NTSTATUS status;
ULONG_PTR info = 0;
if (MouseSpoofDispatchIoctl(Irp, ioctl, inputLength, &status, &info))
{
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
```

On unload: `MouseSpoofOnUnload();`

Requirements: WDK (`ntddmou.h`, `ntimage.h`), link `ntoskrnl.lib`. `IoDriverObjectType` is declared in `kernel/ntundoc.hpp` (exported by ntoskrnl).

IOCTL codes default to `0xE5B9` (move) and `0x9B18` (spoof). Change them in `include/mouse_spoof_abi.hpp` if they collide with your driver.

---

## Usermode integration

```cpp
#include "usermode/mouse_client.hpp"
#include "usermode/mouse_spoof.hpp"

mouse_spoof::bind(driverHandle);
mouse_spoof::set_suppress_mask(MOUSE_BUTTON_4_DOWN); // hide mouse4 from other apps
mouse_spoof::sync();

mouse_client::move(driverHandle, dx, dy);
mouse_client::left_click(driverHandle);

if (mouse_spoof::key_down(VK_XBUTTON1)) { /* still true while suppressed */ }
const auto phys = mouse_spoof::sample_physical();
// phys.hooked, phys.live, phys.dx/dy, phys.activity
```

Call `sync()` when the suppress mask changes (e.g. user toggles which bind uses mouse4). Call `sample_physical()` on your tick; deltas are consumed in the kernel on each query.

---

## Protocol

Move request: `X`/`Y` are `dx + MOUSE_SPOOF_MOVE_BIAS` (bias `52347`). `ButtonFlags` are `ntddmou` flags (`MOUSE_LEFT_BUTTON_DOWN` = `0x1`, …, `MOUSE_BUTTON_4_DOWN` = `0x40`, `MOUSE_BUTTON_5_DOWN` = `0x100`).

Spoof request:

| Field | In | Out |
|---|---|---|
| `SetMask` | `1` = apply `SuppressMask` | — |
| `SuppressMask` | bits to strip | bits actually armed (0 if hook not live) |
| `ButtonsDown` | — | physical XBUTTON hold (DOWN flags) |
| `HookInstalled` | — | bit0 hooked; bits 8–15 hook count; high word `0x4D53` ABI tag |
| `PhysicalMove` / `Dx` / `Dy` / `IdleMs` | — | HID activity since last query |
| `Abi` | — | `0x4D530003` layout stamp |

Bump `MOUSE_SPOOF_ABI` only when this struct layout changes.

---

## Files

| Path | Role |
|---|---|
| `include/mouse_spoof_abi.hpp` | Shared structs + IOCTL codes |
| `kernel/ntundoc.hpp` | `ObReferenceObjectByName` / `IoDriverObjectType` |
| `kernel/mouse.hpp` / `mouse.cpp` | Discovery, inject, CONNECT_DATA hook |
| `kernel/dispatch.hpp` / `dispatch.cpp` | IOCTL glue |
| `usermode/mouse_client.hpp` | `DeviceIoControl` helpers |
| `usermode/mouse_spoof.hpp` | Bind hold + physical sample |
