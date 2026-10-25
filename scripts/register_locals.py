"""Compile the generated code with guest registers in C++ locals.

XenonRecomp can emit some PPC registers as locals of each generated function
instead of fields of the PPCContext in memory (its ctr_as_local, xer_as_local,
cr_as_local, non_argument_as_local and non_volatile_as_local options, which
Unleashed Recompiled ships with). A local can live in a host register; a
context field must be stored before every call and loaded again after it,
because every call may read and write the whole context.

The shipped generated code is not regenerated (see loop_head_checkpoints.py),
and its generator rewrites many instructions in their `ctx.` form, so this
does the same conversion on the finished output instead, at build time, by
classes of registers (CMake option SFR_REGISTER_LOCALS):

  ctr          the count register
  xer          the fixed-point exception register (see XER below)
  cr           cr0..cr7, tracked bit by bit
  nonargument  r0, r2, r11, r12, f0, v32..v63 (XenonRecomp's set)
  nonvolatile  r14..r31, f14..f31, v14..v31, v64..v127 (XenonRecomp's set)

Not converted: LR (host code and the HUD read return addresses, skip_lr),
MSR (global state: one function's mtmsrd is another's mfmsr), the reservation
(ctx.reserved holds the thread's state; the generated code never uses it).

Unlike XenonRecomp, which trusts the calling convention, this checks it. The
context stays the mailbox between functions, and an analysis of the whole
program decides what goes through it:

- A function has a local for each register it names, and for each
  nonvolatile one it passes from its caller to a callee. A local starts from
  the context when the function may read it before writing it (its live-in
  set), else from zero, as XenonRecomp's do.
- Before a call, the caller stores each local the callee may read: the
  callee's live-in set (with its own callees', to a fixed point over the call
  graph), and what host code reads at that call (SITE_READS, by the return
  address the code checks: hooks and imports that look at their caller's
  registers). An indirect call reaches a function whose address escaped,
  which the compiler gives the standard convention; only the CRT's exception
  funclets read r12 there (INDIRECT_READS).
- After a call, the caller loads each volatile register it reads before
  writing (a result outside the convention), and the callee stores it at its
  returns. Only the caller's own reads ask for this; code after a call that
  never returns (KeBugCheck, throws) asks nothing.
- A function that writes a nonvolatile register it never saves (itself, or
  through a callee) leaves it changed for its caller: CLOBBERS below. The
  movie decoder does it on purpose: its entry (sub_82A796E8) calls a routine
  (sub_82A7FFE8) that saves v32..v127 and loads its constants into
  v64..v72, which its leaf routines then read on entry (v69 in sub_82A9B720);
  another (sub_82A80330) swaps the caller's set back in at the end. Such a
  register is returned through the context like a volatile one, whenever the
  caller needs it after the call: for its own reads or for a later callee's.
- The nonvolatile save and restore helpers (__savegprlr_N, __restgprlr_N,
  __savefpr_N, ...) only move registers that are now locals, which the C++
  frame preserves; they are dropped, as XenonRecomp drops them, but for what
  else they do: __savegprlr_N's store of the return address 8 bytes below the
  stack pointer (the back chain race_hud_owners.h and stack traces read) and
  __restgprlr_N's load of it into LR. OBSERVED_RESTORES keeps those whose
  entry host code watches.

Statements are classified conservatively: anything not recognised as a full
write of a register counts as a read of it, which only adds stores and
loads; a 32-bit write of a GPR counts as a full one (the upper half is
undefined on the console). Prologue saves of nonvolatile registers into the
frame and mfcr are not reads (the caller's values are its own locals now).
Control flow or a helper handed the context that the parser does not know
stops the conversion.

XER: the generated code reads SO only as compares copy it into a CR field
and writes it only in sub_82A6BA5C (a context restore, mtxer), so it is zero
everywhere else, as in locals that start at zero; OV is never read. Only CA
is tracked.

usage: python scripts/register_locals.py --classes ctr,xer,cr,nonargument,nonvolatile [--loop-heads]
           [--image image.bin] [--report FILE] <generated dir> <output dir>
       python scripts/register_locals.py --file --classes ... <source> <output>
"""
import argparse
import json
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import loop_head_checkpoints  # noqa: E402

GENERATED = re.compile(r'ppc_recomp\.\d+\.cpp')

# ---------------------------------------------------------------- registers
# Registers (what becomes a local); CR fields are tracked bit by bit, as the
# generated code writes single bits (crand, cror, mtcrf).
NAMES = ([f'r{i}' for i in range(32)] + [f'f{i}' for i in range(32)] + [f'v{i}' for i in range(128)] +
         [f'cr{i}' for i in range(8)] + ['ctr', 'xer', 'msr'])
CR_BITS = ('lt', 'gt', 'eq', 'so')
_UNITS = [n for n in NAMES if not n.startswith('cr')] + [f'cr{i}.{b}' for i in range(8) for b in CR_BITS]
BIT = {name: 1 << i for i, name in enumerate(_UNITS)}
for _i in range(8):
    BIT[f'cr{_i}'] = sum(BIT[f'cr{_i}.{b}'] for b in CR_BITS)


def mask(names):
    value = 0
    for name in names:
        value |= BIT[name]
    return value


def names_of(value):
    return [name for name in NAMES if value & BIT[name]]


CLASSES = {
    'ctr': mask(['ctr']),
    'xer': mask(['xer']),
    'cr': mask(f'cr{i}' for i in range(8)),
    'nonargument': mask(['r0', 'r2', 'r11', 'r12', 'f0'] + [f'v{i}' for i in range(32, 64)]),
    'nonvolatile': mask([f'r{i}' for i in range(14, 32)] + [f'f{i}' for i in range(14, 32)] +
                        [f'v{i}' for i in range(14, 32)] + [f'v{i}' for i in range(64, 128)]),
}
# Destroyed by a call under the Xbox 360 convention.
VOLATILE = mask(['r0', 'r2', 'r11', 'r12', 'f0', 'ctr', 'xer', 'msr', 'cr0', 'cr1', 'cr5', 'cr6', 'cr7'] +
                [f'v{i}' for i in range(32, 64)])
