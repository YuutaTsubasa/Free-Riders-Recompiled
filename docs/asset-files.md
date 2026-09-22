# Original game opens the basic shader asset

The verified original entry now opens, queries, reads and closes `game:\shader\Xbox360BasicShader.fxobj` through the native file adapter. All 93,676 bytes are in the original game buffer and match the local asset hash. The original title/menu objective remains incomplete; this is asset loading, not a rendered game frame.

## Current result

`out/lhzu-native-boot.log` follows the same read/close through all68 native shader
resources, original device AddRef and effect initialization. Checked lhzu now
completes all four original vertex declarations with verified metadata/elements.
Original device capture and native semaphore/event creation and clearing also
complete; next stop isExCreateThread, at1944entries, LR824D2518.
See [original synchronization initialization](native-sync-objects.md). No original draw,
Present, title or menu is established.

## Original effect-parser checkpoint 6f47f1c

After read/close checkpoint `2274f32`, the original `0x825E7F98` effect loader now runs with the native device. Its complete body was independently audited: it only compares, stores and passes that device value; it does not dereference its Xbox layout. The exception is restricted to this entry and the known device. Its `0x82A56044` stack-save helper independently ignores r3 and is admitted without modifying its original code. All downstream native-object checks remain, including `0x824F44F8`.

`out/original-effect-entry-final-boot.log` proves the original parser requests vertex shader creation at `0x824ED770`, container `0x40019B18`, words `102A1101,178,DC`,596bytes,SHA-1 `0aa2cf2beb066de65d5cb9bb9b26bb1110487192`. These bytes match asset offset `0x1124` and the previously translated first vertex shader. The optional read-only trace catches inaccessible inspection and does not force an earlier error before an original callback could return.

The actual stop is unsupported `0x824D1858` (vector-memory/unchecked-memory diagnostic) called from `0x824ED808` inside original vertex-shader resource initialization, after1361entries. `LAST_FUNCTION` names the previous entered `0x82A56710`, because the stopping stub does not enter its body. The next required native adapter is shader creation at the now-observed library boundary. No shader creation completion, original draw, Present or title/menu is proved.

Verification: `out/original-effect-entry-red.log`, `out/original-effect-prologue-red.log`, `out/original-shader-trace-red.log`, `out/original-effect-entry-final-build.log`, `out/original-effect-entry-final-boot.log`, `out/original-effect-entry-tests-final.log` (104 Python, no skips), `out/original-effect-entry-ctest-final.log` (24/24), `out/original-effect-entry-dependencies.log` (both pins).

The exact wrapper/helper exceptions were independently audited and the final guard/trace change passed separate quality review. Negative guard values and unavailable-inspection cases were inspected rather than directly exercised by the new original-entry assertions.

## Content-read checkpoint 2274f32

`out/file-read-close-boot.log` records an actual transfer into guest buffer `0x40001BA0`, requested93676/transferred93676, starting offset0, status0, SHA-1 `96b652112909d62121e25f2fdb24bfd3b23869d2`. This hash is calculated from the guest buffer after copying and matches the separately checked local asset. Original NtClose then releases handle `0x72000004` through actual CloseHandle. The original caller proceeds to `0x825E7F98` with the native device, loaded FXOBJ buffer and output pointer; the conservative graphics guard stops there after1297entries, LR `0x824B7A44`. Shader creation/draw/Present and confirmation input are still incomplete.

AssetFiles uses synchronous [ReadFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile) on retained native handles. Null offsets and the explicit -2 current-position marker use the native position; nonnegative explicit offsets seek before reading. Positive partial transfers succeed, zero bytes at EOF return `0xC0000011`, and a valid zero-length request returns success without moving the file pointer. Invalid/closed handles return `0xC0000008`; other negative offsets and unexpected native failures stop explicitly. A temporary byte buffer is allocated before seeking.

