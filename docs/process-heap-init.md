# Process type and critical-section initialization

The diagnostic hosts one user process. `KeGetCurrentProcessType` at the verified Free Riders import address `0x82ACB59C` reports type 1. The game stores that byte in its heap metadata after the call at LR `0x824E14B8`. This does not provide a process information block, process switching, or thread scheduling.

The reference is Xenia revision `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`: [kernel_state.h](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/kernel_state.h) defines user type 1 and initializes `process_type_` to it; [kernel_state.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/kernel_state.cc) copies it into the process information block; [xboxkrnl_threading.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc) returns the active process type. Both locally pinned Marathon and Unleashed references return 1 for their user process. Free Riders has no static `KeSetCurrentProcessType` import; unprovided dynamic calls remain diagnostic stops.

The next observed import is `RtlInitializeCriticalSection` at `0x82ACB49C`, called with LR `0x824E150C`. Its 28-byte guest layout and initializer follow the same pinned Xenia [xboxkrnl_rtl.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc); the 16-byte dispatch header fields are defined in [xobject.h](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xobject.h).

| Offset | Width | Initialized value |
| --- | --- | --- |
| 0 | 1 | Type = 1 |
| 1 | 1 | Absolute / spin count = 0 |
| 4 | 4 | Signal state = 0 |
| 16 | 4 | Lock count = -1 (`FF FF FF FF`) |
| 20 | 4 | Recursion count = 0 |
| 24 | 4 | Owning thread = 0 |

Bytes 2–3 and 8–15 are preserved, as are surrounding bytes. The entire 28-byte range is checked before any write, including import-variable guards and committed-memory bounds. Stores use the checked guest big-endian interface. The import has a void ABI in the pinned Xenia source; dispatch preserves guest registers instead of manufacturing a success return value. This differs from the return-zero convention in the reference recompilations.

Initialization only creates guest data. The subsequent [locking increment](critical-section-locking.md) supports single-thread ownership, Enter/Leave transitions and spin-count initialization. Waiting, wakeup and contention remain unsupported. No host lock object is created by this initializer, and reinitialization of an active lock is outside the supported execution path.

Synthetic native tests cover exact bytes, preserved fields, repeated initialization, committed boundaries and failure without partial writes. The actual-image integration test confirms that the original entry crosses both imports and reaches the next recorded dependency. Windows is the only verified host.

The verified initializer argument is `0x40000618`. At commit `93cf166`, execution reached `sub_824DF608` before stopping on `XboxHardwareInfo` at `0x82000798`, LR `0x824DD01C`, with 20 function entries and diagnostic exit code 3. The subsequent [hardware-info increment](hardware-info.md) advances to `RtlEnterCriticalSection`. Neither result verifies complete heap initialization or the game.