TYPES = {'r': 'PPCRegister', 'f': 'PPCRegister', 'v': 'PPCVRegister', 'c': None}


def declared_type(name):
    if name.startswith('cr'):
        return 'PPCCRRegister'
    if name == 'xer':
        return 'PPCXERRegister'
    if name in ('ctr',):
        return 'PPCRegister'
    if name == 'msr':
        return 'uint64_t'
    return TYPES[name[0]]


# Host code that reads its caller's registers from the context, audited from
# src/ (every `ctx.rN` of a localisable register outside generated code).
# Each such read is tied to one call site, by the return address the code
# checks: the caller stores the registers before that call, direct or not.
SITE_READS = {
    0x82232E48: ['r30'],         # diagnostic_main.cpp: XNotifyPositionUI's listener audit
    0x822344D8: ['r30', 'r31'],  # diagnostic_main.cpp: XamUserGetSigninState's unselected-user audit
    0x822118F0: ['r31'],         # diagnostic_main.cpp enter_function_observed: language selection
    0x8250E718: ['r30'],         # diagnostic_main.cpp enter_function_observed: texture transfer
    0x8253A980: ['r30'],         # diagnostic_main.cpp enter_function_observed: DDS copy
    0x8249C224: ['r19', 'r30'],  # game_patches.cpp: the stream reader's idle wait (824B2060, by pointer)
}
# And by callee: guest_register_hooks.cpp's save helpers read r12 and r14..r31
# (only while the nonvolatile class is off; with it they are dropped).
HOST_READS = {f'__savegprlr_{n}': ['r12'] + [f'r{i}' for i in range(n, 32)] for n in range(14, 32)}
LR_SET = re.compile(r'^\t\t?ctx\.lr = 0x([0-9A-Fa-f]+);$')
# Inline helpers in the generated code that read or write registers through
# the context: name -> (reads, writes).
HELPER_ACCESS = {
    'sfr::synchronize_resource_memory': ([f'r{i}' for i in range(20, 28)] + ['r30'], []),
    # They set all of cr0 (SO from XER.SO, which is zero: module comment).
    'sfr::store_conditional_word': ([], ['cr0']),
    'sfr::store_conditional_doubleword': ([], ['cr0']),
}
# Helpers handed the context that read and write none of its registers but
# r13 (the thread's), which is never a local: diagnostic_hooks.h.
CONTEXT_FREE = {'sfr::guest_checkpoint', 'sfr::enter_function', 'sfr::load_reserved_word',
                'sfr::load_reserved_doubleword'}
# Functions whose registers host code reads in the middle of their body: such
# a register is written through to the context after each write and call.
WRITE_THROUGH = {
    # diagnostic_main.cpp: the VdGlobalDevice provider reads r31 of 824F19E8
    # (only without SFR_DIRECT_MEMORY's direct access).
    'sub_824F19E8': ['r31'],
}
# An indirect call reaches a function whose address escaped, which the
# compiler gives the standard convention (arguments in r3-r10, f1-f13,
# v1-v13; nothing else read, volatile registers destroyed). The exception is
# the CRT's own assembly: it calls exception funclets with r12 holding the
# establisher frame (4095 functions begin by reading r12).
INDIRECT_READS = ['r12']
# Imports that never return to their caller (ExTerminateThread unwinds the
# host stack to the thread's entry).
NORETURN_IMPORTS = {'__imp__KeBugCheck', '__imp__KeBugCheckEx', '__imp__RtlRaiseException',
                    '__imp__HalReturnToFirmware', '__imp__ExTerminateThread', '__imp__RtlUnwind'}
# Restore helpers whose entry host code observes, by the return address
# still set when the epilogue branches to them: kept (they then load stale
# stack words into the context, which no caller reads for these registers).
OBSERVED_RESTORES = {
    0x822345EC,  # diagnostic_main.cpp: the unselected-user audit ends at __restgprlr_27 from here
}
SAVE_RESTORE = re.compile(r'^(__savegprlr|__restgprlr|__savefpr|__restfpr|__savevmx|__restvmx)_(\d+)$')

REF = re.compile(r'\bctx\.(r\d+|f\d+|v\d+|cr\d|ctr|xer|msr)\b')
REF_BIT = re.compile(r'\bctx\.(r\d+|f\d+|v\d+|cr\d|ctr|xer|msr)\b(?:\.(lt|gt|eq|so)\b)?')
FULL_FIELDS = {'u64', 's64', 'f64'}
FUNC_START = re.compile(r'^PPC_FUNC_IMPL\(__imp__(\w+)\) \{$')
LABEL = re.compile(r'^(loc_[0-9A-Fa-f]+):$')
GOTO = re.compile(r'^\tgoto (loc_[0-9A-Fa-f]+);$')
IF_GOTO = re.compile(r'^\tif \((.*)\) goto (loc_[0-9A-Fa-f]+);$')
RETURN = re.compile(r'^\t\t?return;$')
IF_RETURN = re.compile(r'^\tif \((.*)\) return;$')
IF_BLOCK = re.compile(r'^\tif \((.*)\) \{$')
SWITCH = re.compile(r'^\tswitch \((.*)\) \{$')
CASE = re.compile(r'^\tcase [^:]+: goto (loc_[0-9A-Fa-f]+);$')
DEFAULT = re.compile(r'^\tdefault: (.*)$')
CLOSE = re.compile(r'^\t\}$')
CALL = re.compile(r'^(\t\t?)(\w+)\(ctx, base\);$')
INDIRECT = re.compile(r'^(\t\t?)PPC_CALL_INDIRECT_FUNC\((.*)\);$')
HELPER = re.compile(r'^\t\t?(?:ctx\.\w+\.\w+ = )?(sfr::\w+)\(ctx\b')
UNKNOWN_FLOW = re.compile(r'\b(goto|return|switch|case|default|else|for|while|do|longjmp|setjmp)\b')
# A statement that writes one register whole, then reads only its operands.
ASSIGN = re.compile(r'^\t\t?ctx\.(r\d+|f\d+|v\d+|cr\d|ctr|xer|msr)(?:\.(\w+))?(\[[^\]]*\])? = (.*)$')
COMPARE = re.compile(r'^\t\t?ctx\.(cr\d)\.compare(?:<\w+>)?\((.*)$')
VSTORE = re.compile(r'^\t\t?simde_mm_store_(?:si128\(\(simde__m128i\*\)|ps\()ctx\.(v\d+)\.\w+, (.*)$')
VOUT_FIRST = re.compile(r'^\t\t?sfr::vmx::(?!set_compare_cr6)\w+\(ctx\.(v\d+), (.*)$')
VLOAD = re.compile(r'^\t\t?sfr::(?:load_vector_(?:memory|left|right)\((.*)|vector_unpack_half\((ctx\.v\d+\.u16)), '
                   r'ctx\.(v\d+)\.(?:u8\);|f32, \d+\);)$')