GuestFiles rejects event/APC/context modes, checks the complete writable buffer and optional IO block, rejects overlap and validates a supplied offset before native reads. It copies exactly the transferred bytes through checked GuestMemory stores, preserving existing write-combine barriers and untransferred memory. Optional IO contains BE32 status/byte count. The actual eight-argument Xbox import is bound at `0x82ACB7BC`. The known-file NtClose binding at `0x82ACB62C` returns success only after native close; other kernel objects and duplicate closes remain explicit stops.

Tests cover exact binary data including NUL/high-bit bytes, independent file positions, explicit/current offsets, partial/EOF/zero reads, invalid memory before position changes, optional IO and successful exclusive reopen after guest close. RED logs: `out/file-read-unit-red.log`, `out/file-read-guest-red.log`, `out/file-read-boot-red.log`, `out/file-close-guest-red.log`, `out/file-close-boot-red.log`. Final build/targeted/full evidence: `out/file-read-close-build.log`, `out/file-read-close-targeted.log` (3/3), `out/file-read-close-ctest-final.log` (24/24), `out/file-read-close-tests-final.log` (104 Python, no skips), `out/file-read-close-dependencies.log` (both pins).

Independent specification and quality reviews approved the read/close implementation.

## File-information checkpoint 549c762

The class34 adapter queries `FILE_BASIC_INFO` and `FILE_STANDARD_INFO` through [GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex) on the retained handle. It preserves all four separate timestamps, actual allocation and file sizes, and attributes. It encodes six BE64 fields, BE32 attributes and zero reserved padding into the original 56-byte structure. Optional IO receives status0/information56. Short buffers and invalid handles return the verified NT errors without changing outputs; other classes, oversized buffers and invalid or overlapping output ranges stop explicitly. The current bounded contract accepts the observed exact56-byte request.

`out/file-information-boot.log` records EOF93676 and actual allocation94208. The original game then allocates its read buffer and calls NtReadFile with handle72000004, event/APC/context0, IO70131330, buffer40001BA0, length16DEC (93676), and null byte-offset pointer. It stops at `0x82ACB7BC`, LR `0x824D1388`, after1293entries. Content has not yet been read; the next implementation must fill this guest buffer from the retained native file and preserve synchronous position/EOF semantics.

Independent native tests compare every metadata field against Windows, grow a test file after opening, and set distinct native timestamps to verify refreshed data and separate change/write times. Guest tests compare each BE byte to independently queried metadata, verify the exact output boundary, optional IO and rejection atomicity. Logs: `out/file-information-guest-red.log`, `out/file-information-host-red.log`, `out/file-information-boot-red.log`, `out/file-information-build.log`, `out/file-information-targeted.log`, `out/file-information-ctest-final.log` (23/23), `out/file-information-tests-final.log` (104 Python, no skips), and `out/file-information-dependencies.log` (both pins).

The combined host/guest specification review and independent quality review approved this file-information change.

## Original file-open checkpoint 698e654

The original wrapper `0x824D0F28` supplies NtCreateFile `0x82ACB58C` with access `0x80100080`, null allocation size, file attributes 0, sharing 0, disposition 1 and options `0x60`. The ninth argument is a checked BE32 stack word at SP+84. The 12-byte object attributes contain root `0xFFFFFFFD`, an eight-byte counted ANSI descriptor pointer and flags `0x40`. The output IO block is eight bytes containing BE32 status and information.

