"""Make an auditable, diagnostic-only copy of XenonRecomp alias-format output.

This is a conservative source filter, not a verifier of PowerPC semantics. The
generated executable still needs checked memory/dispatch hooks and OS isolation.
No input is edited. An existing destination is never replaced.
"""

import argparse
from bisect import bisect_left
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import time
import tomllib
import uuid


IDENT = r'[A-Za-z_][A-Za-z_0-9]*'
ALIAS = re.compile(r'__attribute__\(\(alias\("(__imp__' + IDENT +
                   r')"\)\)\) PPC_WEAK_FUNC\((' + IDENT + r')\);\r?\n'
                   r'PPC_FUNC_IMPL\((' + IDENT + r')\) \{')
TOKENS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|'
                    r"'(?:\\.|[^'\\])*'|[{}]")
DECLARATION = re.compile(r'PPC_EXTERN_FUNC\((' + IDENT + r')\);')
MAPPING = re.compile(r'\{\s*0x([0-9A-Fa-f]+),\s*(' + IDENT + r')\s*\},')
INSTRUCTION = re.compile(r'^\t// (?!ERROR\b)(.*)$', re.MULTILINE)
TIME_BASE = re.compile(r'^(\t// mftb r(?P<register>[0-9]|[12][0-9]|3[01])\r?\n)'
                       r'\tctx\.r(?P=register)\.u64 = __rdtsc\(\);(?=\r?$)', re.MULTILINE)
REGISTER = r'(?:[0-9]|[12][0-9]|3[01])'
RESERVATION = re.compile(r'^\t// (?P<op>lwarx|stwcx\.|ldarx|stdcx\.) r(?P<reg>' + REGISTER +
                         r'),(?P<ra>0|r' + REGISTER + r'),r(?P<rb>' + REGISTER + r')\r?\n', re.MULTILINE)
VECTOR_MEMORY = re.compile(r'^\t// (?P<op>lvx128|stvx128|lvx|stvx) v(?P<reg>[0-9]+),'
                           r'(?P<ra>0|r' + REGISTER + r'),r(?P<rb>' + REGISTER + r')\r?\n', re.MULTILINE)
VECTOR_WORD_STORE = re.compile(r'^\t// (?P<op>stvewx128|stvewx) v(?P<reg>[0-9]+),'
                               r'(?P<ra>0|r' + REGISTER + r'),r(?P<rb>' + REGISTER + r')\r?\n', re.MULTILINE)
LOG_PATTERNS = (
    ('unrecognized', re.compile(r'Unrecognized instruction at 0x([0-9A-Fa-f]+): (\S+)')),
    ('decode', re.compile(r'Unable to decode instruction [0-9A-Fa-f]+ at (?:0x)?([0-9A-Fa-f]+)')),
    ('missing_switch', re.compile(r'Found a switch jump table at (?:0x)?([0-9A-Fa-f]+) with no switch table entry present')),
    # The lookahead captures the address as group 1 and the opcode as group 2.
    ('missing_comparison', re.compile(r'(?=\S+ at (?:0x)?([0-9A-Fa-f]+) has RC)(\S+) at (?:0x)?[0-9A-Fa-f]+ '
                                      r'has RC bit enabled but no comparison was generated')),
    ('switch_outside', re.compile(r'ERROR: Switch case at ([0-9A-Fa-f]+) is trying to jump outside function: [0-9A-Fa-f]+')),
)


def read(path):
    with path.open('r', encoding='utf-8', newline='') as stream:
        return stream.read()


def write(path, content):
    with path.open('w', encoding='utf-8', newline='') as stream:
        stream.write(content)


def parse_log(path):
    raw = read(path)
    counts, opcodes, events, other = Counter(), Counter(), {}, []
    for number, line in enumerate(raw.splitlines(), 1):
        if not line.strip():
            continue
        for kind, pattern in LOG_PATTERNS:
            match = pattern.fullmatch(line.strip())
            if match:
                address = int(match[1], 16)
                if address > 0xFFFFFFFF or address % 4:
                    raise ValueError(f'Invalid guest address in log line {number}')
                counts[kind] += 1
                # Keep every event and its opcode. Multiple diagnostics may
                # name the same address, and a supported exception is safe
                # only when all of them describe that exact opcode.
                opcode = match[2] if kind in ('unrecognized', 'missing_comparison') else None
                events.setdefault(address, []).append((kind, opcode))
                if kind == 'unrecognized':
                    opcodes[match[2]] += 1
                break
        else:
            if not re.fullmatch(r'Recompiling functions\.\.\. [0-9.]+%', line.strip()):
                raise ValueError(f'Unparsed diagnostic log line {number}: {line}')
    return events, {
        'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
        'lines': len(raw.splitlines()),
        'unrecognized_events': counts['unrecognized'],
        'decode_events': counts['decode'],
        'missing_switch_events': counts['missing_switch'],
        'missing_comparison_events': counts['missing_comparison'],
        'switch_outside_events': counts['switch_outside'],
        'unique_unsupported_addresses': len(events),
        'opcodes': dict(sorted(opcodes.items())),
        'other_lines': other,
    }


def parse_functions(source, filename):
    """Recognize this emitter's complete translation unit, tracking nested braces."""
    prefix = re.match(r'#include "ppc_recomp_shared.h"\r?\n\s*', source)
    if not prefix:
        raise ValueError(f'{filename}: unexpected source preamble')
    cursor = prefix.end()
    while cursor < len(source):
        match = ALIAS.match(source, cursor)
        if not match or match[1] != match[3] or match[1] != '__imp__' + match[2]:
            raise ValueError(f'{filename}: malformed function/alias at offset {cursor}')
        depth, end = 1, None
        for token in TOKENS.finditer(source, match.end()):
            if token[0] == '{':
                depth += 1
            elif token[0] == '}':
                depth -= 1
                if depth == 0:
                    end = token.end()
                    break
        if end is None:
            raise ValueError(f'{filename}: unterminated function {match[2]}')
        if source[match.end():end].count('PPC_FUNC_IMPL('):
            raise ValueError(f'{filename}: nested function definition')
        yield match[2], match.start(), match.end(), end, source[match.end():end - 1]
        cursor = end
        while cursor < len(source) and source[cursor].isspace():
            cursor += 1


def rewrite_time_base(body):
    # Only the exact paired instruction/emission is understood. Keep the guest
    # instruction comment so address/label auditing still describes the input.
    return TIME_BASE.subn(lambda match: match[1] + '\tctx.r' + match['register'] +
                         '.u64 = sfr::read_time_base();', body)


def rewrite_reservations(body):
    chunks, cursor, loads, stores = [], 0, 0, 0
    for instruction in RESERVATION.finditer(body):
        reg, ra, rb = instruction['reg'], instruction['ra'], instruction['rb']
        is_load = instruction['op'] in ('lwarx', 'ldarx')
        width = 64 if instruction['op'] in ('ldarx', 'stdcx.') else 32
        suffix = 'doubleword' if width == 64 else 'word'
        ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
        newline = '\r\n' if instruction[0].endswith('\r\n') else '\n'
        if is_load:
            lines = [f'\tctx.reserved.u{width} = *(uint{width}_t*)(base + {ea});',
                     f'\tctx.r{reg}.u64 = __builtin_bswap{width}(ctx.reserved.u{width});']
            replacement = f'\tctx.r{reg}.u64 = sfr::load_reserved_{suffix}(ctx, uint32_t({ea}));'
        else:
            lines = ['\tctx.cr0.lt = 0;', '\tctx.cr0.gt = 0;',
                     f'\tctx.cr0.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint{width}_t*>(base + {ea}), '
                     f'ctx.reserved.s{width}, __builtin_bswap{width}(ctx.r{reg}.s{width}));',
                     '\tctx.cr0.so = ctx.xer.so;']
            replacement = f'\tsfr::store_conditional_{suffix}(ctx, uint32_t({ea}), ctx.r{reg}.u{width});'
        expected = newline.join(lines) + newline
        if not body.startswith(expected, instruction.end()):
            continue
        # Match the whole emitted instruction, not a prefix followed by code
        # that can overwrite the checked result or perform extra side effects.
        following = body[instruction.end() + len(expected):]
        if not re.match(r'\s*(?:\t// |loc_[0-9A-Fa-f]+:|\Z)', following):
            continue
        chunks.append(body[cursor:instruction.end()])
        chunks.append(replacement + newline)
        cursor = instruction.end() + len(expected)
        loads += is_load
        stores += not is_load
    chunks.append(body[cursor:])
    return ''.join(chunks), loads, stores


def rewrite_vector_memory(body):
    chunks, cursor, loads, stores = [], 0, 0, 0
    for instruction in VECTOR_MEMORY.finditer(body):
        op, reg, ra, rb = (instruction[key] for key in ('op', 'reg', 'ra', 'rb'))
        if str(int(reg)) != reg or int(reg) > (127 if op.endswith('128') else 31):
            continue
        ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
        guest = f'(simde__m128i*)(base + (({ea}) & ~0xF))'
        register = f'(simde__m128i*)ctx.v{reg}.u8'
        is_load = op.startswith('l')
        dest, source = (register, guest) if is_load else (guest, register)
        newline = '\r\n' if instruction[0].endswith('\r\n') else '\n'
        expected = (f'\tsimde_mm_store_si128({dest}, simde_mm_shuffle_epi8(simde_mm_load_si128({source}), '
                    'simde_mm_load_si128((simde__m128i*)VectorMaskL)));' + newline)
        if not body.startswith(expected, instruction.end()):
            continue
        chunks.append(body[cursor:instruction.end()])
        hook = 'load_vector_memory' if is_load else 'store_vector_memory'
        chunks.append(f'\tsfr::{hook}(uint32_t({ea}), ctx.v{reg}.u8);' + newline)
        cursor = instruction.end() + len(expected)
        loads += is_load
        stores += not is_load
    chunks.append(body[cursor:])
    return ''.join(chunks), loads, stores


def rewrite_vector_word_stores(body):
    chunks, cursor, stores = [], 0, 0
    for instruction in VECTOR_WORD_STORE.finditer(body):
        op, reg, ra, rb = (instruction[key] for key in ('op', 'reg', 'ra', 'rb'))
        if str(int(reg)) != reg or int(reg) > (127 if op.endswith('128') else 31):
            continue
        ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
        newline = '\r\n' if instruction[0].endswith('\r\n') else '\n'
        assignment = f'\tea = ({ea}) & ~0x3;' + newline
        expected = assignment + f'\tPPC_STORE_U32(ea, ctx.v{reg}.u32[3 - ((ea & 0xF) >> 2)]);' + newline
        if not body.startswith(expected, instruction.end()):
            continue
        chunks.append(body[cursor:instruction.end()])
        # Preserve the emitter's local effective-address assignment as well as
        # the guest instruction comment. Only the actual store changes.
        chunks.append(assignment + f'\tsfr::store_vector_word(ea, ctx.v{reg}.u8);' + newline)
        cursor = instruction.end() + len(expected)
        stores += 1
    chunks.append(body[cursor:])
    return ''.join(chunks), stores