# Loads with update: the first register is the loaded value, the second the
# base (read and written).
LOAD_UPDATE = re.compile(r'^\t\t?sfr::(?:load_update<\w+>|load_halfword_update|load_single_update|'
                         r'load_double_update)\(\*sfr::active_memory, ctx\.(r\d+|f\d+)\.u64, (.*)$')
# addc writes its destination and the carry from its operands alone.
ADDC = re.compile(r'^\t\t?sfr::addc\(ctx\.(r\d+)\.u64, (.*), ctx\.xer\.ca\);$')
SET_CR6 = re.compile(r'^\t\t?sfr::vmx::set_compare_cr6\(ctx\.(cr6), (.*)$')


def refs(text):
    value = 0
    for match in REF_BIT.finditer(text):
        name = match[1]
        if match[2] and name.startswith('cr'):
            value |= BIT[f'{name}.{match[2]}']
        elif name in BIT:
            value |= BIT[name]
    return value


def xer_reads(text):
    """XER reads that count: CA, not the SO a compare is handed."""
    return re.search(r'\bctx\.xer\.ca\b', text) is not None


# mfcr: a GPR built from every CR bit, which a prologue stores to save the
# nonvolatile fields for its caller. Its reads are not uses: a caller's CR is
# not a function's input under the ABI, and the fields it saves are the
# caller's locals now, which the C++ frame preserves.
# A prologue's store of a nonvolatile register into its own frame (std/stfd
# below r1) keeps the caller's value for the epilogue's load: not a use, as
# the caller's value is the caller's local now.
ABI_SAVE = re.compile(r'^\t\t?PPC_STORE_U(?:64|32)\(ctx\.r1\.u32 \+ -?\d+, ctx\.(r\d+|f\d+)\.u(?:64|32)\);$')
VECTOR_SAVE = re.compile(r'^\t\t?sfr::store_vector_memory\(uint32_t\(ctx\.r1\.u32 \+ ctx\.r\d+\.u32\), ctx\.(v\d+)\.u8\);$')
NONVOLATILE_SAVED = mask([f'r{i}' for i in range(14, 32)] + [f'f{i}' for i in range(14, 32)] +
                         [f'v{i}' for i in range(14, 32)] + [f'v{i}' for i in range(64, 128)])
# One lane of a vector register (XenonRecomp writes some operations lane by
# lane): a run that writes every lane, reading none of the register, writes
# it whole.
LANE = re.compile(r'^\t\t?ctx\.(v\d+)\.([us](?:8|16|32)|f32)\[(\d+)\] = (.*)$')
LANES = {'u8': 16, 's8': 16, 'u16': 8, 's16': 8, 'u32': 4, 's32': 4, 'f32': 4}
MFCR = re.compile(r'^\t\t?ctx\.(r\d+)\.u64 \|?= ctx\.cr\d\.(?:lt|gt|eq|so) \? 0x[0-9A-Fa-f]+ : 0;$')


def uses_defs(text):
    """(uses, full defs) of a plain statement; any other reference is a use."""
    full = 0
    rest = text
    match = ABI_SAVE.match(text) or VECTOR_SAVE.match(text)
    if match and BIT[match[1]] & NONVOLATILE_SAVED:
        return 0, 0
    match = MFCR.match(text)
    if match:
        if '|=' in text:
            return BIT[match[1]], 0
        return 0, BIT[match[1]]
    match = ASSIGN.match(text)
    if match and match[1] in BIT:
        name, field, index, rhs = match[1], match[2], match[3], match[4]
        whole = (index is None and (field in FULL_FIELDS or field is None or
                                    (name[0] == 'r' and field in ('u32', 's32'))) and
                 not name.startswith('cr') and name != 'xer')
        if name == 'msr':
            whole = field is None
        if whole:
            full = BIT[name]
            rest = rhs
        elif name == 'xer':
            # CA is the only field read (SO and OV: see the module comment)
            if field == 'ca':
                full = BIT[name]
            rest = rhs
        elif name.startswith('cr'):
            if field in CR_BITS and index is None:
                full = BIT[f'{name}.{field}']  # one bit
            rest = rhs
        else:
            rest = text  # a part of the register: the rest of it lives on
    else:
        for pattern, group, after in ((COMPARE, 1, 2), (VSTORE, 1, 2), (VOUT_FIRST, 1, 2), (SET_CR6, 1, 2)):
            match = pattern.match(text)
            if match:
                full = BIT[match[group]]
                rest = match[after]
                break
        else:
            match = VLOAD.match(text)
            if match:
                full = BIT[match[3]]
                rest = match[1] or match[2]
            match = match or LOAD_UPDATE.match(text)
            if match and not full:
                full = BIT[match[1]]
                rest = match[2]
            if not match:
                match = ADDC.match(text)
                if match:
                    full = BIT[match[1]] | BIT['xer']
                    rest = match[2]
    uses = refs(rest)
    if uses & BIT['xer'] and not xer_reads(rest):
        uses &= ~BIT['xer']
    return uses, full


