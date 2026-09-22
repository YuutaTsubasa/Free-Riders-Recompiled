import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import shutil
import uuid
from contextlib import contextmanager
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
try:
    import prepare_shaders as shaders
except ImportError:
    shaders = None


def container():
    data = bytearray(132)
    struct.pack_into('>9I', data, 0, 0x102a1101, 108, 24, 0, 72, 0, 36, 0, 0)
    struct.pack_into('>6I', data, 36, 0, 24, 0, 0, 0, 0)
    struct.pack_into('>8I', data, 72, 36, 28, 28, 0, 0, 0, 0, 0)
    # One EXEC_END clause at instruction 1, count 1; the other CF slot is NOP.
    value = (2 << 44) | (1 << 12) | 1
    struct.pack_into('>3I', data, 108, value & 0xffffffff, value >> 32, 0)
    return bytes(data)


def dxil(kind=1):
    program = struct.pack('<HHI4sIII', 0x60, kind, 7, b'DXIL', 0x100, 16, 4) + b'BC\xc0\xde'
    return b'DXBC' + bytes(16) + struct.pack('<III', 1, 44 + len(program), 1) + struct.pack('<I', 36) + b'DXIL' + struct.pack('<I', len(program)) + program


SYNTHETIC_IMAGE_BASE = 0x82000000
SYNTHETIC_IMAGE_SIZE = 0x800
SYNTHETIC_POINTER_OFFSET = 0x700


def tagged_container(tag, stage='vertex'):
    data = bytearray(container())
    struct.pack_into('>I', data, 0, 0x102a1101 if stage == 'vertex' else 0x102a1100)
    # Distinct instruction payloads keep all identities unique without ROM bytes.
    data[-1] = tag
    return bytes(data)


def synthetic_embedded_image():
    data = bytearray(SYNTHETIC_IMAGE_SIZE)
    records = []
    for index in range(6):
        stage = 'vertex' if index < 3 else 'pixel'
        blob = tagged_container(128 + index, stage)
        offset = index * 0x100
        data[offset:offset + len(blob)] = blob
        records.append(dict(guest_address=SYNTHETIC_IMAGE_BASE + offset,
                            size=len(blob), stage=stage,
                            source_sha256=hashlib.sha256(blob).hexdigest()))
    # An adjacent valid-looking container must never enter the fixed allowlist.
    data[0x600:0x600 + 132] = tagged_container(200)
    struct.pack_into('>6I', data, SYNTHETIC_POINTER_OFFSET,
                     *(record['guest_address'] for record in records))
    return bytes(data), tuple(records)


@contextmanager
def embedded_policy(data, records, digest=None):
    with patch.multiple(shaders, create=True, IMAGE_BASE=SYNTHETIC_IMAGE_BASE,
                        IMAGE_SIZE=SYNTHETIC_IMAGE_SIZE,
                        IMAGE_SHA256=digest or hashlib.sha256(data).hexdigest(),
                        EMBEDDED_POINTER_TABLE=SYNTHETIC_IMAGE_BASE + SYNTHETIC_POINTER_OFFSET,
                        EMBEDDED_SHADERS=records):
        yield


def synthetic_basic_asset():
    return b''.join(tagged_container(index, 'vertex' if index % 2 == 0 else 'pixel')
                    for index in range(68))


def fake_shader_tools(translator, seen, fail_compile=None):
    compiled = 0
    def run(args, **kwargs):
        nonlocal compiled
        args = [str(arg) for arg in args]
        if '--version' in args:
            return subprocess.CompletedProcess(args, 0, b'test compiler', b'')
        if args[0] == str(translator):
            original = Path(args[1]).read_bytes()
            stage = 'vertex' if struct.unpack_from('>I', original)[0] & 1 else 'pixel'
            mask = 0 if stage == 'vertex' else 7
            seen.append(original)
            Path(args[2]).write_bytes(b'test HLSL')
            return subprocess.CompletedProcess(args, 0,
                f'{stage} HLSL bytes=9 spec_constants_mask={mask}\n'.encode(), b'')
        compiled += 1
        if compiled == fail_compile:
            return subprocess.CompletedProcess(args, 1, b'', b'late embedded compile rejected')
        profile = args[args.index('-T') + 1]
        kind = 6 if profile == 'lib_6_3' else (1 if profile == 'vs_6_0' else 0)
        Path(args[args.index('-Fo') + 1]).write_bytes(dxil(kind))
        return subprocess.CompletedProcess(args, 0, b'', b'')
    return run


