# XboxHardwareInfo flags prefix

The verified Free Riders import table contains `__imp__XboxHardwareInfo` at `0x82000798`. This slot holds the guest address of hardware data, not a function or an additional pointer-to-pointer. The diagnostic binds the exact name/address pair to `0x71200000` after verifying the image, import table and original XEX header.

Only four bytes are mapped: the big-endian flags word `00 00 00 20`. This is the profile used by pinned Xenia revision `95a5c3ee250f80c3b9d139658649d9ffb6db3eec` in [xboxkrnl_module.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc). Xenia allocates a 16-byte object, sets flags to `0x20`, sets the CPU count byte at offset4 to6, and explicitly expresses uncertainty about the remaining11bytes. The diagnostic exposes only the observed flags prefix; even the CPU count is not supplied until needed by a verified call path.

The locally pinned Marathon and Unleashed `kernel/imports.cpp` entries for this symbol only log a stub. They are not evidence for a hardware layout or a usable implementation.

Free Riders `sub_824DF608` loads the import pointer, reads the word at offset0 and masks `0x10`. With the selected flags profile that bit is clear, so the guest's existing branch to `0x824DF684` is taken. The diagnostic does not patch the branch or claim physical hardware capabilities. Xenia's comments suggest a storage-related meaning for `0x20`, but this increment does not provide a disk, a device service or debug hardware behavior.

GuestMemory retains a four-byte logical extent despite host page rounding. Reads or writes crossing offset4, or preceding the mapping, stop before touching memory. Duplicate or conflicting initialization fails without overwriting existing data. All other unsupported variable imports keep their guards. This is a writable diagnostic data prefix under the existing GuestMemory model, not a general kernel data or read-only memory implementation.

Synthetic tests verify the exact bytes, bitmask, boundary failures without partial writes and collision behavior. Actual-entry integration verifies the binding and progress beyond the former stop. Only the Windows host has been built and executed.

At commit `14f8f32`, the trace reached `sub_824DCE98` and stopped at `RtlEnterCriticalSection` (`0x82ACB4AC`, LR `0x824DCEB8`, calls22, exit3). The subsequent [locking increment](critical-section-locking.md) advances past the Enter/Leave pair. These observations do not verify complete startup.