class Function:
    __slots__ = ('name', 'begin', 'end', 'nodes', 'labels', 'callees', 'indirect', 'livein',
                 'ret', 'referenced', 'livebefore', 'written', 'saved', 'clobbers')

    def __init__(self, name, begin):
        self.name, self.begin, self.end = name, begin, None
        self.nodes, self.labels, self.callees = [], {}, set()
        self.indirect = False
        self.livein = self.ret = self.referenced = 0
        self.written = self.saved = self.clobbers = 0
        self.livebefore = []


# Node kinds
PLAIN, CALL_K, ICALL, EXIT, BRANCH = range(5)


class Node:
    __slots__ = ('kind', 'line', 'uses', 'defs', 'succ', 'callee', 'helper', 'falls', 'lane', 'returns', 'site', 'lr')

    def __init__(self, kind, line, uses=0, defs=0, callee=None, helper=None, falls=True, returns=True):
        self.kind, self.line, self.uses, self.defs = kind, line, uses, defs
        self.succ, self.callee, self.helper = [], callee, helper
        self.falls = falls and kind != EXIT
        self.lane = None
        self.returns = returns and kind == EXIT
        self.site = 0  # registers host code reads at this call (SITE_READS)
        self.lr = None  # the return address last set before this call


def parse(lines, source=''):
    """Functions of one generated file, each a graph of statement nodes."""
    functions = []
    current = None
    for index, raw in enumerate(lines):
        line = raw.rstrip('\r')
        if current is None:
            match = FUNC_START.match(line)
            if match:
                current = Function(match[1], index)
                pending, blocks, defaulted = [], [], set()
                last_lr = None
            continue
        nodes = current.nodes
        here = len(nodes)
        if line == '}':
            current.end = index
            nodes.append(Node(EXIT, index))
            if blocks:
                raise ValueError(f'{source}: {current.name}: unclosed block')
            for i, node in enumerate(nodes):
                if node.falls:
                    node.succ.append(i + 1)
            for i, label in pending:
                if label not in current.labels:
                    raise ValueError(f'{source}: {current.name}: goto to unknown {label}')
                nodes[i].succ.append(current.labels[label])
            for node in nodes:
                node.succ = sorted(set(node.succ))
            functions.append(current)
            current = None
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith('//'):
            continue
        match = LABEL.match(line)
        if match:
            current.labels[match[1]] = here
            nodes.append(Node(PLAIN, index))
            continue
        match = GOTO.match(line)
        if match:
            nodes.append(Node(BRANCH, index, falls=False))
            pending.append((here, match[1]))
            continue
        match = IF_GOTO.match(line)
        if match:
            nodes.append(Node(BRANCH, index, refs_partial(match[1])))
            pending.append((here, match[2]))
            continue
        if RETURN.match(line):
            nodes.append(Node(EXIT, index))
            continue
        match = IF_RETURN.match(line)
        if match:
            # a branch whose taken edge reaches its own exit node
            branch = Node(BRANCH, index, refs_partial(match[1]), falls=False)
            branch.succ.extend([here + 1, here + 2])
            nodes.append(branch)
            nodes.append(Node(EXIT, index))
            continue
        match = IF_BLOCK.match(line)
        if match:
            nodes.append(Node(BRANCH, index, refs_partial(match[1])))
            blocks.append(here)
            continue
        match = SWITCH.match(line)
        if match:
            nodes.append(Node(BRANCH, index, refs_partial(match[1]), falls=False))
            blocks.append(here)
            continue
        match = CASE.match(line)
        if match:
            pending.append((blocks[-1], match[1]))
            continue
        match = DEFAULT.match(line)
        if match:
            if not match[1].startswith('throw '):
                raise ValueError(f'{source}:{index + 1}: unexpected default: {line.strip()}')
            defaulted.add(blocks[-1])
            nodes[blocks[-1]].succ.append(here)
            nodes.append(Node(EXIT, index, returns=False))
            continue
        if CLOSE.match(line):
            opener = blocks.pop()
            # An if block's false edge comes here, and a switch's without a default.
            if nodes[opener].falls or opener not in defaulted:
                nodes[opener].succ.append(here)
            nodes.append(Node(PLAIN, index))
            continue
        match = CALL.match(line)
        if match:
            node = Node(CALL_K, index, callee=match[2])
            node.site = mask(SITE_READS.get(last_lr, []))
            node.lr = last_lr
            nodes.append(node)
            current.callees.add(match[2])
            continue
        match = INDIRECT.match(line)
        if match:
            node = Node(ICALL, index, refs_partial(match[2]))
            node.site = mask(SITE_READS.get(last_lr, []))
            nodes.append(node)
            current.indirect = True
            continue
        match = LR_SET.match(line)
        if match:
            last_lr = int(match[1], 16)
        if stripped.startswith('throw ') or stripped.startswith('sfr::unsupported_function('):
            nodes.append(Node(EXIT, index, returns=False))
            continue
        # Any other control flow would be invisible to the analysis: refuse it.
        # So would a helper that is handed the whole context.
        helper_match = HELPER.match(line)
        if (UNKNOWN_FLOW.search(stripped) or stripped.endswith('{') or stripped.startswith('}') or
                (re.search(r'\(ctx[,)]', stripped) and
                 not (helper_match and (helper_match[1] in HELPER_ACCESS or helper_match[1] in CONTEXT_FREE)))):
            raise ValueError(f'{source}:{index + 1}: statement the analysis does not know: {stripped}')
        uses, defs = uses_defs(line)
        match = HELPER.match(line)
        helper = None
        if match and match[1] in HELPER_ACCESS:
            helper = match[1]
            uses |= mask(HELPER_ACCESS[helper][0])
            defs |= mask(HELPER_ACCESS[helper][1])
        node = Node(PLAIN, index, uses, defs, helper=helper)
        match = LANE.match(line)
        if match:
            node.lane = (match[1], match[2], int(match[3]), BIT[match[1]] & refs(match[4]) != 0)
        nodes.append(node)
    if current is not None:
        raise ValueError(f'{source}: unterminated function {current.name}')
    for function in functions:
        for i in range(function.begin, function.end):
            function.referenced |= refs(lines[i])
        whole_lane_runs(function.nodes)
        written_and_saved(function, lines)
    return functions