@contextmanager
def working_directory():
    folder = Path(__file__).resolve().parents[1] / 'out/tests' / ('shader-' + uuid.uuid4().hex)
    folder.mkdir(parents=True)
    try:
        yield folder
    finally:
        shutil.rmtree(folder)


class ShaderPreparationTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(shaders, 'production shader preparation is missing')

    def test_valid_container_has_exact_extent_and_stage(self):
        records = shaders.scan_containers(container())
        self.assertEqual([(r['offset'], r['size'], r['stage']) for r in records], [(0, 132, 'vertex')])

    def test_rejects_truncated_container(self):
        with self.assertRaisesRegex(ValueError, 'extent'):
            shaders.scan_containers(container()[:-1])

    def test_rejects_unterminated_constant_name(self):
        data = bytearray(container())
        data[104:108] = b'abcd'
        with self.assertRaisesRegex(ValueError, 'string'):
            shaders.scan_containers(data)

    def test_rejects_instruction_clause_outside_program(self):
        data = bytearray(container())
        struct.pack_into('>I', data, 108, 0x1002)
        with self.assertRaisesRegex(ValueError, 'exec clause extent'):
            shaders.scan_containers(data)

    def test_rejects_unknown_asset_even_if_structurally_valid(self):
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            shaders.validate_asset(container())

    def test_rejects_wrong_compiled_stage(self):
        with self.assertRaisesRegex(ValueError, 'stage'):
            shaders.validate_dxil(dxil(0), 'vertex', 0)
        shaders.validate_dxil(dxil(), 'vertex', 0)
        shaders.validate_dxil(dxil(6), 'pixel', 7)

    def test_rejects_truncated_dxbc_and_bad_bitcode_extent(self):
        for data in (dxil()[:-1], b'DXBC', dxil().replace(b'BC\xc0\xde', b'nope')):
            with self.subTest(data=data), self.assertRaises(ValueError):
                shaders.validate_dxil(data, 'vertex', 0)

    def test_profile_preserves_pixel_specialization(self):
        self.assertEqual(shaders.compiler_profile('vertex', 0), ['-T', 'vs_6_0', '-E', 'shaderMain'])
        self.assertEqual(shaders.compiler_profile('pixel', 7), ['-T', 'lib_6_3'])

    def test_accepts_powershell_utf8_provenance(self):
        with working_directory() as folder:
            translator = folder / 'translator.exe'
            translator.write_bytes(b'test executable')
            record = {'commit': shaders.TOOL_PIN, 'sha256': shaders.sha(translator.read_bytes()), 'sources': {name: shaders.sha((shaders.ROOT / name).read_bytes()) for name in shaders.TRANSLATOR_SOURCES}}
            translator.with_suffix('.json').write_text(json.dumps(record), encoding='utf-8-sig')
            with patch.object(shaders.subprocess, 'check_output', return_value=shaders.TOOL_PIN), patch.object(shaders.subprocess, 'check_call', return_value=0):
                self.assertEqual(shaders.verify_translator(translator), record)

    def test_rejects_provenance_missing_upstream_sources(self):
        with working_directory() as folder:
            translator = folder / 'translator.exe'; translator.write_bytes(b'test')
            record = {'commit': shaders.TOOL_PIN, 'sha256': shaders.sha(b'test'), 'sources': {}}
            translator.with_suffix('.json').write_text(json.dumps(record))
            with patch.object(shaders.subprocess, 'check_output', return_value=shaders.TOOL_PIN):
                with self.assertRaisesRegex(ValueError, 'source inventory'):
                    shaders.verify_translator(translator)

    def test_cache_publication_failure_restores_previous_manifest(self):
        with working_directory() as folder:
            cache = folder / 'shader_cache_data.cpp'; cache.write_text('old cache')
            manifest = folder / 'manifest.json'; manifest.write_text('old manifest')
            pending = folder / 'pending.cpp'; pending.write_text('new cache')
            real_replace = shaders.os.replace
            def replace(source, destination):
                if Path(destination) == cache:
                    raise OSError('publication blocked')
                return real_replace(source, destination)
            with patch.object(shaders.os, 'replace', side_effect=replace):
                with self.assertRaisesRegex(OSError, 'publication blocked'):
                    shaders.publish_cache(pending, b'new manifest', folder)
            self.assertEqual(cache.read_text(), 'old cache')
            self.assertEqual(manifest.read_text(), 'old manifest')

    def test_generated_cache_identifies_immutable_generation_manifest(self):
        self.assertIn('generation-unit/manifest.json', shaders.generate_cpp([], Path('generation-unit')))

    def test_failed_compile_preserves_old_cache_and_does_not_reuse_old_dxil(self):
        with working_directory() as folder:
            root = Path(folder)
            asset = root / 'asset'; asset.write_bytes(container())
            output = root / 'cache'; output.mkdir()
            cache = output / 'shader_cache_data.cpp'; cache.write_text('previous valid cache')
            (output / 'shader.dxil').write_bytes(dxil())
            records = shaders.scan_containers(container())
            def run(args, **kwargs):
                if '--version' in args:
                    return subprocess.CompletedProcess(args, 0, b'test compiler', b'')
                if str(args[0]) == 'translator':
                    Path(args[2]).write_text('test HLSL')
                    return subprocess.CompletedProcess(args, 0, b'vertex HLSL bytes=9 spec_constants_mask=0\n', b'')
                return subprocess.CompletedProcess(args, 1, b'', b'compile rejected')
            with patch.object(shaders, 'validate_asset', return_value=records), patch.object(shaders, 'verify_translator', return_value={'commit': 'test'}), patch.object(shaders.subprocess, 'run', side_effect=run):
                with self.assertRaisesRegex(RuntimeError, 'exit 1'):
                    shaders.prepare(asset, Path('translator'), Path('dxc'), output)
            self.assertEqual(cache.read_text(), 'previous valid cache')
            self.assertFalse((output / 'manifest.json').exists())

    def test_embedded_allowlist_matches_the_six_audited_containers(self):
        expected = [
            (0x820D2600, 296, 'vertex', 'f563f13062eb4ae4a414f6509b5dcf4abb3837fed62b65eb220898734ac388d1'),
            (0x820D2728, 284, 'vertex', '8942ad3cc544af5b4ff186f762010e24d69340ec30a4d9fe0780304d3d028474'),
            (0x820D2848, 320, 'vertex', '86c79b8000206e479be9a5b964f0c6c9c6e03889297ee7894e78ad3a868cfac1'),
            (0x820D2988, 160, 'pixel', 'f098278a3b50fbeec562599620580322b5fa223c389ca2c222215a13399f5247'),
            (0x820D2A28, 232, 'pixel', '2302efea6ffd5c9e20ebebe71af9ad9617cb9483fb1d919b9296248e5fc8dd28'),
            (0x820D2B10, 232, 'pixel', 'd6c0aeb178913e74870fc3dfb7ba209536c6a62400d2204a198d015198b76ea0'),
        ]
        self.assertEqual(shaders.IMAGE_BASE, 0x82000000)
        self.assertEqual(shaders.IMAGE_SIZE, 34209792)
        self.assertEqual(shaders.IMAGE_SHA256, 'd092c5397d769594d7b46cda023cf2c62aed9df4e76db44efee8136daf00ad99')
        self.assertEqual(shaders.EMBEDDED_POINTER_TABLE, 0x82AF6D30)
        self.assertIsInstance(shaders.EMBEDDED_SHADERS, tuple)
        self.assertEqual([(r['guest_address'], r['size'], r['stage'], r['source_sha256'])
                          for r in shaders.EMBEDDED_SHADERS], expected)

    def test_embedded_image_returns_only_six_exact_records_with_image_offsets(self):
        data, policy = synthetic_embedded_image()
        snapshot = tuple(dict(record) for record in policy)
        with embedded_policy(data, policy):
            records = shaders.validate_embedded_image(data)
        self.assertEqual(records, [dict(record, offset=record['guest_address'] - SYNTHETIC_IMAGE_BASE,
                                       source_kind='decoded_image') for record in policy])
        self.assertEqual(policy, snapshot, 'validation must not mutate the allowlist')
        self.assertEqual(len(records), 6, 'adjacent shader-looking bytes are excluded')

    def test_embedded_image_rejects_full_image_and_pointer_table_changes(self):
        data, policy = synthetic_embedded_image()
        changed = bytearray(data)
        changed[0x7ff] ^= 1  # Outside every shader and the pointer table.
        cases = [('image hash', bytes(changed), hashlib.sha256(data).hexdigest()),
                 ('truncated image', data[:-1], hashlib.sha256(data[:-1]).hexdigest()),
                 ('oversized image', data + b'\0', hashlib.sha256(data + b'\0').hexdigest())]
        for name, sample, digest in cases:
            with self.subTest(name=name), embedded_policy(sample, policy, digest):
                with self.assertRaises(ValueError):
                    shaders.validate_embedded_image(sample)
        for index in range(6):
            changed = bytearray(data)
            struct.pack_into('>I', changed, SYNTHETIC_POINTER_OFFSET + index * 4,
                             policy[(index + 1) % 6]['guest_address'])
            # Re-pin the synthetic whole image so the pointer check must fail itself.
            with self.subTest(pointer=index), embedded_policy(changed, policy):
                with self.assertRaises(ValueError):
                    shaders.validate_embedded_image(changed)

    def test_embedded_image_checks_slice_digest_stage_and_container_extent(self):
        data, original_policy = synthetic_embedded_image()
        for name in ('digest', 'stage', 'extent', 'short slice', 'long slice'):
            changed = bytearray(data)
            policy = [dict(record) for record in original_policy]
            offset = policy[-1]['guest_address'] - SYNTHETIC_IMAGE_BASE
            if name == 'digest':
                changed[offset + 131] ^= 1
            elif name == 'stage':
                struct.pack_into('>I', changed, offset, 0x102a1101)
            elif name == 'extent':
                struct.pack_into('>I', changed, offset + 4, 112)
            else:
                policy[-1]['size'] += -1 if name == 'short slice' else 4
            if name != 'digest':
                policy[-1]['source_sha256'] = hashlib.sha256(
                    changed[offset:offset + policy[-1]['size']]).hexdigest()
            with self.subTest(name=name), embedded_policy(changed, tuple(policy)):
                with self.assertRaises(ValueError):
                    shaders.validate_embedded_image(changed)

    def test_embedded_image_rejects_bad_allowlist_ranges_count_and_duplicates(self):
        data, original_policy = synthetic_embedded_image()
        for name in ('underflow', 'overflow', 'duplicate', 'count'):
            changed = bytearray(data)
            policy = [dict(record) for record in original_policy]
            if name == 'underflow':
                policy[-1]['guest_address'] = SYNTHETIC_IMAGE_BASE - 4
            elif name == 'overflow':
                policy[-1]['guest_address'] = SYNTHETIC_IMAGE_BASE + SYNTHETIC_IMAGE_SIZE - 4
            elif name == 'duplicate':
                policy[-1] = dict(policy[0])
            else:
                policy.pop()
            for index, record in enumerate(policy):
                struct.pack_into('>I', changed, SYNTHETIC_POINTER_OFFSET + index * 4,
                                 record['guest_address'])
            with self.subTest(name=name), embedded_policy(changed, tuple(policy)):
                with self.assertRaises(ValueError):
                    shaders.validate_embedded_image(changed)

    def test_invalid_embedded_source_fails_before_tools_or_output_creation(self):
        data, original_policy = synthetic_embedded_image()
        basic = synthetic_basic_asset()
        with working_directory() as folder:
            asset = folder / 'asset'; asset.write_bytes(basic)
            image = folder / 'image.bin'
            for name in ('image hash', 'last slice digest', 'last pointer', 'last stage'):
                changed = bytearray(data)
                policy = [dict(record) for record in original_policy]
                offset = policy[-1]['guest_address'] - SYNTHETIC_IMAGE_BASE
                if name == 'image hash':
                    changed[-1] ^= 1
                elif name == 'last slice digest':
                    changed[offset + 131] ^= 1
                elif name == 'last pointer':
                    struct.pack_into('>I', changed, SYNTHETIC_POINTER_OFFSET + 20, policy[0]['guest_address'])
                else:
                    struct.pack_into('>I', changed, offset, 0x102a1101)
                    policy[-1]['source_sha256'] = hashlib.sha256(changed[offset:offset + 132]).hexdigest()
                image.write_bytes(changed)
                digest = hashlib.sha256(data if name == 'image hash' else changed).hexdigest()
                output = folder / name.replace(' ', '-')
                with self.subTest(name=name), embedded_policy(changed, tuple(policy), digest), \
                     patch.object(shaders, 'ASSET_SHA256', hashlib.sha256(basic).hexdigest()), \
                     patch.object(shaders, 'verify_translator') as verify, \
                     patch.object(shaders.subprocess, 'run') as run:
                    with self.assertRaises(ValueError):
                        shaders.prepare(asset, Path('translator'), Path('dxc'), output, image=image)
                    verify.assert_not_called()
                    run.assert_not_called()
                    self.assertFalse(output.exists())

    def test_combined_preparation_preserves_all_74_sources_and_per_item_provenance(self):
        data, policy = synthetic_embedded_image()
        basic = synthetic_basic_asset()
        with working_directory() as folder:
            asset = folder / 'asset'; asset.write_bytes(basic)
            image = folder / 'image.bin'; image.write_bytes(data)
            translator = folder / 'translator'; translator.write_bytes(b'test translator')
            compiler = folder / 'dxc'; compiler.write_bytes(b'test compiler executable')
            output = folder / 'cache'
            seen = []
            with embedded_policy(data, policy), \
                 patch.object(shaders, 'ASSET_SHA256', hashlib.sha256(basic).hexdigest()), \
                 patch.object(shaders, 'verify_translator', return_value={'commit': 'test'}), \
                 patch.object(shaders.subprocess, 'run', side_effect=fake_shader_tools(translator, seen)):
                manifest = shaders.prepare(asset, translator, compiler, output, image=image)
            self.assertEqual(manifest['schema_version'], 2)
            self.assertEqual(manifest['asset_sha256'], hashlib.sha256(basic).hexdigest())
            sources = {record['kind']: record for record in manifest['sources']}
            self.assertEqual(len(manifest['sources']), 2)
            self.assertEqual(set(sources), {'basic_asset', 'decoded_image'})
            for kind, blob in [('basic_asset', basic), ('decoded_image', data)]:
                self.assertEqual(sources[kind]['size'], len(blob))
                self.assertEqual(sources[kind]['sha256'], hashlib.sha256(blob).hexdigest())
            self.assertEqual(sources['decoded_image']['image_base'], SYNTHETIC_IMAGE_BASE)
            expected = [dict(record, source_kind='basic_asset') for record in shaders.scan_containers(basic)]
            expected += [dict(record, source_kind='decoded_image', offset=record['guest_address'] - SYNTHETIC_IMAGE_BASE)
                         for record in policy]
            self.assertEqual(len(manifest['records']), 74)
            self.assertEqual(len(seen), 74)
            self.assertEqual(len({record['directory'] for record in manifest['records']}), 74)
            generation = output / manifest['generation']
            for record, wanted, source_bytes in zip(manifest['records'], expected, seen):
                for key, value in wanted.items():
                    self.assertEqual(record[key], value)
                source = basic if record['source_kind'] == 'basic_asset' else data
                original = source[record['offset']:record['offset'] + record['size']]
                self.assertEqual(source_bytes, original)
                item = generation / record['directory']
                self.assertEqual((item / 'original.bin').read_bytes(), original)
                for field, filename in [('source_sha256', 'original.bin'), ('hlsl_sha256', 'shader.hlsl'),
                                        ('dxil_sha256', 'shader.dxil')]:
                    self.assertEqual(record[field], hashlib.sha256((item / filename).read_bytes()).hexdigest())
                self.assertEqual(record['dxil_bytes'], (item / 'shader.dxil').stat().st_size)
                self.assertEqual(record['specialization_mask'], 0 if record['stage'] == 'vertex' else 7)
                self.assertIn('translator_args', record)
                self.assertIn('compiler_args', record)
            self.assertEqual(json.loads((output / 'manifest.json').read_text()), manifest)
            self.assertEqual(json.loads((generation / 'manifest.json').read_text()), manifest)
            self.assertEqual(manifest['cache_cpp_sha256'], hashlib.sha256((output / 'shader_cache_data.cpp').read_bytes()).hexdigest())

    def test_duplicate_identity_across_basic_and_image_fails_before_tools(self):
        data, original_policy = synthetic_embedded_image()
        basic = synthetic_basic_asset()
        changed = bytearray(data)
        changed[:132] = basic[:132]
        policy = [dict(record) for record in original_policy]
        policy[0]['source_sha256'] = hashlib.sha256(basic[:132]).hexdigest()
        with working_directory() as folder:
            asset = folder / 'asset'; asset.write_bytes(basic)
            image = folder / 'image.bin'; image.write_bytes(changed)
            output = folder / 'cache'
            with embedded_policy(changed, tuple(policy)), \
                 patch.object(shaders, 'ASSET_SHA256', hashlib.sha256(basic).hexdigest()), \
                 patch.object(shaders, 'verify_translator') as verify, \
                 patch.object(shaders.subprocess, 'run') as run:
                # Both inputs are independently valid: only their union duplicates.
                self.assertEqual(len(shaders.validate_asset(basic)), 68)
                self.assertEqual(len(shaders.validate_embedded_image(changed)), 6)
                with self.assertRaises(ValueError):
                    shaders.prepare(asset, Path('translator'), Path('dxc'), output, image=image)
                verify.assert_not_called()
                run.assert_not_called()
                self.assertFalse(output.exists())

    def test_last_embedded_compile_failure_preserves_both_old_publications(self):
        data, policy = synthetic_embedded_image()
        basic = synthetic_basic_asset()
        with working_directory() as folder:
            asset = folder / 'asset'; asset.write_bytes(basic)
            image = folder / 'image.bin'; image.write_bytes(data)
            translator = folder / 'translator'; translator.write_bytes(b'test translator')
            compiler = folder / 'dxc'; compiler.write_bytes(b'test compiler executable')
            output = folder / 'cache'; output.mkdir()
            cache = output / 'shader_cache_data.cpp'; cache.write_bytes(b'previous cache')
            manifest = output / 'manifest.json'; manifest.write_bytes(b'previous manifest')
            old_generation = output / 'generation-old'; old_generation.mkdir()
            (old_generation / 'shader.dxil').write_bytes(dxil())
            seen = []
            with embedded_policy(data, policy), \
                 patch.object(shaders, 'ASSET_SHA256', hashlib.sha256(basic).hexdigest()), \
                 patch.object(shaders, 'verify_translator', return_value={'commit': 'test'}), \
                 patch.object(shaders.subprocess, 'run', side_effect=fake_shader_tools(translator, seen, fail_compile=74)):
                with self.assertRaisesRegex(RuntimeError, 'late embedded compile rejected'):
                    shaders.prepare(asset, translator, compiler, output, image=image)
            self.assertEqual(len(seen), 74, 'failure occurs only after compiling all basic and five embedded shaders')
            self.assertEqual(cache.read_bytes(), b'previous cache')
            self.assertEqual(manifest.read_bytes(), b'previous manifest')
            self.assertEqual((old_generation / 'shader.dxil').read_bytes(), dxil())

    def test_four_argument_preparation_remains_basic_only_with_schema_two(self):
        basic = synthetic_basic_asset()
        with working_directory() as folder:
            asset = folder / 'asset'; asset.write_bytes(basic)
            translator = folder / 'translator'; translator.write_bytes(b'test translator')
            compiler = folder / 'dxc'; compiler.write_bytes(b'test compiler executable')
            output = folder / 'cache'
            seen = []
            with patch.object(shaders, 'ASSET_SHA256', hashlib.sha256(basic).hexdigest()), \
                 patch.object(shaders, 'validate_embedded_image', create=True) as validate_image, \
                 patch.object(shaders, 'verify_translator', return_value={'commit': 'test'}), \
                 patch.object(shaders.subprocess, 'run', side_effect=fake_shader_tools(translator, seen)):
                manifest = shaders.prepare(asset, translator, compiler, output)
            validate_image.assert_not_called()
            self.assertEqual(manifest['schema_version'], 2)
            self.assertEqual(len(manifest['records']), 68)
            self.assertEqual(len(seen), 68)
            self.assertTrue(all(record['source_kind'] == 'basic_asset' for record in manifest['records']))
            self.assertEqual(len(manifest['sources']), 1)
            self.assertEqual(manifest['sources'][0]['kind'], 'basic_asset')


if __name__ == '__main__':
    unittest.main()