def rewrite_vector_partial_loads(body, address):
    """Replace exact pinned partial-load emissions, retaining wrapped uint32 EA."""
    instructions = list(INSTRUCTION.finditer(body))
    chunks, cursor, loads, invalid = [], 0, 0, []
    for index, instruction in enumerate(instructions):
        text = instruction[1].removesuffix('\r')
        if not text.lstrip().lower().startswith(('lvlx', 'lvrx')):
            continue
        match = re.fullmatch(r'(lvlx128|lvrx128|lvlx|lvrx) v([0-9]+),(0|r' + REGISTER +
                             r'),r(' + REGISTER + r')', text)
        start = instruction.end() + 1
        end = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        newline = '\r\n' if instruction[0].endswith('\r') else '\n'
        if match and body.startswith('\n', instruction.end()):
            op, reg, ra, rb = match.groups()
            ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
            left = op.startswith('lvlx')
            mask = 'L' if left else 'R'
            assignment = f'\ttemp.u32 = {ea};' + newline
            shuffle = ('simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)(base + '
                       '(temp.u32 & ~0xF))), simde_mm_load_si128((simde__m128i*)&VectorMask' +
                       mask + '[(temp.u32 & 0xF) * 16]))')
            value = shuffle if left else 'temp.u32 & 0xF ? ' + shuffle + ' : simde_mm_setzero_si128()'
            expected = assignment + f'\tsimde_mm_store_si128((simde__m128i*)ctx.v{reg}.u8, {value});' + newline
            block = body[start:end]
            following = block[len(expected):]
            if (str(int(reg)) == reg and int(reg) <= (127 if op.endswith('128') else 31) and
                    block.startswith(expected) and
                    not re.sub(r'^loc_[0-9A-Fa-f]+:(?:\r?\n|$)', '', following, flags=re.MULTILINE).strip()):
                hook = 'load_vector_left' if left else 'load_vector_right'
                chunks.extend((body[cursor:start], assignment +
                               f'\tsfr::{hook}(temp.u32, ctx.v{reg}.u8);' + newline))
                cursor = start + len(expected)
                loads += 1
                continue
        invalid.append(address + index * 4)
    chunks.append(body[cursor:])
    return ''.join(chunks), loads, invalid


def rewrite_vector_partial_stores(body, address):
    """Replace entire canonical left/right store loops, preserving exact EA."""
    instructions = list(INSTRUCTION.finditer(body))
    chunks, cursor, stores, invalid = [], 0, 0, []
    for index, instruction in enumerate(instructions):
        text = instruction[1].removesuffix('\r')
        if not text.lstrip().lower().startswith(('stvl', 'stvr')):
            continue
        match = re.fullmatch(r'(stvlx128|stvrx128|stvlx|stvrx) v([0-9]+),(0|r' + REGISTER +
                             r'),r(' + REGISTER + r')', text)
        start = instruction.end() + 1
        end = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        newline = '\r\n' if instruction[0].endswith('\r') else '\n'
        if match and body.startswith('\n', instruction.end()):
            op, reg, ra, rb = match.groups()
            ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
            left = op.startswith('stvlx')
            count = '16 - (ea & 0xF)' if left else 'ea & 0xF'
            target, element = ('ea + i', '15 - i') if left else ('ea - i - 1', 'i')
            assignment = f'\tea = {ea};' + newline
            expected = (assignment + f'\tfor (size_t i = 0; i < ({count}); i++)' + newline +
                        f'\t\tPPC_STORE_U8({target}, ctx.v{reg}.u8[{element}]);' + newline)
            block = body[start:end]
            following = block[len(expected):]
            if (str(int(reg)) == reg and int(reg) <= (127 if op.endswith('128') else 31) and
                    block.startswith(expected) and
                    not re.sub(r'^loc_[0-9A-Fa-f]+:(?:\r?\n|$)', '', following, flags=re.MULTILINE).strip()):
                hook = 'store_vector_left' if left else 'store_vector_right'
                chunks.extend((body[cursor:start], assignment + f'\tsfr::{hook}(ea, ctx.v{reg}.u8);' + newline))
                cursor = start + len(expected)
                stores += 1
                continue
        invalid.append(address + index * 4)
    chunks.append(body[cursor:])
    return ''.join(chunks), stores, invalid


def rewrite_cache_zero(body, address):
    """Replace complete canonical 32-byte dcbz or 128-byte dcbzl emissions."""
    instructions = list(INSTRUCTION.finditer(body))
    chunks, cursor, resolved, lines, invalid = [], 0, [], [], []
    for index, instruction in enumerate(instructions):
        text = instruction[1].removesuffix('\r')
        if not text.lstrip().lower().startswith('dcbz'):
            continue
        pc = address + index * 4
        plain = re.fullmatch(r'(dcbz|dcbzl) (0|r' + REGISTER + r'),r(' + REGISTER + r')', text)
        start = instruction.end() + 1
        end = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        newline = '\r\n' if instruction[0].endswith('\r') else '\n'
        if plain and body.startswith('\n', instruction.end()):
            op, ra, rb = plain.groups()
            width = 32 if op == 'dcbz' else 128
            ea = ('' if ra in ('0', 'r0') else f'ctx.{ra}.u32 + ') + f'ctx.r{rb}.u32'
            expected = f'\tmemset(base + (({ea}) & ~{width - 1}), 0, {width});' + newline
            block = body[start:end]
            following = block[len(expected):]
            if (block.startswith(expected) and
                    not re.sub(r'^loc_[0-9A-Fa-f]+:(?:\r?\n|$)', '', following, flags=re.MULTILINE).strip()):
                hook = 'zero_cache_block' if width == 32 else 'zero_cache_line'
                chunks.extend((body[cursor:start], f'\tsfr::{hook}(uint32_t({ea}));' + newline))
                cursor = start + len(expected)
                (resolved if width == 32 else lines).append(pc)
                continue
        invalid.append(pc)
    chunks.append(body[cursor:])
    return ''.join(chunks), resolved, lines, invalid


def rewrite_bdzf(body, address, events):
    """Resolve only logged empty relative bdzf blocks with verified local labels."""
    return _rewrite_counted_branch(body, address, events, 'bdzf', 'branch_counter_zero_false')


def rewrite_bdnzt(body, address, events):
    """Resolve logged empty relative bdnzt blocks (bare CR0 bit or 4*crN+bit)."""
    return _rewrite_counted_branch(body, address, events, 'bdnzt', 'branch_counter_nonzero_true')


def rewrite_summary_overflow_branches(body, address, events):
    """Resolve logged empty bso/bns (CR summary-overflow bit) local branches."""
    resolved, invalid = set(), []
    for opcode, negate in (('bso', ''), ('bns', '!')):
        body, found, bad = _rewrite_local_branch(
            body, address, events, opcode, opcode + r' (?:cr([0-7]),)?(0x[0-9a-f]+)',
            lambda match, negate=negate: f'{negate}ctx.cr{match[1] or 0}.so')
        resolved |= found
        invalid += bad
    return body, resolved, sorted(set(invalid))


# Record forms XenonRecomp translates without their condition-register update
# (logged "has RC bit enabled but no comparison was generated"). Integer forms
# compare like XenonRecomp's own record forms; vector compares set CR6.
MISSING_COMPARISONS = {
    'rlwimi.': lambda d: f'ctx.cr0.compare<int32_t>(ctx.r{d}.s32, 0, ctx.xer);',
    'rldicl.': lambda d: f'ctx.cr0.compare<int64_t>(ctx.r{d}.s64, 0, ctx.xer);',
    'vcmpgtuh.': lambda d: f'sfr::vmx::set_compare_cr6(ctx.cr6, ctx.v{d});',
}


def rewrite_missing_comparisons(body, address, events):
    """Append the missing CR update after a logged record-form translation."""
    instructions = list(INSTRUCTION.finditer(body))
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        pc = address + 4 * index
        address_events = events.get(pc, [])
        if not any(kind == 'missing_comparison' for kind, _ in address_events):
            continue
        text = instruction[1].removesuffix('\r')
        opcode, _, operands = text.partition(' ')
        destination = re.match(r'[rv]([0-9]+),', operands)
        if (opcode not in MISSING_COMPARISONS or not destination or
                any(event != ('missing_comparison', opcode) for event in address_events)):
            invalid.append(pc)
            continue
        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        boundary = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        block = body[line_end:boundary]
        label = re.search(r'^loc_[0-9A-Fa-f]+:', block, re.MULTILINE)
        code_end = line_end + (label.start() if label else len(block))
        if not body[line_end:code_end].strip():
            invalid.append(pc)  # nothing was translated to complete
            continue
        newline = '\r\n' if instruction[0].endswith('\r') else '\n'
        insertions.append((code_end, '\t' + MISSING_COMPARISONS[opcode](destination[1]) + newline))
        resolved.add(pc)
    for position, code in reversed(insertions):
        body = body[:position] + code + body[position:]
    return body, resolved, sorted(set(invalid))


def _rewrite_counted_branch(body, address, events, opcode, helper):
    return _rewrite_local_branch(
        body, address, events, opcode, opcode + r' (?:4\*cr([0-7])\+)?(lt|gt|eq|so),(0x[0-9a-f]+)',
        lambda match: f'sfr::{helper}(ctx.ctr.u64, ctx.cr{match[1] or 0}.{match[2]})')