# Any write of a register, whole or in part (a statement that begins with it).
WRITE = re.compile(r'^\t\t?ctx\.(r\d+|f\d+|v\d+)\b[^=]*= ')
SELF_MOVE = re.compile(r'^\t\t?ctx\.(r\d+)\.u64 = ctx\.\1\.u64;$')
# stvx/stvlx into the frame through an address computed first (824DBDC4, a
# memcpy, keeps v64/v65 so): ea = r1 + rN, then the store.
FRAME_EA = re.compile(r'^\t\t?ea = ctx\.r1\.u32 \+ ctx\.r\d+\.u32;$')
EA_VECTOR_SAVE = re.compile(r'^\t\t?sfr::store_vector_(?:memory|left)\(ea, ctx\.(v\d+)\.u8\);$')
# A stack pointer loaded through another register: a switch to another
# frame (longjmp, sub_82A57E40; the context restore, sub_82A6BA5C). The
# registers such a function loads are that frame's, not its caller's.
STACK_SWITCH = re.compile(r'^\t\t?ctx\.r1\.u(?:64|32) = PPC_LOAD_U(?:64|32)\(ctx\.r(?!1\.)\d+\.')
SAVE_FIRST = {'__savegprlr': 'r', '__savefpr': 'f', '__savevmx': 'v'}


def written_and_saved(function, lines):
    """The nonvolatile registers the function writes, and those it saves for
    its caller (a store into its frame, or a save helper), over the
    statements reachable from its entry (a jump table's data decoded as code
    follows some switches)."""
    nodes = function.nodes
    seen, stack = {0}, [0] if nodes else []
    while stack:
        for succ in nodes[stack.pop()].succ:
            if succ not in seen:
                seen.add(succ)
                stack.append(succ)
    written = saved = 0
    for i in seen:
        node = nodes[i]
        if node.kind == CALL_K:
            match = SAVE_RESTORE.match(node.callee)
            if match and match[1] in SAVE_FIRST:
                first, prefix = int(match[2]), SAVE_FIRST[match[1]]
                last = 128 if prefix == 'v' and first >= 64 else 32
                saved |= mask(f'{prefix}{n}' for n in range(first, last))
            continue
        if node.kind != PLAIN:
            continue
        line = lines[node.line].rstrip('\r')
        if SELF_MOVE.match(line):
            continue  # mr rN,rN changes nothing
        written |= node.defs
        match = WRITE.match(line)
        if match:
            written |= BIT[match[1]]
        match = ABI_SAVE.match(line) or VECTOR_SAVE.match(line)
        if match:
            saved |= BIT[match[1]]
        match = EA_VECTOR_SAVE.match(line)
        if match and i and FRAME_EA.match(lines[nodes[i - 1].line].rstrip('\r')):
            saved |= BIT[match[1]]
        if STACK_SWITCH.match(line):
            written = 0
            break
    function.written = written & NONVOLATILE_SAVED
    function.saved = saved & NONVOLATILE_SAVED


def whole_lane_runs(nodes):
    """Mark the first statement of each run writing every lane as a full write."""
    i = 0
    while i < len(nodes):
        lane = nodes[i].lane
        if lane is None or lane[3]:
            i += 1
            continue
        register, kind = lane[0], lane[1]
        seen, j = set(), i
        while (j < len(nodes) and nodes[j].lane is not None and nodes[j].lane[0] == register and
               nodes[j].lane[1] == kind and not nodes[j].lane[3] and not nodes[j].succ[1:] and
               (j == i or not is_target(nodes, j))):
            seen.add(nodes[j].lane[2])
            j += 1
        if len(seen) == LANES[kind]:
            nodes[i].defs |= BIT[register]
            for k in range(i, j):
                nodes[k].uses &= ~BIT[register]
        i = max(j, i + 1)


def is_target(nodes, j):
    return False  # labels are their own nodes; a lane statement is never a jump target


def refs_partial(text):
    uses = refs(text)
    if uses & BIT['xer'] and not xer_reads(text):
        uses &= ~BIT['xer']
    return uses


