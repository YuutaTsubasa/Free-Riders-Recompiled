# Original thread creation processor mask

Historical checkpoint `180c76c`; latest progression is [executable module query](executable-module-query.md).

The Windows original title/menu goal remains **unachieved**. At checkpoint
`2e6acca`, the original game requests suspended creation with flags `08000001`.
The upper byte is a logical processor mask, not a processor index.

## Evidence and supported profile

The original ROM instructions at `824D45C4` (`7E6B3830`, `slw r11,r19,r7`) and
`824D45C8` (`5169C00E`, `rlwimi r9,r11,24,0,7`) form the mask from the configured
CPU before calling ExCreateThread at `824D45E0` (`485F719D`). The original code
sets r19 to one. The actual observed request therefore selects guest CPU 3.

[Pinned Xenia XThread](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xthread.cc)
decodes the upper byte as a single logical-processor bit and updates PCR,
KTHREAD and host affinity before resume. Our zero-mask behavior retains the
existing parent inheritance, consistent with [Microsoft's Xbox 360 multicore
guidance](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/coding-for-multiple-cores).

The bounded extension accepts low 24 bits exactly one (suspended creation) and
upper byte zero or one of `01/02/04/08/10/20`. Other flags, multi-processor masks
and nonexistent processors remain unsupported before allocation/publication.
All original flags are preserved in big-endian KTHREAD+16C. The selected CPU
must agree in PCR+10C, KTHREAD+BF and the actual Windows thread affinity.

The existing native adapter maps guest CPU IDs onto the process's allowed
processors in its validated processor group. It binds to one actual host
processor, using the guest index modulo the available host count. The original
guest still sees its requested CPU ID. This does not emulate Xbox CPU timing.

The original startup `824D2A10` calls the supplied worker with its original
argument and then calls ExTerminateThread if it returns. Worker `824D4388`
branches to original `824D42A8`. Its queue and synchronization operations must
run through supported original/runtime paths; no queue completion is invented.

## Actual native run

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-doubleword
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

The captured `out/creation-affinity-boot.log` ends with exit 3:

```text
RESULT ExCreateThread output=0x82b50140 handle=0x72200024 status=0x0 startup=0x824d2a10 worker=0x824d4388 argument=0x82b4ffc8 flags=0x8000001 pcr=0x74000000 thread=0x74001000 tls=0x74002000 stack_limit=0x74021000 stack_base=0x74061000 guest_id=10 native_id=28108 suspended=1 entry_started=0 backend=windows-thread
THREAD_CREATION_PROCESSOR guest_id=10 parent_cpu=0 pcr_cpu=3 thread_cpu=3 host_mask=0x8
RESULT NtResumeThread handle=0x72200024 output=0x0 status=0x0 previous=1 guest_id=10 backend=windows-thread
ORIGINAL_WORKER_BEGIN guest_id=10 host_thread=28108 startup=0x824d2a10 worker=0x824d4388 argument=0x82b4ffc8 pcr=0x74000000
WORKER_ENTER guest_id=10 name=sub_824D2A10 address=0x824d2a10
WORKER_ENTER guest_id=10 name=sub_824D4388 address=0x824d4388
WORKER_ENTER guest_id=10 name=sub_824D42A8 address=0x824d42a8
WORKER_ENTER guest_id=10 name=sub_824DD718 address=0x824dd718
ORIGINAL_WAIT_REQUEST guest_id=10 handle=0x7210002c mode=0x1 alertable=0x0 timeout_ptr=0x0
STOP import-function @0x82acb73c: __imp__XexGetModuleHandle
LAST_FUNCTION sub_824D1ED0 @0x824d1ed0 LR=0x824d1ee4 calls=3113
```

Native IDs and host masks depend on the host/run; the CPU fields and original
entries are checked independently. There are nine created native workers, five
resumed. The new worker genuinely waits on its unsignaled event until runtime
cancellation; no successful wait completion is reported. Main-thread execution
then reaches a module-handle query with name r3=0 and output r4=`701316B0`.
That unsupported import is the next dependency; original rendering and menu
input are still missing.

## Validation

The new native test failed on the prior `flags == 1` implementation, then
passed after implementing the processor mask. It covers all six inherited and
explicit selections, matching native affinity, original BE fields, parked
callbacks, and invalid flags with unchanged allocations/outputs. Existing
factory/publication rollback tests remain. Full results are 34/34 CTest and
136 Python tests, no skips, including true-ROM worker/CPU/wait evidence.

Logs: `out/creation-affinity-red-build.log`, `out/creation-affinity-red.log`,
`out/creation-affinity-build.log`, `out/creation-affinity-green.log`,
`out/creation-affinity-ctest.log`, `out/creation-affinity-python.log`.