def _rewrite_local_branch(body, address, events, opcode, syntax, condition):
    """Insert `if (condition) goto loc_target;` for a logged, empty branch whose
    last operand is a verified label inside this function."""
    instructions = list(INSTRUCTION.finditer(body))
    offsets = [item.start() for item in instructions]
    labels = list(re.finditer(r'^loc_([0-9A-Fa-f]+):', body, re.MULTILINE))
    end = address + len(instructions) * 4
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        pc = address + index * 4
        text = instruction[1].removesuffix('\r')
        plain = re.fullmatch(syntax, text)
        address_events = events.get(pc, [])
        matching_log = bool(address_events) and all(event == ('unrecognized', opcode)
                                                  for event in address_events)
        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        next_instruction = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        # Labels may separate adjacent instruction comments, but must not hide
        # a preexisting implementation or extra statements after the label.
        gap = body[line_end:next_instruction]
        empty = not re.sub(r'^loc_[0-9A-Fa-f]+:[ \t]*(?:\r?\n|$)', '', gap, flags=re.MULTILINE).strip()
        valid_target = False
        if plain:
            target_text = plain[plain.re.groups]
            target = int(target_text, 16)
            named = [label for label in labels if label[0] == f'loc_{target:X}:']
            valid_target = (hex(target) == target_text and address <= target < end and
                            target <= 0xFFFFFFFF and target % 4 == 0 and
                            -32768 <= target - pc <= 32764 and len(named) == 1 and
                            address + 4 * bisect_left(offsets, named[0].start()) == target)
            if valid_target:
                target_gap = body[named[0].end():offsets[(target - address) // 4]]
                # Match the exact label spelling recognized by checkpoint insertion.
                valid_target = (target_gap.startswith(('\n', '\r\n')) and
                                not re.sub(r'^loc_[0-9A-Fa-f]+:[ \t]*(?:\r?\n|$)', '',
                                           target_gap, flags=re.MULTILINE).strip())
        if plain and matching_log and empty and valid_target:
            newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            prefix = '' if line_end > instruction.end() else newline
            insertions.append((line_end, prefix +
                f'\tif ({condition(plain)}) goto loc_{target:X};' + newline))
            resolved.add(pc)
        elif text.lstrip().lower().startswith(opcode) or matching_log:
            invalid.append(pc)
    for position, code in reversed(insertions):
        body = body[:position] + code + body[position:]
    return body, resolved, sorted(set(invalid))


def rewrite_eqv(body, address, events):
    """Resolve logged empty plain eqv without changing CR or XER."""
    instructions = list(INSTRUCTION.finditer(body))
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        pc = address + 4 * index
        text = instruction[1].removesuffix('\r')
        plain = re.fullmatch(r'eqv r(' + REGISTER + r'),r(' + REGISTER + r'),r(' + REGISTER + r')', text)
        address_events = events.get(pc, [])
        matching_log = bool(address_events) and all(event == ('unrecognized', 'eqv') for event in address_events)
        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        boundary = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        gap = re.sub(r'^loc_[0-9A-Fa-f]+:\r?\n', '', body[line_end:boundary], flags=re.MULTILINE)
        if plain and matching_log and not gap.strip():
            destination, left, right = plain.groups()
            newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            prefix = '' if line_end > instruction.end() else newline
            insertions.append((line_end, prefix + f'\tctx.r{destination}.u64 = '
                               f'~(ctx.r{left}.u64 ^ ctx.r{right}.u64);' + newline))
            resolved.add(pc)
        elif text.lstrip().lower().startswith('eqv') or matching_log:
            invalid.append(pc)
    for position, code in reversed(insertions):
        body = body[:position] + code + body[position:]
    return body, resolved, invalid


def rewrite_addme(body, address, events):
    """Resolve exact logged empty plain-addme blocks with a checked CA input."""
    instructions = list(INSTRUCTION.finditer(body))
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        pc = address + 4 * index
        text = instruction[1].removesuffix('\r')
        plain = re.fullmatch(r'addme r(' + REGISTER + r'),r(' + REGISTER + r')', text)
        address_events = events.get(pc, [])
        matching_log = bool(address_events) and all(event == ('unrecognized', 'addme') for event in address_events)
        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        next_instruction = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        gap = body[line_end:next_instruction]
        empty = not re.sub(r'^loc_[0-9A-Fa-f]+:(?:\r?\n|$)', '', gap, flags=re.MULTILINE).strip()
        if plain and matching_log and empty:
            destination, source = plain.groups()
            newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            prefix = '' if line_end > instruction.end() else newline
            insertions.append((line_end, prefix + f'\tsfr::addme(ctx.r{destination}.u64, '
                               f'ctx.r{source}.u64, ctx.xer.ca);' + newline))
            resolved.add(pc)
        elif text.lstrip().lower().startswith('addme') or matching_log:
            invalid.append(pc)
    for position, code in reversed(insertions):
        body = body[:position] + code + body[position:]
    return body, resolved, invalid


def rewrite_addc(body, address, events):
    """Resolve only exact logged empty plain-addc blocks, without old-CA input."""
    instructions = list(INSTRUCTION.finditer(body))
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        pc = address + 4 * index
        text = instruction[1].removesuffix('\r')
        plain = re.fullmatch(r'addc r(' + REGISTER + r'),r(' + REGISTER + r'),r(' + REGISTER + r')', text)
        address_events = events.get(pc, [])
        matching_log = bool(address_events) and all(event == ('unrecognized', 'addc') for event in address_events)
        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        next_instruction = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        gap = body[line_end:next_instruction]
        empty = not re.sub(r'^loc_[0-9A-Fa-f]+:(?:\r?\n|$)', '', gap, flags=re.MULTILINE).strip()
        if plain and matching_log and empty:
            destination, left, right = plain.groups()
            newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            prefix = '' if line_end > instruction.end() else newline
            insertions.append((line_end, prefix + f'\tsfr::addc(ctx.r{destination}.u64, '
                               f'ctx.r{left}.u64, ctx.r{right}.u64, ctx.xer.ca);' + newline))
            resolved.add(pc)
        elif text.lstrip().lower().startswith('addc') or matching_log:
            invalid.append(pc)
    for position, code in reversed(insertions):
        body = body[:position] + code + body[position:]
    return body, resolved, invalid


def rewrite_subfze(body, address, events):
    """Rewrite only logged, exact, empty plain-subfze instruction blocks."""
    instructions = list(INSTRUCTION.finditer(body))
    labels = [match.start() for match in re.finditer(
        r'^[A-Za-z_][A-Za-z_0-9]*:', body, re.MULTILINE)]
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        instruction_address = address + 4 * index
        text = instruction[1].removesuffix('\r')
        # subfze. also records the result in CR0, as XenonRecomp's record forms do.
        plain = re.fullmatch(r'subfze(\.?) r(' + REGISTER + r'),r(' + REGISTER + r')', text)
        normalized = text.lstrip().lower()
        related = normalized.startswith(('subfze', 'sfze'))
        address_events = events.get(instruction_address, [])
        matching_log = bool(address_events) and plain is not None and all(
            event == ('unrecognized', 'subfze' + plain[1]) for event in address_events)

        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        next_instruction = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        next_label = min((position for position in labels if position >= line_end), default=len(body))
        boundary = min(next_instruction, next_label)
        empty_block = not body[line_end:boundary].strip()

        if plain and matching_log and empty_block:
            record, destination, source = plain.groups()
            newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            prefix = '' if line_end > instruction.end() else newline
            compare = f' ctx.cr0.compare<int32_t>(ctx.r{destination}.s32, 0, ctx.xer);' if record else ''
            insertions.append((line_end, prefix + f'\tsfr::subfze(ctx.r{destination}.u64, '
                               f'ctx.r{source}.u64, ctx.xer.ca);' + compare + newline))
            resolved.add(instruction_address)
        elif related or (matching_log and not plain):
            invalid.append(instruction_address)

    for position, hook in reversed(insertions):
        body = body[:position] + hook + body[position:]
    return body, resolved, sorted(set(invalid))


def rewrite_sthu(body, address, events):
    """Rewrite only logged, canonical, empty store-halfword-update blocks."""
    instructions = list(INSTRUCTION.finditer(body))
    labels = [match.start() for match in re.finditer(
        r'^[A-Za-z_][A-Za-z_0-9]*:', body, re.MULTILINE)]
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        instruction_address = address + 4 * index
        text = instruction[1].removesuffix('\r')
        plain = re.fullmatch(r'sthu r(' + REGISTER + r'),(0|-?[1-9][0-9]{0,4})'
                             r'\(r([1-9]|[12][0-9]|3[01])\)', text)
        valid_operands = plain is not None and -32768 <= int(plain[2]) <= 32767
        # The indexed form (sthux) is handled with the supplemental instructions.
        related = text.lstrip().lower().startswith('sthu') and not text.lstrip().lower().startswith('sthux')
        address_events = events.get(instruction_address, [])
        matching_log = bool(address_events) and all(
            event == ('unrecognized', 'sthu') for event in address_events)

        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        next_instruction = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        next_label = min((position for position in labels if position >= line_end), default=len(body))
        boundary = min(next_instruction, next_label)
        empty_block = not body[line_end:boundary].strip()

        if valid_operands and matching_log and empty_block:
            source, displacement, base = plain.groups()
            newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            prefix = '' if line_end > instruction.end() else newline
            insertions.append((line_end, prefix + '\tsfr::store_halfword_update(*sfr::active_memory, '
                               f'ctx.r{base}.u64, ctx.r{source}.u64, {displacement});' + newline))
            resolved.add(instruction_address)
        elif related or matching_log:
            invalid.append(instruction_address)

    for position, hook in reversed(insertions):
        body = body[:position] + hook + body[position:]
    return body, resolved, sorted(set(invalid))


def rewrite_stfsu(body, address, events):
    """Translate logged empty stfsu blocks using the FPR's raw double-format bits."""
    instructions = list(INSTRUCTION.finditer(body))
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        instruction_address = address + 4 * index
        text = instruction[1].removesuffix('\r')
        plain = re.fullmatch(r'stfsu f(' + REGISTER + r'),(0|-?[1-9][0-9]{0,4})'
                             r'\(r([1-9]|[12][0-9]|3[01])\)', text)
        valid_operands = plain is not None and -32768 <= int(plain[2]) <= 32767
        related = text.lstrip().lower().startswith('stfsu')
        address_events = events.get(instruction_address, [])
        matching_log = bool(address_events) and all(
            event == ('unrecognized', 'stfsu') for event in address_events)
        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        boundary = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        # Labels may separate instructions, but must not hide extra statements.
        gap = re.sub(r'^loc_[0-9A-Fa-f]+:\r?\n', '', body[line_end:boundary], flags=re.MULTILINE)
        empty_block = not gap.strip()
        if valid_operands and matching_log and empty_block:
            source, displacement, base = plain.groups()
            newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            prefix = '' if line_end > instruction.end() else newline
            insertions.append((line_end, prefix + '\tsfr::store_float_single_update(*sfr::active_memory, '
                               f'ctx.r{base}.u64, ctx.f{source}.u64, {displacement});' + newline))
            resolved.add(instruction_address)
        elif related or matching_log:
            invalid.append(instruction_address)
    for position, hook in reversed(insertions):
        body = body[:position] + hook + body[position:]
    return body, resolved, sorted(set(invalid))


def rewrite_lhzu(body, address, events):
    """Rewrite only logged, canonical, empty load-halfword-update blocks."""
    instructions = list(INSTRUCTION.finditer(body))
    labels = [match.start() for match in re.finditer(
        r'^[A-Za-z_][A-Za-z_0-9]*:', body, re.MULTILINE)]
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        instruction_address = address + 4 * index
        text = instruction[1].removesuffix('\r')
        plain = re.fullmatch(r'lhzu r(' + REGISTER + r'),(0|-?[1-9][0-9]{0,4})'
                             r'\(r([1-9]|[12][0-9]|3[01])\)', text)
        valid_operands = (plain is not None and -32768 <= int(plain[2]) <= 32767
                          and plain[1] != plain[3])
        # The indexed form (lhzux) is handled with the supplemental instructions.
        related = text.lstrip().lower().startswith('lhzu') and not text.lstrip().lower().startswith('lhzux')
        address_events = events.get(instruction_address, [])
        matching_log = bool(address_events) and all(
            event == ('unrecognized', 'lhzu') for event in address_events)

        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        next_instruction = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        next_label = min((position for position in labels if position >= line_end), default=len(body))
        boundary = min(next_instruction, next_label)
        empty_block = not body[line_end:boundary].strip()

        if valid_operands and matching_log and empty_block:
            target, displacement, base = plain.groups()
            newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            prefix = '' if line_end > instruction.end() else newline
            insertions.append((line_end, prefix + '\tsfr::load_halfword_update('
                               '*sfr::active_memory, '
                               f'ctx.r{target}.u64, ctx.r{base}.u64, {displacement});' + newline))
            resolved.add(instruction_address)
        elif related or matching_log:
            invalid.append(instruction_address)

    for position, hook in reversed(insertions):
        body = body[:position] + hook + body[position:]
    return body, resolved, sorted(set(invalid))


_GPR = r'r(' + REGISTER + r')'
_FPR = r'f(' + REGISTER + r')'
_VR = r'v([0-9]|[1-9][0-9]|1[01][0-9]|12[0-7])'
_DISPLACEMENT = r'(0|-?[1-9][0-9]{0,4})'
_MEMORY = '*sfr::active_memory'


def _offset(displacement):
    value = int(displacement)
    if not -32768 <= value <= 32767:
        raise ValueError('displacement out of range')
    return f'uint64_t(int64_t({value}))'


def _operands(indexed, first, second):
    """Return (base register, offset expression): X-form is RA,RB; D-form is D(RA)."""
    return (first, f'ctx.r{second}.u64') if indexed else (second, _offset(first))


def _load_update(kind, indexed):
    def emit(target, first, second):
        base, offset = _operands(indexed, first, second)
        if base == '0' or base == target:
            raise ValueError('invalid load-with-update registers')  # RA=0 or RA=RT is invalid
        return f'sfr::load_update<{kind}>({_MEMORY}, ctx.r{target}.u64, ctx.r{base}.u64, {offset});'
    return emit


def _float_load_update(function, indexed):
    def emit(target, first, second):
        base, offset = _operands(indexed, first, second)
        if base == '0':
            raise ValueError('invalid load-with-update base')
        return f'sfr::{function}({_MEMORY}, ctx.f{target}.u64, ctx.r{base}.u64, {offset});'
    return emit


def _store_update(kind, register_file, indexed):
    def emit(source, first, second):
        base, offset = _operands(indexed, first, second)
        if base == '0':
            raise ValueError('invalid store-with-update base')
        return (f'sfr::store_update<{kind}>({_MEMORY}, ctx.r{base}.u64, '
                f'ctx.{register_file}{source}.u64, {offset});')
    return emit


def _vector(name, record=False):
    def emit(destination, a, b):
        statement = f'sfr::vmx::{name}(ctx.v{destination}, ctx.v{a}, ctx.v{b});'
        if record:
            statement += f' sfr::vmx::set_compare_cr6(ctx.cr6, ctx.v{destination});'
        return statement
    return emit


def _splat_halfword(destination, value):
    if not -16 <= int(value) <= 15:
        raise ValueError('vspltish immediate out of range')
    return f'sfr::vmx::vspltish(ctx.v{destination}, int16_t({int(value)}));'


# Instructions the pinned XenonRecomp logs as unrecognized and emits as an
# empty block. Each entry: exact operand syntax and the statement to insert.
_D_FORM = _DISPLACEMENT + r'\(' + _GPR + r'\)'
SUPPLEMENTAL_INSTRUCTIONS = {
    'lbzux': (_GPR + ',' + _GPR + ',' + _GPR, _load_update('uint8_t', True)),
    'lhzux': (_GPR + ',' + _GPR + ',' + _GPR, _load_update('uint16_t', True)),
    'lwzux': (_GPR + ',' + _GPR + ',' + _GPR, _load_update('uint32_t', True)),
    'ldux': (_GPR + ',' + _GPR + ',' + _GPR, _load_update('uint64_t', True)),
    'lhau': (_GPR + ',' + _D_FORM, _load_update('int16_t', False)),
    'lfsu': (_FPR + ',' + _D_FORM, _float_load_update('load_single_update', False)),
    'lfsux': (_FPR + ',' + _GPR + ',' + _GPR, _float_load_update('load_single_update', True)),
    'lfdu': (_FPR + ',' + _D_FORM, _float_load_update('load_double_update', False)),
    'lfdux': (_FPR + ',' + _GPR + ',' + _GPR, _float_load_update('load_double_update', True)),
    'stbux': (_GPR + ',' + _GPR + ',' + _GPR, _store_update('uint8_t', 'r', True)),
    'sthux': (_GPR + ',' + _GPR + ',' + _GPR, _store_update('uint16_t', 'r', True)),
    'stdux': (_GPR + ',' + _GPR + ',' + _GPR, _store_update('uint64_t', 'r', True)),
    'stfdu': (_FPR + ',' + _D_FORM, _store_update('uint64_t', 'f', False)),
    'stfdux': (_FPR + ',' + _GPR + ',' + _GPR, _store_update('uint64_t', 'f', True)),
    'vspltish': (_VR + r',(-?[0-9]{1,2})', _splat_halfword),
    'vcfpuxws128': (_VR + ',' + _VR + r',([0-9]|[12][0-9]|3[01])',
                    lambda d, b, scale: f'sfr::vmx::vcfpuxws(ctx.v{d}, ctx.v{b}, {int(scale)});'),
    'vsel128': (_VR + ',' + _VR + ',' + _VR + ',' + _VR,
                lambda d, a, b, c: f'sfr::vmx::vsel(ctx.v{d}, ctx.v{a}, ctx.v{b}, ctx.v{c});'),
}
for _name in ('vslh', 'vsrh', 'vsrah', 'vrlh', 'vsrab', 'vaddsbs', 'vaddsws', 'vadduhs', 'vsubshs',
              'vsubuhs', 'vsububm', 'vmaxsh', 'vminsh', 'vmaxuh', 'vminuh', 'vmaxuw', 'vminuw', 'vavguh',
              'vcmpequh', 'vcmpgtsh', 'vcmpgtuw', 'vcmpgtsw'):
    SUPPLEMENTAL_INSTRUCTIONS[_name] = (_VR + ',' + _VR + ',' + _VR, _vector(_name))
for _name in ('vcmpequh', 'vcmpgtsh', 'vcmpgtuw', 'vcmpgtsw'):
    SUPPLEMENTAL_INSTRUCTIONS[_name + '.'] = (_VR + ',' + _VR + ',' + _VR, _vector(_name, record=True))


# Condition-register bit operands: a bare bit names cr0, otherwise 4*crN+bit.
_CR_BIT = r'(?:4\*cr([0-7])\+)?(lt|gt|eq|so)'


def _cr_bit(field, bit):
    return f'ctx.cr{field or 0}.{bit}'


def _cr_logic(expression):
    def emit(d_field, d_bit, a_field, a_bit, b_field, b_bit):
        a, b = _cr_bit(a_field, a_bit), _cr_bit(b_field, b_bit)
        return f'{_cr_bit(d_field, d_bit)} = {expression.format(a=a, b=b)};'
    return emit


# Vector packs: two source vectors narrowed into one. In XenonRecomp's
# byte-reversed registers the guest's first half (from vA) is the host's upper
# half, so host lane i < n takes vB's lane i and lane n + i takes vA's lane i.
for _name in ('vpkswss', 'vpkswus', 'vpkuwum', 'vpkuwus', 'vpkuhus', 'vpkshss'):
    SUPPLEMENTAL_INSTRUCTIONS[_name] = (_VR + ',' + _VR + ',' + _VR, _vector(_name))
    SUPPLEMENTAL_INSTRUCTIONS[_name + '128'] = (_VR + ',' + _VR + ',' + _VR, _vector(_name))
SUPPLEMENTAL_INSTRUCTIONS['vslo'] = (_VR + ',' + _VR + ',' + _VR, _vector('vslo'))
SUPPLEMENTAL_INSTRUCTIONS['vslo128'] = (_VR + ',' + _VR + ',' + _VR, _vector('vslo'))
# lvehx: the halfword at EA & ~1 lands in its element; other elements are
# undefined by the architecture and keep their previous value here.
SUPPLEMENTAL_INSTRUCTIONS['lvehx'] = (
    _VR + r',(0|r' + REGISTER + r'),' + _GPR,
    lambda d, a, b: ('{ const uint32_t ea = (' + ('0' if a == '0' else f'ctx.{a}.u32') +
                     f' + ctx.r{b}.u32) & ~1u; ctx.v{d}.u16[7 - ((ea & 15) >> 1)] = PPC_LOAD_U16(ea); }}'))


SUPPLEMENTAL_INSTRUCTIONS['cror'] =(_CR_BIT + ',' + _CR_BIT + ',' + _CR_BIT, _cr_logic('({a} | {b}) & 1'))
SUPPLEMENTAL_INSTRUCTIONS['crorc'] = (_CR_BIT + ',' + _CR_BIT + ',' + _CR_BIT, _cr_logic('({a} | !{b}) & 1'))


def rewrite_supplemental(body, address, events):
    """Translate logged, canonical, empty blocks of SUPPLEMENTAL_INSTRUCTIONS.

    Like the per-instruction rewrites above, an instruction qualifies only when
    every log event at its address names exactly this opcode as unrecognized,
    its operands match the exact syntax and are valid, and XenonRecomp emitted
    nothing for it (labels may follow). Anything else is reported invalid.
    """
    instructions = list(INSTRUCTION.finditer(body))
    insertions, resolved, invalid = [], set(), []
    for index, instruction in enumerate(instructions):
        instruction_address = address + 4 * index
        text = instruction[1].removesuffix('\r')
        opcode, _, operands = text.partition(' ')
        if opcode not in SUPPLEMENTAL_INSTRUCTIONS:
            continue
        address_events = events.get(instruction_address, [])
        if not address_events or any(event != ('unrecognized', opcode) for event in address_events):
            continue  # not logged as unsupported: XenonRecomp translated it
        syntax, emit = SUPPLEMENTAL_INSTRUCTIONS[opcode]
        line_end = instruction.end()
        if body.startswith('\n', line_end):
            line_end += 1
        boundary = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        gap = re.sub(r'^loc_[0-9A-Fa-f]+:\r?\n', '', body[line_end:boundary], flags=re.MULTILINE)
        operand_match = re.fullmatch(syntax, operands)
        try:
            statement = emit(*operand_match.groups()) if operand_match and not gap.strip() else None
        except ValueError:
            statement = None
        if statement is None:
            invalid.append(instruction_address)
            continue
        newline = '\r\n' if instruction[0].endswith('\r') else '\n'
        prefix = '' if line_end > instruction.end() else newline
        insertions.append((line_end, prefix + '\t' + statement + newline))
        resolved.add(instruction_address)
    for position, statement in reversed(insertions):
        body = body[:position] + statement + body[position:]
    return body, resolved, sorted(set(invalid))


def validate_vector_partial_masks(header):
    """All 16 rows of each pinned byte-shuffle table determine partial loads."""
    for name in ('VectorMaskL', 'VectorMaskR'):
        declarations = re.findall(r'inline uint8_t ' + name + r'\[\] =\s*\{([^}]+)\};', header)
        if len(declarations) != 1 or len(re.findall(r'\b' + name + r'\s*\[', header)) != 1:
            raise ValueError('Missing or ambiguous ' + name + ' declaration')
        tokens = declarations[0].strip().removesuffix(',').split(',')
        if len(tokens) != 256 or any(not re.fullmatch(r'\s*(?:0x[0-9A-Fa-f]+|0|[1-9][0-9]*)\s*', token)
                                     for token in tokens):
            raise ValueError('Unrecognized ' + name + ' initializer')
        values = [int(token.strip(), 16 if token.strip().startswith('0x') else 10) for token in tokens]
        expected = [(0xff if j < k else 15 + k - j) if name == 'VectorMaskL'
                    else (k - 1 - j if j < k else 0xff)
                    for k in range(16) for j in range(16)]
        if values != expected:
            raise ValueError(name + ' differs from the pinned partial-vector byte mask')


def validate_vector_mask(header):
    declarations = re.findall(r'inline uint8_t VectorMaskL\[\] =\s*\{([^}]+)\};', header)
    if len(declarations) != 1:
        raise ValueError('Missing or ambiguous VectorMaskL declaration')
    tokens = declarations[0].strip().rstrip(',').split(',')
    if len(tokens) < 16 or any(not re.fullmatch(r'\s*(?:0x[0-9A-Fa-f]+|0|[1-9][0-9]*)\s*', token)
                               for token in tokens):
        raise ValueError('Unrecognized VectorMaskL initializer')
    values = [int(token.strip(), 16 if token.strip().startswith('0x') else 10) for token in tokens]
    if values[:16] != list(range(15, -1, -1)) or any(value > 255 for value in values):
        raise ValueError('VectorMaskL does not reverse the full vector')


# These targets are the exact distinct entries reached by the original 105-byte
# country table, audited in out/user-country-next-audit.json. They remain local
# instruction addresses, never new guest function mappings.
COUNTRY_CTR = {
    0x824D1BA8: (0x824D1C18, (
        0x824D1C1C, 0x824D1C24, 0x824D1C2C, 0x824D1C34, 0x824D1C3C,
        0x824D1C44, 0x824D1C4C, 0x824D1C54, 0x824D1C5C, 0x824D1C64,
        0x824D1C6C, 0x824D1C74, 0x824D1C7C, 0x824D1C84, 0x824D1C8C,
        0x824D1C94, 0x824D1C9C, 0x824D1CA4, 0x824D1CAC, 0x824D1CB4,
        0x824D1CBC, 0x824D1CC4, 0x824D1CCC, 0x824D1CD4, 0x824D1CDC,
        0x824D1CE4, 0x824D1CEC, 0x824D1CF4, 0x824D1CFC, 0x824D1D04,
        0x824D1D0C, 0x824D1D14, 0x824D1D1C, 0x824D1D24, 0x824D1D2C,
        0x824D1D34, 0x824D1D3C, 0x824D1D44,
    ), '323a22fa66dd76eda2f701f414cf8c6fdf23a2d71377567178cbc9ee587b94de'),
}


# Exact local destinations of the audited formatting routine's BE16 table.
FORMAT_CTR = {
    0x82A68220: (0x82A68658, (
        0x82A6865C, 0x82A6866C, 0x82A686C8, 0x82A68744, 0x82A68754,
        0x82A687FC, 0x82A68838, 0x82A68844, 0x82A68968, 0x82A6896C,
        0x82A68974, 0x82A68978, 0x82A68980, 0x82A689B0, 0x82A68B28,
    ), 'f25c067c64663a6d5e2a8f596e6f2bb053d7b6d3c73a160766a441d49805cdd7'),
}


def _format_ctr_layout(body, address):
    """Pin the original routine; restore its one unlinked local CTR branch."""
    branch, targets, expected_hash = FORMAT_CTR[address]
    if hashlib.sha256(body.replace('\r\n', '\n').encode('utf-8')).hexdigest() != expected_hash:
        raise ValueError('Format CTR whole-function hash mismatch')
    instructions = list(INSTRUCTION.finditer(body))
    offsets = [item.start() for item in instructions]
    end = address + 4 * len(instructions)
    if (not instructions or address < 0 or address % 4 or end > 0x100000000 or
            branch < address or branch >= end or branch % 4 or not targets or
            len(set(targets)) != len(targets) or
            any(target <= branch or target >= end or target % 4 for target in targets)):
        raise ValueError('Format CTR policy has invalid instruction addresses')

    def empty_label_gap(gap):
        return not re.sub(r'^loc_[0-9A-F]+:(?:\r?\n|$)', '', gap,
                          flags=re.MULTILINE).strip()

    labels = {}
    for label in re.finditer(r'^[ \t]*([A-Za-z_][A-Za-z_0-9]*):[^\r\n]*(?:\r?\n|$)',
                             body, re.MULTILINE):
        spelling = re.fullmatch(r'loc_([0-9A-F]+):(?:\r?\n|$)', label[0])
        if not spelling:
            raise ValueError('Format CTR contains a noncanonical local label')
        pc = int(spelling[1], 16)
        index = bisect_left(offsets, label.start())
        if (spelling[1] != f'{pc:X}' or pc in labels or index >= len(instructions) or
                pc != address + 4 * index or
                not empty_label_gap(body[label.end():offsets[index]])):
            raise ValueError('Format CTR local label is duplicate or misplaced')
        labels[pc] = label.start()

    branch_index = (branch - address) // 4
    # Unlike the country wrapper, this routine also contains real linked calls.
    # They keep their original LR assignments and indirect call statements.
    annotations = [item[1].removesuffix('\r') for item in instructions]
    if (annotations[branch_index] != 'bctr ' or
            sum(text.strip().startswith('bctr') and text != 'bctrl '
                for text in annotations) != 1):
        raise ValueError('Format CTR policy does not identify one plain bctr')
    instruction = instructions[branch_index]
    newline = '\r\n' if instruction[0].endswith('\r') else '\n'
    begin = instruction.end() + 1
    stop = instructions[branch_index + 1].start() if branch_index + 1 < len(instructions) else len(body)
    expected = ('\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\treturn;\n').replace('\n', newline)
    if (not body.startswith('\n', instruction.end()) or not body.startswith(expected, begin) or
            not empty_label_gap(body[begin + len(expected):stop])):
        raise ValueError('Format CTR branch has noncanonical emitted statements')
    return targets, instructions, labels, begin, begin + len(expected), newline


def validate_format_ctr(body, address):
    if address not in FORMAT_CTR:
        return None
    return _format_ctr_layout(body, address)[0]


def rewrite_format_ctr(body, address, original_body):
    if validate_format_ctr(original_body, address) is None:
        return body
    # Must run before ordinary instruction rewrites (notably the logged lhzu).
    # Both inputs must still match every statement of the audited original body.
    targets, instructions, labels, begin, end, newline = _format_ctr_layout(body, address)
    replacement = '\tswitch (ctx.ctr.u32) {' + newline
    replacement += ''.join(f'\tcase 0x{target:08X}: goto loc_{target:X};' + newline
                           for target in targets)
    replacement += ('\tdefault: throw sfr::RuntimeStop("format-ctr-target", ctx.ctr.u32, '
                    '"unrecognized local format branch target");' + newline + '\t}' + newline)
    edits = [(begin, end, replacement)]
    for target in targets:
        if target not in labels:
            instruction = instructions[(target - address) // 4]
            label_newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            edits.append((instruction.start(), instruction.start(), f'loc_{target:X}:' + label_newline))
    for begin, end, replacement in sorted(edits, reverse=True):
        body = body[:begin] + replacement + body[end:]
    return body


def _country_ctr_layout(body, address):
    """Check the audited body and locate only canonical local branch targets."""
    branch, targets, expected_hash = COUNTRY_CTR[address]
    canonical = body.replace('\r\n', '\n')
    if hashlib.sha256(canonical.encode('utf-8')).hexdigest() != expected_hash:
        raise ValueError('Country CTR whole-function hash mismatch')
    instructions = list(INSTRUCTION.finditer(body))
    offsets = [item.start() for item in instructions]
    end = address + 4 * len(instructions)
    if (not instructions or address < 0 or address % 4 or end > 0x100000000 or
            branch < address or branch >= end or branch % 4 or not targets or
            len(set(targets)) != len(targets) or
            any(target <= branch or target >= end or target % 4 for target in targets)):
        raise ValueError('Country CTR policy has invalid instruction addresses')

    def empty_label_gap(gap):
        return not re.sub(r'^loc_[0-9A-F]+:(?:\r?\n|$)', '', gap,
                          flags=re.MULTILINE).strip()

    labels = {}
    for label in re.finditer(r'^[ \t]*([A-Za-z_][A-Za-z_0-9]*):[^\r\n]*(?:\r?\n|$)',
                             body, re.MULTILINE):
        spelling = re.fullmatch(r'loc_([0-9A-F]+):(?:\r?\n|$)', label[0])
        if not spelling:
            raise ValueError('Country CTR contains a noncanonical local label')
        pc = int(spelling[1], 16)
        index = bisect_left(offsets, label.start())
        if (spelling[1] != f'{pc:X}' or pc in labels or index >= len(instructions) or
                pc != address + 4 * index or
                not empty_label_gap(body[label.end():offsets[index]])):
            raise ValueError('Country CTR local label is duplicate or misplaced')
        labels[pc] = label.start()

    def block(index, expected):
        instruction = instructions[index]
        newline = '\r\n' if instruction[0].endswith('\r') else '\n'
        begin = instruction.end() + 1
        stop = instructions[index + 1].start() if index + 1 < len(instructions) else len(body)
        expected = expected.replace('\n', newline)
        if (not body.startswith('\n', instruction.end()) or
                not body.startswith(expected, begin) or
                not empty_label_gap(body[begin + len(expected):stop])):
            raise ValueError('Country CTR instruction has noncanonical emitted statements')
        return begin, begin + len(expected), newline

    branch_index = (branch - address) // 4
    if (instructions[branch_index][1].removesuffix('\r') != 'bctr ' or
            sum(item[1].removesuffix('\r').strip().startswith('bctr')
                for item in instructions) != 1):
        raise ValueError('Country CTR policy does not identify one plain bctr')
    branch_begin, branch_end, newline = block(
        branch_index, '\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\treturn;\n')
    for target in targets:
        index = (target - address) // 4
        assignment = re.fullmatch(r'li r11,(0|-?[1-9][0-9]*)',
                                  instructions[index][1].removesuffix('\r'))
        if not assignment or not -32768 <= int(assignment[1]) <= 32767:
            raise ValueError('Country CTR target is not a canonical original case instruction')
        block(index, f'\tctx.r11.s64 = {assignment[1]};\n')
    return targets, instructions, labels, branch_begin, branch_end, newline


def validate_country_ctr(body, address):
    if address not in COUNTRY_CTR:
        return None
    return _country_ctr_layout(body, address)[0]


def rewrite_country_ctr(body, address, original_body):
    if validate_country_ctr(original_body, address) is None:
        return body
    # Revalidate emission, including every C++ statement. This audited function
    # has no ordinary instruction rewrite that needs to change its original body.
    targets, instructions, labels, begin, end, newline = _country_ctr_layout(body, address)
    original_comments = [item[1].removesuffix('\r') for item in INSTRUCTION.finditer(original_body)]
    if original_comments != [item[1].removesuffix('\r') for item in instructions]:
        raise ValueError('Country CTR instruction comments changed before emission')
    replacement = '\tswitch (ctx.ctr.u32) {' + newline
    replacement += ''.join(f'\tcase 0x{target:08X}: goto loc_{target:X};' + newline
                           for target in targets)
    replacement += ('\tdefault: throw sfr::RuntimeStop("country-ctr-target", ctx.ctr.u32, '
                    '"unrecognized local country branch target");' + newline + '\t}' + newline)
    edits = [(begin, end, replacement)]
    for target in targets:
        if target not in labels:
            instruction = instructions[(target - address) // 4]
            label_newline = '\r\n' if instruction[0].endswith('\r') else '\n'
            edits.append((instruction.start(), instruction.start(), f'loc_{target:X}:' + label_newline))
    for begin, end, replacement in sorted(edits, reverse=True):
        body = body[:begin] + replacement + body[end:]
    return body


def load_jump_tables(path):
    """Read switches.toml as {bctr address: (index register, labels)}."""
    if path is None or not Path(path).is_file():
        return {}
    tables = {}
    for entry in tomllib.loads(read(Path(path))).get('switch', []):
        base, register, labels = entry['base'], entry['r'], entry['labels']
        if (base in tables or base % 4 or not 0 <= register < 32 or not labels or
                any(label % 4 or not 0 <= label <= 0xFFFFFFFF for label in labels)):
            raise ValueError(f'Invalid jump table entry at 0x{base:08X}')
        tables[base] = (register, tuple(labels))
    return tables


def rewrite_jump_tables(body, address, tables):
    """Dispatch each configured bctr on its computed CTR target.

    XenonRecomp emits `switch (ctx.rN.u64)` over the table index with an
    unreachable default. Switching on the CTR value the original code just
    computed keeps the original semantics without trusting the index register
    or its upper 32 bits, and any target outside the verified labels stops.
    Returns (body, rewritten bctr addresses, invalid bctr addresses).
    """
    instructions = list(INSTRUCTION.finditer(body))
    edits, rewritten, invalid = [], [], []
    for index, instruction in enumerate(instructions):
        pc = address + 4 * index
        if pc not in tables:
            continue
        register, labels = tables[pc]
        newline = '\r\n' if instruction[0].endswith('\r') else '\n'
        begin = instruction.end() + 1
        expected = (f'\tswitch (ctx.r{register}.u64) {{\n' +
                    ''.join(f'\tcase {case}:\n\t\tgoto loc_{label:X};\n' for case, label in enumerate(labels)) +
                    '\tdefault:\n\t\t__builtin_unreachable();\n\t}\n').replace('\n', newline)
        if (instruction[1].removesuffix('\r') != 'bctr ' or not body.startswith('\n', instruction.end())
                or not body.startswith(expected, begin)):
            invalid.append(pc)
            continue
        replacement = '\tswitch (ctx.ctr.u32) {' + newline
        replacement += ''.join(f'\tcase 0x{label:08X}: goto loc_{label:X};' + newline
                               for label in sorted(set(labels)))
        replacement += ('\tdefault: throw sfr::RuntimeStop("jump-table-target", ctx.ctr.u32, '
                        '"computed jump table target is not a verified case label");' + newline +
                        '\t}' + newline)
        edits.append((begin, begin + len(expected), replacement))
        rewritten.append(pc)
    for begin, end, replacement in sorted(edits, reverse=True):
        body = body[:begin] + replacement + body[end:]
    return body, rewritten, invalid


NATIVE_RESOURCE_COHERENCY = {
    0x824F1C08: (0x824F1D88, 0x824F1E64,
                 'cb1c6b2ed0c4104235335296d550032ee67ec39cac16006b35e5401e7bf4564a'),
}


def _native_resource_coherency_span(body, address):
    begin, end, _ = NATIVE_RESOURCE_COHERENCY[address]
    instructions = list(INSTRUCTION.finditer(body))
    function_end = address + 4 * len(instructions)
    if not address <= begin < end < function_end or begin % 4 or end % 4:
        raise ValueError('Invalid native resource coherency instruction range')
    start = instructions[(begin - address) // 4].start()
    end_comment = instructions[(end - address) // 4].start()
    labels = list(re.finditer(r'^loc_([0-9A-Fa-f]+):', body, re.MULTILINE))
    ending = [label for label in labels if label[0] == f'loc_{end:X}:']
    if len(ending) != 1:
        raise ValueError('Missing or duplicate native resource coherency ending label')
    boundary = ending[0]
    offsets = [item.start() for item in instructions]
    gap = body[boundary.end():end_comment]
    if (address + 4 * bisect_left(offsets, boundary.start()) != end or
            not gap.startswith(('\n', '\r\n')) or gap.strip()):
        raise ValueError('Noncanonical or misplaced native resource coherency ending label')
    stop = boundary.start()
    for branch in re.finditer(r'\bgoto\s+(' + IDENT + r')\s*;', body):
        target = re.fullmatch(r'loc_([0-9A-Fa-f]+)', branch[1])
        inside = start <= branch.start() < stop
        if target is None:
            if inside:
                raise ValueError('Unknown branch exit from native resource coherency block')
            continue
        target = int(target[1], 16)
        if not inside and begin <= target < end:
            raise ValueError('External branch enters native resource coherency block')
        if inside and not begin <= target <= end:
            raise ValueError('Native resource coherency block has a different branch exit')
    if re.search(r'\breturn\b', body[start:stop]):
        raise ValueError('Native resource coherency block returns before its ending label')
    return start, stop


def validate_native_resource_coherency(body, address):
    """Admit only the audited original body, before any ordinary instruction rewrite."""
    if address not in NATIVE_RESOURCE_COHERENCY:
        return None
    expected_hash = NATIVE_RESOURCE_COHERENCY[address][2]
    canonical = body.replace('\r\n', '\n')
    if hashlib.sha256(canonical.encode('utf-8')).hexdigest() != expected_hash:
        raise ValueError('Native resource coherency whole-function hash mismatch')
    return _native_resource_coherency_span(body, address)


def rewrite_native_resource_coherency(body, address, original_body):
    """Replace the native block last, retaining original comments and the exit label."""
    if validate_native_resource_coherency(original_body, address) is None:
        return body
    original_comments = [item[1].removesuffix('\r') for item in INSTRUCTION.finditer(original_body)]
    rewritten_comments = [item[1].removesuffix('\r') for item in INSTRUCTION.finditer(body)]
    if original_comments != rewritten_comments:
        raise ValueError('Instruction comments changed before native resource coherency emission')
    start, stop = _native_resource_coherency_span(body, address)
    instructions = list(INSTRUCTION.finditer(body, start, stop))
    newline = '\r\n' if instructions[0][0].endswith('\r') else '\n'
    comments = ''.join(instruction[0] + '\n' for instruction in instructions)
    return (body[:start] + '\tsfr::synchronize_resource_memory(ctx);' + newline +
            comments + body[stop:])


def inspect_body(body, address, symbols, events, jump_tables=None):
    instructions = list(INSTRUCTION.finditer(body))
    end = address + 4 * len(instructions)
    reasons, details = [], {}
    _, jump_table_sites, invalid_jump_tables = rewrite_jump_tables(body, address, jump_tables or {})
    if jump_table_sites:
        details['jump_tables'] = len(jump_table_sites)
        details['jump_table_addresses'] = [f'0x{item:08X}' for item in jump_table_sites]
    if invalid_jump_tables:
        reasons.append('unsupported_jump_table')
        details['invalid_jump_table_addresses'] = [f'0x{item:08X}' for item in invalid_jump_tables]
    try:
        if validate_native_resource_coherency(body, address) is not None:
            details['native_resource_coherency_blocks'] = 1
    except ValueError:
        reasons.append('unsupported_native_resource_coherency')
    try:
        if validate_country_ctr(body, address) is not None:
            details['country_ctr_branches'] = 1
    except ValueError:
        reasons.append('unsupported_country_ctr')
    try:
        if validate_format_ctr(body, address) is not None:
            details['format_ctr_branches'] = 1
    except ValueError:
        reasons.append('unsupported_format_ctr')
    eqv_body, resolved_eqv, invalid_eqv = rewrite_eqv(body, address, events)
    addc_body, resolved_addc, invalid_addc = rewrite_addc(eqv_body, address, events)
    addme_body, resolved_addme, invalid_addme = rewrite_addme(addc_body, address, events)
    subfze_body, resolved_subfze, invalid_subfze = rewrite_subfze(addme_body, address, events)
    sthu_body, resolved_sthu, invalid_sthu = rewrite_sthu(subfze_body, address, events)
    stfsu_body, resolved_stfsu, invalid_stfsu = rewrite_stfsu(sthu_body, address, events)
    lhzu_body, resolved_lhzu, invalid_lhzu = rewrite_lhzu(stfsu_body, address, events)
    branch_body, resolved_bdzf, invalid_bdzf = rewrite_bdzf(lhzu_body, address, events)
    branch_body, resolved_bdnzt, invalid_bdnzt = rewrite_bdnzt(branch_body, address, events)
    if invalid_bdnzt:
        reasons.append('unsupported_bdnzt')
        details['invalid_bdnzt_addresses'] = [f'0x{item:08X}' for item in invalid_bdnzt]
    branch_body, resolved_so_branches, invalid_so_branches = rewrite_summary_overflow_branches(branch_body, address, events)
    if invalid_so_branches:
        reasons.append('unsupported_summary_overflow_branch')
        details['invalid_summary_overflow_branch_addresses'] = [f'0x{item:08X}' for item in invalid_so_branches]
    branch_body, resolved_comparisons, invalid_comparisons = rewrite_missing_comparisons(branch_body, address, events)
    if invalid_comparisons:
        reasons.append('unsupported_missing_comparison')
        details['invalid_missing_comparison_addresses'] = [f'0x{item:08X}' for item in invalid_comparisons]
    branch_body, resolved_supplemental, invalid_supplemental = rewrite_supplemental(branch_body, address, events)
    if resolved_supplemental:
        details['retained_supplemental'] = len(resolved_supplemental)
    if invalid_supplemental:
        reasons.append('unsupported_supplemental')
        details['invalid_supplemental_addresses'] = [f'0x{item:08X}' for item in invalid_supplemental]
    clock_body, clock_reads = rewrite_time_base(branch_body)
    rewritten, reservation_loads, conditional_stores = rewrite_reservations(clock_body)
    rewritten, vector_loads, vector_stores = rewrite_vector_memory(rewritten)
    rewritten, vector_word_stores = rewrite_vector_word_stores(rewritten)
    rewritten, vector_partial_loads, invalid_partial_loads = rewrite_vector_partial_loads(rewritten, address)
    if invalid_partial_loads:
        reasons.append('unsupported_vector_partial_load')
        details['invalid_vector_partial_load_addresses'] = [f'0x{item:08X}' for item in invalid_partial_loads]
    rewritten, vector_partial_stores, invalid_partial_stores = rewrite_vector_partial_stores(rewritten, address)
    if invalid_partial_stores:
        reasons.append('unsupported_vector_partial_store')
        details['invalid_vector_partial_store_addresses'] = [f'0x{item:08X}' for item in invalid_partial_stores]
    rewritten, cache_zeroes, cache_lines, invalid_cache_zeroes = rewrite_cache_zero(rewritten, address)
    if cache_zeroes:
        details['cache_block_zeroes'] = len(cache_zeroes)
        details['cache_block_zero_addresses'] = [f'0x{item:08X}' for item in cache_zeroes]
    if invalid_cache_zeroes:
        reasons.append('unsupported_cache_zero')
        details['invalid_cache_block_zero_addresses'] = [f'0x{item:08X}' for item in invalid_cache_zeroes]
    if cache_lines:
        details['cache_line_zeroes'] = len(cache_lines)
        details['cache_line_zero_addresses'] = [f'0x{item:08X}' for item in cache_lines]
    vector_instructions = sum(bool(re.match(r'(?:lv(?!sl(?:128)?\b|sr(?:128)?\b)|stv)\S*(?:\s|$)',
                                            item[1].strip())) for item in instructions)
    if vector_instructions != vector_loads + vector_stores + vector_word_stores + vector_partial_stores + vector_partial_loads:
        reasons.append('unsupported_vector_memory')
    if vector_loads:
        details['vector_loads'] = vector_loads
    if vector_stores:
        details['vector_stores'] = vector_stores
    if vector_word_stores:
        details['vector_word_stores'] = vector_word_stores
    if vector_partial_stores:
        details['vector_partial_stores'] = vector_partial_stores
    if vector_partial_loads:
        details['vector_partial_loads'] = vector_partial_loads
    clock_instructions = sum(bool(re.match(r'mftb\S*(?:\s|$)', item[1].strip()))
                             for item in instructions)
    if clock_instructions != clock_reads or re.search(r'\b__rdtscp?\b', rewritten):
        reasons.append('unsupported_time_base')
    if clock_reads:
        details['time_base_reads'] = clock_reads
    reservation_instructions = sum(bool(re.match(r'(?:l[dwbh]arx|st[dwbh]cx\.)(?:\s|$)', item[1].strip()))
                                   for item in instructions)
    if (reservation_instructions != reservation_loads + conditional_stores or
            bool(reservation_loads) != bool(conditional_stores) or
            any(bool(re.search(r'^\t// ' + load + r' ', body, re.MULTILINE)) !=
                bool(re.search(r'^\t// ' + store + r' ', body, re.MULTILINE))
                for load, store in (('lwarx', r'stwcx\.'), ('ldarx', r'stdcx\.'))) or
            re.search(r'ctx\.reserved\b|\b__sync_bool_compare_and_swap\b', rewritten)):
        reasons.append('unsupported_reservation')
    if reservation_loads:
        details['reservation_loads'] = reservation_loads
    if conditional_stores:
        details['conditional_stores'] = conditional_stores
    if resolved_eqv:
        details['retained_eqv'] = len(resolved_eqv)
        details['retained_eqv_addresses'] = [f'0x{item:08X}' for item in sorted(resolved_eqv)]
    if invalid_eqv:
        reasons.append('unsupported_eqv')
        details['invalid_eqv_addresses'] = [f'0x{item:08X}' for item in invalid_eqv]
    if resolved_addme:
        details['retained_addme'] = len(resolved_addme)
        details['retained_addme_addresses'] = [f'0x{item:08X}' for item in sorted(resolved_addme)]
    if invalid_addme:
        reasons.append('unsupported_addme')
        details['invalid_addme_addresses'] = [f'0x{item:08X}' for item in invalid_addme]
    if resolved_addc:
        details['retained_addc'] = len(resolved_addc)
        details['retained_addc_addresses'] = [f'0x{item:08X}' for item in sorted(resolved_addc)]
    if invalid_addc:
        reasons.append('unsupported_addc')
        details['invalid_addc_addresses'] = [f'0x{item:08X}' for item in invalid_addc]
    if resolved_subfze:
        details['retained_subfze'] = len(resolved_subfze)
        details['retained_subfze_addresses'] = [f'0x{item:08X}' for item in sorted(resolved_subfze)]
    if invalid_subfze:
        reasons.append('unsupported_subfze')
        details['invalid_subfze_addresses'] = [f'0x{item:08X}' for item in invalid_subfze]
    if resolved_sthu:
        details['retained_sthu'] = len(resolved_sthu)
        details['retained_sthu_addresses'] = [f'0x{item:08X}' for item in sorted(resolved_sthu)]
    if invalid_sthu:
        reasons.append('unsupported_sthu')
        details['invalid_sthu_addresses'] = [f'0x{item:08X}' for item in invalid_sthu]
    if resolved_stfsu:
        details['retained_stfsu'] = len(resolved_stfsu)
        details['retained_stfsu_addresses'] = [f'0x{item:08X}' for item in sorted(resolved_stfsu)]
    if invalid_stfsu:
        reasons.append('unsupported_stfsu')
        details['invalid_stfsu_addresses'] = [f'0x{item:08X}' for item in invalid_stfsu]
    if resolved_lhzu:
        details['retained_lhzu'] = len(resolved_lhzu)
        details['retained_lhzu_addresses'] = [f'0x{item:08X}' for item in sorted(resolved_lhzu)]
    if invalid_lhzu:
        reasons.append('unsupported_lhzu')
        details['invalid_lhzu_addresses'] = [f'0x{item:08X}' for item in invalid_lhzu]
    if resolved_bdzf:
        details['retained_bdzf'] = len(resolved_bdzf)
        details['retained_bdzf_addresses'] = [f'0x{item:08X}' for item in sorted(resolved_bdzf)]
    if invalid_bdzf:
        reasons.append('unsupported_bdzf')
        details['invalid_bdzf_addresses'] = [f'0x{item:08X}' for item in invalid_bdzf]
    # The range calculation below relies on the emitter's exact comment format.
    # Never silently omit an instruction if the emitter changes indentation.
    unexpected_comments = [comment[0] for comment in re.finditer(r'^[ \t]*//[^\r\n]*', body, re.MULTILINE)
                           if not re.match(r'^[ \t]*//\s*ERROR\b', comment[0])
                           and not comment[0].startswith('\t// ')]
    if unexpected_comments:
        reasons.append('uncertain_instruction_range')
        details['unexpected_comment_count'] = len(unexpected_comments)
    if not instructions:
        reasons.append('empty_instruction_range')
    if end > 0x100000000:
        raise ValueError('Guest instruction range overflows 32-bit address space')
    if '__builtin_debugtrap' in body:
        reasons.append('debugtrap')
    if re.search(r'//\s*ERROR\b', body):
        reasons.append('error_comment')
    if re.search(r'//\s*\.long\b', body):
        reasons.append('undecoded_instruction')
    # This exact emitter prints one single-tab instruction comment per 4 bytes.
    # Labels provide independent alignment checks; comments marked ERROR are
    # additional diagnostics and never advance the guest instruction address.
    instruction_offsets = [item.start() for item in instructions]
    for label in re.finditer(r'^loc_([0-9A-Fa-f]+):', body, re.MULTILINE):
        expected = address + 4 * bisect_left(instruction_offsets, label.start())
        if int(label[1], 16) != expected:
            reasons.append('uncertain_instruction_range')
            break
    if any(not re.match(r'(?:[A-Za-z][A-Za-z0-9_.+-]*|\.long)(?:\s|$)', item[1].strip())
           for item in instructions):
        reasons.append('uncertain_instruction_range')
    event_addresses = sorted(events)
    lo, hi = bisect_left(event_addresses, address), bisect_left(event_addresses, end)
    resolved_events = resolved_supplemental | resolved_eqv | resolved_addc | resolved_addme | resolved_subfze | resolved_sthu | resolved_stfsu | resolved_lhzu | resolved_bdzf | resolved_bdnzt | resolved_so_branches | resolved_comparisons
    unresolved_events = [item for item in event_addresses[lo:hi] if item not in resolved_events]
    if unresolved_events:
        reasons.append('logged_unsupported_instruction')
        details['unsupported_addresses'] = [f'0x{item:08X}' for item in unresolved_events]
        details['unsupported_events'] = {
            f'0x{item:08X}': [kind if opcode is None else f'{kind}:{opcode}'
                              for kind, opcode in events[item]]
            for item in unresolved_events
        }
    labels = set(re.findall(r'^\s*(loc_[0-9A-Fa-f]+):', body, re.MULTILINE))
    targets = set(re.findall(r'\bgoto\s+(' + IDENT + r')\s*;', body))
    missing_labels = sorted(targets - labels)
    if missing_labels:
        reasons.append('undefined_label')
        details['missing_labels'] = missing_labels
    # Every direct generated guest call has exactly these context arguments.
    direct = set(re.findall(r'\b(' + IDENT + r')\s*\(ctx,\s*base\)', body))
    missing = sorted(direct - symbols)
    if missing:
        reasons.append('missing_direct_symbol')
        details['missing_symbols'] = missing
    if re.search(r'\bbase\s*\+', rewritten):
        reasons.append('unchecked_guest_memory')
    return end, reasons, details


def generate(input_dir, log_path, output_dir, jump_table_path=None):
    input_dir, log_path, output_dir = map(lambda p: Path(p).resolve(),
                                         (input_dir, log_path, output_dir))
    if input_dir == output_dir or input_dir in output_dir.parents or output_dir in input_dir.parents:
        raise ValueError('Input and output directories must be separate and non-nested')
    if output_dir.exists():
        raise FileExistsError(f'Destination already exists: {output_dir}')
    headers = ['ppc_recomp_shared.h', 'ppc_context.h', 'ppc_config.h']
    for filename in headers + ['ppc_func_mapping.cpp']:
        if not (input_dir / filename).is_file():
            raise ValueError(f'Missing required generated file: {filename}')
    files = sorted(input_dir.glob('ppc_recomp.*.cpp'))
    if not files:
        raise ValueError('No generated function sources found')
    mapping_source = read(input_dir / 'ppc_func_mapping.cpp')
    mappings = MAPPING.findall(mapping_source)
    addresses = {name: int(address, 16) for address, name in mappings}
    if not mappings or len(addresses) != len(mappings):
        raise ValueError('Empty or duplicate function mapping')
    if any(value % 4 or value > 0xFFFFFFFF for value in addresses.values()):
        raise ValueError('Invalid function address in mapping')
    declarations = DECLARATION.findall(read(input_dir / headers[0]))
    symbols = set(declarations)
    if len(symbols) != len(declarations) or symbols != set(addresses):
        raise ValueError('Function mapping and declarations disagree')
    events, log_summary = parse_log(log_path)
    event_addresses = sorted(events)
    jump_tables = load_jump_tables(jump_table_path)
    emitted_jump_tables = set()
    report = {
        'format_version': 1,
        'generator_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'purpose': 'Diagnostic-only generated code; not a playable game or semantic verification.',
        'range_method': 'Mapping start plus four bytes per XenonRecomp instruction comment; '
                        'overlapping functions inspected independently; labels cross-check position.',
        'limitations': ['Requires checked scalar memory and indirect-call hooks.',
                       'mftb requires the runtime monotonic time-base hook; raw host RDTSC is not guest time.',
                       'Reservation hooks require exclusive guest execution with no intervening ordinary stores or imports.',
                       'Every retained branch label calls the runtime execution safe point, including arithmetic-only loops.',
                       'Full vector memory hooks require 16-byte checked access and reversed register bytes.',
                       'Vector word stores use checked four-byte access and the same reversed register representation.',
                       'Remaining instruction translations are not proven correct.',
                       'Unimplemented imports trap; graphics, audio, input and Kinect are not implemented.'],
        'input_files': {}, 'log': log_summary, 'rejected_functions': [], 'imports': [],
    }
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    # mkdir's normal inherited ACLs also work in restricted Windows sandboxes;
    # tempfile.mkdtemp's mode=0700 can produce inaccessible directories there.
    staging = output_dir.parent / (output_dir.name + '.staging-' + uuid.uuid4().hex)
    staging.mkdir()
    implemented, reason_counts, covered = set(), Counter(), set()
    time_base_reads = native_resource_coherency_blocks = country_ctr_branches = format_ctr_branches = 0
    reservation_loads = conditional_stores = retained_supplemental = 0
    vector_loads = vector_stores = 0
    vector_partial_stores = vector_partial_loads = cache_line_zeroes = 0
    vector_word_stores = retained_eqv = retained_addc = retained_addme = retained_subfze = retained_sthu = retained_stfsu = retained_lhzu = retained_bdzf = cache_block_zeroes = 0
    try:
        for path in files:
            source = read(path)
            report['input_files'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
            chunks, cursor = ['#include "diagnostic_hooks.h"\n'], 0
            for name, start, body_start, end, body in parse_functions(source, path.name):
                if name in implemented or name not in symbols:
                    raise ValueError(f'Duplicate or undeclared generated function: {name}')
                implemented.add(name)
                guest_end, reasons, details = inspect_body(body, addresses[name], symbols, events, jump_tables)
                covered.update(event_addresses[bisect_left(event_addresses, addresses[name]):
                                               bisect_left(event_addresses, guest_end)])
                chunks.append(source[cursor:body_start])
                if reasons:
                    reason_counts.update(reasons)
                    report['rejected_functions'].append({
                        'name': name, 'address': f'0x{addresses[name]:08X}',
                        'end_exclusive': f'0x{guest_end:08X}', 'source': path.name,
                        'reasons': reasons, **details})
                    chunks.append('\n\tsfr::unsupported_function(ctx, ' + json.dumps(name) +
                                  f', 0x{addresses[name]:08X}, ' +
                                  json.dumps('; '.join(reasons)) + ');\n}')
                else:
                    if body.count('PPC_FUNC_PROLOGUE();') != 1:
                        raise ValueError(f'Missing or duplicate function prologue: {name}')
                    time_base_reads += details.get('time_base_reads', 0)
                    reservation_loads += details.get('reservation_loads', 0)
                    conditional_stores += details.get('conditional_stores', 0)
                    vector_loads += details.get('vector_loads', 0)
                    vector_stores += details.get('vector_stores', 0)
                    vector_word_stores += details.get('vector_word_stores', 0)
                    vector_partial_stores += details.get('vector_partial_stores', 0)
                    vector_partial_loads += details.get('vector_partial_loads', 0)
                    retained_subfze += details.get('retained_subfze', 0)
                    retained_sthu += details.get('retained_sthu', 0)
                    retained_stfsu += details.get('retained_stfsu', 0)
                    retained_lhzu += details.get('retained_lhzu', 0)
                    retained_bdzf += details.get('retained_bdzf', 0)
                    cache_block_zeroes += details.get('cache_block_zeroes', 0)
                    cache_line_zeroes += details.get('cache_line_zeroes', 0)
                    retained_addc += details.get('retained_addc', 0)
                    retained_addme += details.get('retained_addme', 0)
                    retained_eqv += details.get('retained_eqv', 0)
                    native_resource_coherency_blocks += details.get('native_resource_coherency_blocks', 0)
                    country_ctr_branches += details.get('country_ctr_branches', 0)
                    format_ctr_branches += details.get('format_ctr_branches', 0)
                    rewritten_body = rewrite_format_ctr(body, addresses[name], body)
                    rewritten_body, sites, _ = rewrite_jump_tables(rewritten_body, addresses[name], jump_tables)
                    emitted_jump_tables.update(sites)
                    rewritten_body = rewrite_eqv(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_addc(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_addme(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_subfze(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_sthu(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_stfsu(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_lhzu(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_bdzf(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_bdnzt(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_summary_overflow_branches(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_missing_comparisons(rewritten_body, addresses[name], events)[0]
                    rewritten_body = rewrite_supplemental(rewritten_body, addresses[name], events)[0]
                    retained_supplemental += details.get('retained_supplemental', 0)
                    rewritten_body = rewrite_vector_memory(rewrite_reservations(rewrite_time_base(rewritten_body)[0])[0])[0]
                    rewritten_body = rewrite_vector_word_stores(rewritten_body)[0]
                    rewritten_body = rewrite_vector_partial_loads(rewritten_body, addresses[name])[0]
                    rewritten_body = rewrite_vector_partial_stores(rewritten_body, addresses[name])[0]
                    rewritten_body = rewrite_cache_zero(rewritten_body, addresses[name])[0]
                    rewritten_body = rewrite_native_resource_coherency(rewritten_body, addresses[name], body)
                    rewritten_body = rewrite_country_ctr(rewritten_body, addresses[name], body)
                    rewritten_body = re.sub(r'^(loc_[0-9A-Fa-f]+:)(\r?\n)',
                        lambda match: match[1] + match[2] + '\tsfr::guest_checkpoint();' + match[2],
                        rewritten_body, flags=re.MULTILINE)
                    chunks.append((rewritten_body + '}').replace(
                        'PPC_FUNC_PROLOGUE();', 'PPC_FUNC_PROLOGUE();\n\tsfr::enter_function(ctx, '
                        + json.dumps(name) + f', 0x{addresses[name]:08X});', 1))
                cursor = end
            chunks.append(source[cursor:])
            write(staging / path.name, ''.join(chunks))
        if vector_loads or vector_stores or vector_word_stores or vector_partial_stores:
            validate_vector_mask(read(input_dir / 'ppc_context.h'))
        if vector_partial_loads:
            validate_vector_partial_masks(read(input_dir / 'ppc_context.h'))
        missing = sorted(symbols - implemented)
        if any(not name.startswith('__imp__') for name in missing):
            raise ValueError('Missing generated non-import functions: ' + ', '.join(
                name for name in missing if not name.startswith('__imp__')))
        imports = ['#include "diagnostic_hooks.h"\n#include "ppc_recomp_shared.h"\n\n']
        for name in missing:
            # Import declarations use ordinary C++ linkage. PPC_FUNC_IMPL adds
            # extern "C" for alias targets and would conflict with those imports.
            imports.append(f'PPC_FUNC({name}) {{\n\tsfr::dispatch_import(ctx, '
                           f'{json.dumps(name)}, 0x{addresses[name]:08X});\n}}\n\n')
            report['imports'].append({'name': name, 'address': f'0x{addresses[name]:08X}'})
        write(staging / 'imports.cpp', ''.join(imports))
        for filename in headers + ['ppc_func_mapping.cpp']:
            shutil.copyfile(input_dir / filename, staging / filename)
            report['input_files'][filename] = hashlib.sha256((input_dir / filename).read_bytes()).hexdigest()
        report['log']['unmapped_unsupported_addresses'] = [f'0x{item:08X}' for item in sorted(set(events) - covered)]
        if report['log']['unmapped_unsupported_addresses']:
            raise ValueError('Unmapped unsupported log addresses: ' + ', '.join(
                report['log']['unmapped_unsupported_addresses']))
        report['counts'] = {'functions': len(implemented), 'imports': len(missing),
                            'retained_functions': len(implemented) - len(report['rejected_functions']),
                            'rejected_functions': len(report['rejected_functions']),
                            'time_base_reads': time_base_reads,
                            'reservation_loads': reservation_loads,
                            'conditional_stores': conditional_stores,
                            'vector_loads': vector_loads,
                            'vector_stores': vector_stores,
                            'vector_word_stores': vector_word_stores,
                            'vector_partial_stores': vector_partial_stores,
                            'vector_partial_loads': vector_partial_loads,
                            'retained_subfze': retained_subfze,
                            'retained_sthu': retained_sthu,
                            'retained_eqv': retained_eqv,
                            'retained_stfsu': retained_stfsu,
                            'retained_lhzu': retained_lhzu,
                            'retained_bdzf': retained_bdzf,
                            'cache_block_zeroes': cache_block_zeroes,
                            'cache_line_zeroes': cache_line_zeroes,
                            'retained_addc': retained_addc,
                            'retained_addme': retained_addme,
                            'native_resource_coherency_blocks': native_resource_coherency_blocks,
                            'country_ctr_branches': country_ctr_branches,
                            'format_ctr_branches': format_ctr_branches,
                            'jump_tables': len(jump_tables),
                            'retained_jump_tables': len(emitted_jump_tables),
                            'retained_supplemental': retained_supplemental,
                            'rejection_reasons': dict(sorted(reason_counts.items()))}
        write(staging / 'report.json', json.dumps(report, indent=2) + '\n')
        if output_dir.exists():
            raise FileExistsError(f'Destination appeared during generation: {output_dir}')
        # Windows can briefly deny a directory rename immediately after its
        # files close. Observed WinError 5 failures recovered after 50 ms.
        # Keep publication atomic and bounded; never retry over a new target.
        for attempt in range(5):
            try:
                staging.rename(output_dir)
                break
            except PermissionError as error:
                if (sys.platform != 'win32' or getattr(error, 'winerror', None) not in (5, 32, 33)
                        or attempt == 4 or output_dir.exists()):
                    raise
                time.sleep(0.05)
    except BaseException:
        shutil.rmtree(staging)
        raise
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--switches', type=Path, help='switches.toml used for the recompilation')
    args = parser.parse_args()
    report = generate(args.input, args.log, args.output, args.switches)
    print(json.dumps(report['counts'], indent=2))


if __name__ == '__main__':
    main()