# ------------------------------------------------------------------ analysis
class Program:
    abi_indirect = True

    def __init__(self, functions, localised):
        self.functions = {f.name: f for f in functions}
        self.localised = localised
        drops = localised & CLASSES['nonvolatile'] == CLASSES['nonvolatile']
        self.drop_save_restore = drops
        host = {}
        for name, regs in HOST_READS.items():
            if drops and SAVE_RESTORE.match(name):
                continue
            host[name] = mask(regs)
        self.host = host

    def find_noreturn(self):
        """Functions that never return: every path ends in a throw or in a
        call of one (least fixed point, from the imports below). Calls of them
        are no source of context returns: the code after one is another
        block's (the compiler placed it there), reached only by its gotos."""
        noreturn = set(NORETURN_IMPORTS)
        changed = True
        while changed:
            changed = False
            for f in self.functions.values():
                if f.name in noreturn or not f.nodes:
                    continue
                seen, stack, returns = {0}, [0], False
                while stack and not returns:
                    node = f.nodes[stack.pop()]
                    if node.kind == EXIT:
                        returns = node.returns
                        continue
                    if node.kind == CALL_K and node.callee in noreturn:
                        continue
                    for succ in node.succ:
                        if succ not in seen:
                            seen.add(succ)
                            stack.append(succ)
                if not returns:
                    noreturn.add(f.name)
                    changed = True
        self.noreturn = noreturn

    def find_clobbers(self):
        """Nonvolatile registers each function may return changed: written
        and not saved, by it or by a callee (least fixed point). The save and
        restore helpers are the convention itself, and a call that never
        returns changes nothing for the code after it."""
        functions = [f for f in self.functions.values() if not SAVE_RESTORE.match(f.name)]
        for f in functions:
            f.clobbers = f.written & ~f.saved
        changed = True
        while changed:
            changed = False
            for f in functions:
                value = f.clobbers
                for name in f.callees:
                    callee = self.functions.get(name)
                    if callee is not None and name not in self.noreturn:
                        value |= callee.clobbers
                value &= ~f.saved
                if value != f.clobbers:
                    f.clobbers = value
                    changed = True

    def callee_clobbers(self, name):
        if SAVE_RESTORE.match(name) or name in self.noreturn:
            return 0
        function = self.functions.get(name)
        return function.clobbers if function is not None else 0

    def callee_livein(self, name):
        if self.drop_save_restore and SAVE_RESTORE.match(name):
            return BIT['r12'] if name.startswith('__savegprlr') else 0
        function = self.functions.get(name)
        value = self.host.get(name, 0)
        if function is not None:
            value |= function.livein
        return value

    def callee_ret(self, name):
        if self.drop_save_restore and SAVE_RESTORE.match(name):
            return 0
        function = self.functions.get(name)
        return function.ret if function is not None else 0

    def liveness(self, function, indirect_livein, indirect_ret, local=False):
        """Backward liveness over the statement graph; returns live-in.

        local: what the function itself reads, with callees reading nothing
        (their own reads are the caller's to store, not to receive)."""
        nodes = function.nodes
        n = len(nodes)
        live_in = [0] * n
        gen = [0] * n
        kill = [0] * n
        for i, node in enumerate(nodes):
            if node.kind == CALL_K:
                gen[i] = node.site | (0 if local else self.callee_livein(node.callee))
                kill[i] = self.callee_ret(node.callee)
            elif node.kind == ICALL:
                gen[i] = node.uses | node.site | (0 if local else indirect_livein)
                kill[i] = indirect_ret
            elif node.kind == EXIT:
                gen[i] = function.ret
            else:
                gen[i] = node.uses
                kill[i] = node.defs
        preds = [[] for _ in range(n)]
        for i, node in enumerate(nodes):
            for s in node.succ:
                preds[s].append(i)
        work = list(range(n))
        on = [True] * n
        while work:
            i = work.pop()
            on[i] = False
            out = 0
            for s in nodes[i].succ:
                out |= live_in[s]
            value = gen[i] | (out & ~kill[i])
            if value != live_in[i]:
                live_in[i] = value
                for p in preds[i]:
                    if not on[p]:
                        on[p] = True
                        work.append(p)
        if local:
            return live_in
        function.livebefore = live_in
        return live_in[0] if n else 0

    def solve(self, indirect_targets=None):
        """Live-in sets and context returns, to a fixed point over the program.

        An indirect call may reach any function in indirect_targets (names;
        None: every function); its callers store the union of their live-in
        sets first. Volatile registers read after an indirect call make each
        of them return those through the context.
        """
        functions = list(self.functions.values())
        self.find_noreturn()
        self.find_clobbers()
        self.ret_seeds = []
        if indirect_targets is None:
            targets = [f for f in functions if not SAVE_RESTORE.match(f.name)]
        else:
            targets = [self.functions[n] for n in indirect_targets if n in self.functions]
        callers = {}
        for f in functions:
            for c in f.callees:
                callers.setdefault(c, set()).add(f.name)
        indirect_livein = 0
        indirect_ret = 0
        rounds = 0
        while True:
            rounds += 1
            changed = False
            work = list(self.functions)
            queued = set(work)
            while work:
                name = work.pop()
                queued.discard(name)
                f = self.functions[name]
                value = self.liveness(f, indirect_livein, indirect_ret) & self.localised
                if value != f.livein:
                    f.livein = value
                    changed = True
                    for caller in callers.get(name, ()):
                        if caller not in queued:
                            queued.add(caller)
                            work.append(caller)
            if self.abi_indirect:
                union = mask(INDIRECT_READS) & self.localised
            else:
                union = 0
                for f in targets:
                    union |= f.livein | (self.host.get(f.name, 0) & self.localised)
            # Volatile registers a caller reads after a call before writing:
            # the callee returns them through the context.
            new_ret = {}
            new_indirect_ret = indirect_ret
            # Only the caller's own reads (and the registers it returns
            # itself) ask a callee to return a register: a later callee's
            # read is served by the caller's store before that call.
            for f in functions:
                if not any(node.kind in (CALL_K, ICALL) for node in f.nodes):
                    continue
                own = self.liveness(f, 0, indirect_ret, local=True)
                for i, node in enumerate(f.nodes):
                    if node.kind not in (CALL_K, ICALL):
                        continue
                    after = 0
                    for succ in node.succ:
                        after |= own[succ]
                    need = after & VOLATILE & self.localised
                    if node.kind == CALL_K:
                        # A nonvolatile register the callee leaves changed,
                        # which the caller or a later callee of it reads.
                        changed_nv = self.callee_clobbers(node.callee) & self.localised
                        if changed_nv:
                            full = 0
                            for succ in node.succ:
                                full |= f.livebefore[succ]
                            need |= full & changed_nv
                    if not need:
                        continue
                    if node.kind == CALL_K:
                        if node.callee in self.noreturn:
                            continue
                        if rounds == 1:
                            self.ret_seeds.append((f.name, node.callee, names_of(need)))
                        new_ret[node.callee] = new_ret.get(node.callee, 0) | need
                    else:
                        if rounds == 1:
                            self.ret_seeds.append((f.name, None, names_of(need)))
                        new_indirect_ret |= need
            for name, value in new_ret.items():
                f = self.functions.get(name)
                if f is not None and (f.ret | value) != f.ret:
                    f.ret |= value
                    changed = True
            if self.abi_indirect:
                new_indirect_ret = 0
            if new_indirect_ret != indirect_ret:
                indirect_ret = new_indirect_ret
                for f in targets:
                    f.ret |= indirect_ret
                changed = True
            if union != indirect_livein:
                indirect_livein = union
                changed = True
            if not changed:
                break
        self.indirect_livein = indirect_livein
        self.indirect_ret = indirect_ret
        self.rounds = rounds