`out/asset-open-context.log` records these values before implementation. The ABI agrees with [pinned Xenia's implementation](https://raw.githubusercontent.com/xenia-project/xenia/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc). The local Marathon and Unleashed NtCreateFile imports are stubs and provide no functional file-open implementation to reuse.

`out/asset-open-boot.log` records the new original execution:

```text
FILE_OPEN_CONTEXT root=0xfffffffd name=0x70131318 attributes=0x40 options=0x60
RESULT NtCreateFile status=0x0 handle=0x72000004 size=93676 path=game:\shader\Xbox360BasicShader.fxobj
IMPORT_CONTEXT name=__imp__NtQueryInformationFile address=0x82acb66c lr=0x82a70838 sp=0x70131250 r3=0x72000004 r4=0x701312a0 r5=0x701312b0 r6=0x38 r7=0x22 r8=0x0 r9=0x0 r10=0x1
STOP import-function @0x82acb66c: __imp__NtQueryInformationFile
LAST_FUNCTION sub_82A70800 @0x82a70800 LR=0x82a70838 calls=1270
```

The local asset SHA-256 remains `b4a865e23ecb7eed2770233d9b865923180d084a483b6d2f0c44cf54d73848b2`, matching the prior shader intake. The runtime opens this existing file and queries its actual size from the retained native handle; it does not return a synthetic file or cached size constant.

## Native ownership and guest adapter

AssetFiles retains the mounted directory handle and each opened file. [CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) uses GENERIC_READ and OPEN_EXISTING with the requested read/write/delete sharing bits. Share 0 provides an actual exclusive open. Directory targets are rejected. Successful guest IDs begin at `0x72000004`, increase by four and are never reused during the owner's lifetime. They are opaque IDs, not host pointers or mapped guest structures. Closing or destroying the owner releases native handles.

Only an ASCII `game:/` or `game:\` path is accepted. Traversal, empty components, embedded NUL, other mounts, alternate data streams, wildcards and unsupported Windows characters are rejected. The final path is obtained from the opened handle and compared against the retained root's final path with a component boundary. Thus a junction leading outside the mount is rejected, while an in-root junction can resolve normally. [GetFinalPathNameByHandleW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew) supplies the resolved object path. Tests exercise both junction cases and verify rejected handles are closed.

GuestFiles supports the observed synchronous read-only request, root 0 or `0xFFFFFFFD`, case-insensitive attributes `0x40` and sharing bits 0..7. It checks both output ranges, rejects overlapping outputs, validates the full object and counted name, and checks supported modes before opening a host file. The filename may end at its counted length without a NUL. Success writes the BE handle, status 0 and FILE_OPENED information 1. Known missing-file/path, access and sharing failures return corresponding NT statuses and an invalid handle; unimplemented modes and unknown host failures stop explicitly. Files are never created, overwritten or truncated.

The diagnostic remains single-threaded. This does not implement general VFS mounts, asynchronous requests, saves, directory enumeration, general kernel-object close operations. Synchronous guest file reads and known-file close are now implemented. The original NtClose import now uses the host close method for known file objects, as described above.

## File-information dependency observed at 698e654

Original function `0x82A70800` requests class `0x22` with a 56-byte output, then reads the BE64 value at output+40 after success. That is the EndOfFile slot of [FILE_NETWORK_OPEN_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_network_open_information). The adapter described above now obtains these fields from the retained native object, including an actual change timestamp distinct from last-write time.

## Reproduction and evidence

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-subfze
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
ctest --test-dir out/build/host --output-on-failure
```

Exit3 currently denotes `ExCreateThread` after all68 shader creations, original effect initialization, four verified original vertex declarations and native semaphore/event initialization. Omitting the asset argument stops explicitly at the first file request. The asset root is optional for inspecting earlier image-validation failures and must be supplied to continue real file loading. Full Python testing additionally sets `SFR_ASSET_DIRECTORY` along with the image, XEX and native executable variables in README.

Recorded RED: `out/asset-open-boot-red.log` (missing real-open event) and `out/asset-open-unit-red.log` (unimplemented AssetFiles/GuestFiles symbols). Build: `out/asset-open-build.log`. Verification: `out/asset-open-targeted.log` (2/2), `out/asset-open-ctest-final.log` (23/23), `out/asset-open-tests-final.log` (104 Python tests, no skips), and `out/asset-open-dependencies.log` (both fixed dependencies verified). Independent host and guest specification reviews and the combined quality review approved the implementation. All assets, generated code, executable and logs remain local and excluded from Git.
