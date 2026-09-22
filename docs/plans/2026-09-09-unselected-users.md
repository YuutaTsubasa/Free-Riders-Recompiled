# Original initialization with no selected user

Current0a3a780 realboot stops at XamUserGetSigninState82ACB32C, LR822344D8.
The one-argument query must reflect the current lack of any selected native
game profile. Windows login and Winsock startup prove no game account state.
Provide an explicitly unselected-user backend for the four ordinary slots;
reject unaudited special indices. Do not invent XUID/name/Live state or perform
selection inside a query. Replace this absence backend when a real local
profile selection service is implemented; it is not a permanent sign-in policy.

Keep the original state0 branch, original virtual reset and record/name clearing.
First test the missing query and index bounds, then execute the original entry.
Audit the next text conversion independently before binding it. In particular,
do not assume host ANSI codepage equals this Xbox library's character mapping.
Actual query result, original zero-state storage and unselected manager state
must be observed from original execution, not produced by an observation hook.
No title/menu completion claim without original rendering/input evidence.

Actual firstquery passes, then original virtualreset completes and reaches
RTL82ACB6BC fromLR824D0E3C: dest701398CC,capacity32,countoutput0,
source701398BC,inputbytes1. Add a bounded ASCII-to-UTF16BE conversion preserving
explicit lengths/embeddedNUL/truncation/oddcapacity and optional BE32 bytecount.
Only consumedASCIIbytes are supported until Xbox ACP evidence exists. The
original wrapper selects a separate UTF8 route for65001, so hostACP/UTF8/Latin1
must not be substituted. Unsupportedencoding, aliases, memoryguards and 1Miunit
worklimit fail before publication. Current source/dest are separate; overlapping
buffers remain a documented unsupported contract rather than guessed expansion.
The core conversion and tests are delegated to the existing runtime agent;
root owns build/main evidence. RED has been verified before GREEN implementation.

Completed2026-09-10: allfour original queries return0, original virtual82234EC0
returns, record/name/profile clear and selected-1 verified, RTL emits2bytes/status0
and original wrapper returns1 per slot. A later call to already implemented
depth/cull setters exposed duplicated LR gates. Full-function ABI review found
no caller dependence, so final policy uses exact seven-entry recognition while
retaining all device/value/cache/attachment/constant checks; partial bridges
are unchanged. The original stack-only82A560BC's six words were independently
verified before admitting its epilogue with a device value remaining inr3.
Actual subsequent calls retain depth1/comparison6/write1/cull6. Next actual stop
824E6A40 LR8221171C value1. Full63 CTest/192 Python tests pass without skips;
independent reviews found no concrete defect. Title/menu goal remains active.