LIS = re.compile(r'^\tctx\.(r\d+)\.s64 = (-?\d+);$')
ADDI = re.compile(r'^\tctx\.(r\d+)\.s64 = ctx\.(r\d+)\.s64 \+ (-?\d+);$')
ORI = re.compile(r'^\tctx\.(r\d+)\.u64 = ctx\.(r\d+)\.u64 \| (\d+);$')


def code_constants(text):
    """Addresses the code builds with lis and addi/ori (a superset: any
    lis value of a register with any later immediate added to it)."""
    found = set()
    high = {}
    for line in text.split('\n'):
        line = line.rstrip('\r')
        match = LIS.match(line)
        if match:
            value = int(match[2])
            if value & 0xFFFF == 0:
                high[match[1]] = value & 0xFFFFFFFF
                found.add(value & 0xFFFFFFFF)
            continue
        match = ADDI.match(line) or ORI.match(line)
        if match and match[2] in high:
            found.add((high[match[2]] + int(match[3])) & 0xFFFFFFFF)
    return found


def image_words(path):
    """Every aligned big-endian word of the image's data sections (vtables,
    function tables): not .text, nor .pdata, which names every function, nor
    the relocations and imports. Needs sections.tsv and image.tsv beside it."""
    path = Path(path)
    data = path.read_bytes()
    info = dict(line.split('\t') for line in (path.parent / 'image.tsv').read_text().splitlines() if '\t' in line)
    base = int(info['base'])
    words = set()
    for line in (path.parent / 'sections.tsv').read_text().splitlines()[1:]:
        name, address, size, _ = line.split('\t')
        if name in ('.text', '.pdata', '.reloc', '.idata'):
            continue
        begin = int(address) - base
        end = min(begin + int(size), len(data))
        words.update(int.from_bytes(data[i:i + 4], 'big') for i in range(begin, end - 3, 4))
    return words


def address_of(name):
    match = re.fullmatch(r'sub_([0-9A-Fa-f]{8})', name)
    return int(match[1], 16) if match else None


# --------------------------------------------------------------- transform
def rename(text, localised_names):
    return REF.sub(lambda m: m[1] if m[1] in localised_names else m[0], text)


def flush(indent, regs):
    return [f'{indent}ctx.{name} = {name};' for name in names_of(regs)]


def reload(indent, regs):
    return [f'{indent}{name} = ctx.{name};' for name in names_of(regs)]


def transform_function(lines, function, program, out):
    # A local for each register the function names, and for each nonvolatile
    # one it passes from its caller to a callee (a callee in between would
    # leave its own value in the context). A volatile register it only passes
    # on stays in the context, untouched.
    localised = program.localised & (function.referenced | (function.livein & ~VOLATILE))
    nodes = function.nodes
    by_line = {}
    for i, node in enumerate(nodes):
        by_line.setdefault(node.line, (i, node))
    names = set(names_of(localised))
    write_through = mask(WRITE_THROUGH.get(function.name, [])) & localised
    ret = function.ret & localised
    for index in range(function.begin, function.end + 1):
        raw = lines[index]
        cr = '\r' if raw.endswith('\r') else ''
        line = raw.rstrip('\r')
        entry = by_line.get(index)
        if index == function.begin:
            out.append(raw)
            continue
        if index == function.begin + 1 and line == '\tPPC_FUNC_PROLOGUE();':
            out.append(raw)
            for name in names_of(localised):
                kind = declared_type(name)
                if function.livein & BIT[name]:
                    out.append(f'\t{kind} {name} = ctx.{name};{cr}')
                else:
                    out.append(f'\t{kind} {name}{{}};{cr}')
            continue
        if entry is None:
            out.append(rename(raw, names))
            continue
        i, node = entry
        indent = '\t\t' if line.startswith('\t\t') else '\t'
        if node.kind == CALL_K:
            match = CALL.match(line)
            callee = match[2]
            if program.drop_save_restore and SAVE_RESTORE.match(callee) and not (
                    callee.startswith('__rest') and node.lr in OBSERVED_RESTORES):
                # The return address the bl set is the helper's only: drop it.
                while out and out[-1].rstrip('\r').startswith(indent + 'ctx.lr = 0x'):
                    out.pop()
                if callee.startswith('__savegprlr'):
                    r12 = 'r12' if 'r12' in names else 'ctx.r12'
                    out.append(f'{indent}PPC_STORE_U32(ctx.r1.u32 - 8, {r12}.u32);{cr}')
                elif callee.startswith('__restgprlr'):
                    # It also returns with the saved return address in LR,
                    # which a function entered by a branch from the caller
                    # saves as its own, and host code compares.
                    out.append(f'{indent}ctx.lr = PPC_LOAD_U32(ctx.r1.u32 - 8);{cr}')
                continue
            before = (program.callee_livein(callee) | node.site) & localised
            after = 0
            for s in node.succ:
                after |= function.livebefore[s]
            after &= program.callee_ret(callee) & localised
            out.extend(x + cr for x in flush(indent, before))
            out.append(raw)
            out.extend(x + cr for x in reload(indent, after))
            if write_through:
                out.extend(x + cr for x in flush(indent, write_through))
            continue
        if node.kind == ICALL:
            before = (program.indirect_livein | node.site) & localised
            after = 0
            for s in node.succ:
                after |= function.livebefore[s]
            after &= program.indirect_ret & localised
            out.extend(x + cr for x in flush(indent, before))
            out.append(rename(raw, names))
            out.extend(x + cr for x in reload(indent, after))
            if write_through:
                out.extend(x + cr for x in flush(indent, write_through))
            continue
        if node.kind == EXIT and ret and RETURN.match(line):
            out.extend(x + cr for x in flush(indent, ret))
            out.append(raw)
            continue
        if node.kind == EXIT and ret and line == '}':
            out.extend(x + cr for x in flush('\t', ret))
            out.append(raw)
            continue
        if node.kind == BRANCH and ret and IF_RETURN.match(line):
            cond = IF_RETURN.match(line)[1]
            stores = ' '.join(f'ctx.{n} = {n};' for n in names_of(ret))
            out.append(f'\tif ({rename(cond, names)}) {{ {stores} return; }}{cr}')
            continue
        if node.helper:
            reads, writes = HELPER_ACCESS[node.helper]
            out.extend(x + cr for x in flush(indent, mask(reads) & localised))
            out.append(rename(raw, names))
            out.extend(x + cr for x in reload(indent, mask(writes) & localised))
            continue
        out.append(rename(raw, names))
        if write_through and node.kind == PLAIN and (node.defs | node.uses) & write_through:
            out.extend(x + cr for x in flush(indent, write_through & ((node.defs | node.uses))))


def transform_text(text, program, functions):
    lines = text.split('\n')
    out = []
    cursor = 0
    for function in functions:
        out.extend(lines[cursor:function.begin])
        transform_function(lines, function, program, out)
        cursor = function.end + 1
    out.extend(lines[cursor:])
    return '\n'.join(out)


# ---------------------------------------------------------------- driver
def read_sources(directory, loop_heads):
    texts = {}
    for path in sorted(Path(directory).iterdir()):
        if path.is_file() and GENERATED.fullmatch(path.name):
            text = path.read_bytes().decode('utf-8')
            if loop_heads:
                text = loop_head_checkpoints.transform(text)[0]
            texts[path.name] = text
    return texts


def _parse_one(item):
    name, text = item
    return name, parse(text.split('\n'), name)


def parse_classes(value):
    if value in ('', 'none'):
        return 0
    result = 0
    for name in value.split(','):
        if name not in CLASSES:
            raise SystemExit(f'unknown register class {name!r}; known: {", ".join(CLASSES)}')
        result |= CLASSES[name]
    return result


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--classes', default='none')
    parser.add_argument('--loop-heads', action='store_true')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    parser.add_argument('--analyse-only', action='store_true')
    parser.add_argument('--image', type=Path,
                        help='image.bin of the same executable: indirect calls then reach only functions '
                             'whose address is in the image or built by the code (default: any function)')
    parser.add_argument('--file', action='store_true',
                        help='source and output are single files, analysed as a whole program on their own '
                             '(the functions a test extracts)')
    args = parser.parse_args(argv)
    if args.file:
        text = args.source.read_bytes().decode('utf-8')
        functions = parse(text.split('\n'), args.source.name)
        program = Program(functions, parse_classes(args.classes))
        program.solve()
        args.output.write_bytes(transform_text(text, program, functions).encode('utf-8'))
        return
    if args.source.resolve() == args.output.resolve():
        sys.exit('the output must be another directory')
    localised = parse_classes(args.classes)
    texts = read_sources(args.source, args.loop_heads)
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        parsed = dict(pool.map(_parse_one, texts.items(), chunksize=4))
    every = [f for fs in parsed.values() for f in fs]
    program = Program(every, localised)
    program.targets = None
    if localised:
        if args.image:
            addresses = image_words(args.image)
            for text in texts.values():
                addresses |= code_constants(text)
            program.targets = sorted(f.name for f in every if address_of(f.name) in addresses)
        program.solve(program.targets)
    args.output.mkdir(parents=True, exist_ok=True)
    for name, text in ([] if args.analyse_only else texts.items()):
        result = transform_text(text, program, parsed[name]) if localised else text
        target = args.output / name
        # Written in full every time, so the build sees it newer than its source.
        target.write_bytes(result.encode('utf-8'))
    if args.report:
        report = summary(program, every)
        args.report.write_text(json.dumps(report, indent=1) + '\n')
    print(f'register locals: classes={args.classes} functions={len(every)}')


def summary(program, functions):
    count = lambda pred: sum(1 for f in functions if pred(f))  # noqa: E731
    per_reg = {}
    for f in functions:
        for name in names_of(f.livein):
            per_reg[name] = per_reg.get(name, 0) + 1
    ret = {}
    for f in functions:
        for name in names_of(f.ret):
            ret.setdefault(name, []).append(f.name)
    return {
        'rounds': getattr(program, 'rounds', 0),
        'indirect_targets': len(program.targets) if program.targets is not None else 'all',
        'targets_with_livein': {f.name: names_of(f.livein) for f in functions
                                if program.targets is not None and f.name in set(program.targets) and f.livein},
        'with_ret': {f.name: names_of(f.ret) for f in functions if f.ret},
        # Functions returning nonvolatile registers they leave changed (CLOBBERS).
        'nonvolatile_ret': {f.name: names_of(f.ret & NONVOLATILE_SAVED) for f in functions
                            if f.ret & NONVOLATILE_SAVED},
        'ret_seeds': getattr(program, 'ret_seeds', []),
        'noreturn': len(getattr(program, 'noreturn', ())),
        'functions': len(functions),
        'with_livein': count(lambda f: f.livein),
        'livein_by_register': dict(sorted(per_reg.items(), key=lambda kv: -kv[1])),
        'indirect_livein': names_of(getattr(program, 'indirect_livein', 0)),
        'indirect_ret': names_of(getattr(program, 'indirect_ret', 0)),
        'ret_by_register': {k: (len(v), sorted(v)[:20]) for k, v in ret.items()},
        'livein_examples': {name: sorted(f.name for f in functions if f.livein & BIT[name])[:30]
                            for name in per_reg},
    }


if __name__ == '__main__':
    main(sys.argv[1:])
