from pathlib import Path
import sys
import tempfile
import os
import unittest
import zipfile
import json

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import anonymize_benchmark as anon

LOG = (
    'NATIVE_USER_LANGUAGE source=GetUserDefaultUILanguage windows_langid=0x404 xbox_language=8\n'
    'NATIVE_USER_COUNTRY source=GetUserDefaultGeoName iso=TW xbox_country=101\n'
    'RESULT NtCreateFile path=sav:\\SfrAllData.sav host=C:\\Users\\Alice Smith\\Documents\\game\\out\\bench\\x'
    '\\save-warmup-1\\E000C56DDFFD8EA0\\00000001\\SfrA\n'
    'RESULT host=c:/users/bob/AppData/x /home/carol/game MYPC-01 started\n'
    'NATIVE_PRESENT frame=0 draws=800 racing=1 presented=1 seconds=1.000\n'
)
INFO = ('commit=3872cb4\ngenerated=C:/Users/Alice/Documents/game/out/recomp/diagnostic-local\n'
        'cpu=Intel(R) Core(TM) i7-6850K CPU @ 3.60GHz\ngpu=NVIDIA GeForce RTX 3080 Ti\n')


class AnonymizeTest(unittest.TestCase):
    def test_json_escaped_username_is_removed(self):
        data = json.dumps({'save': r'C:\Users\Alice\save'}).encode()
        self.assertNotIn(b'Alice', anon.scrub(data, anon.patterns([]))[0])

    def test_existing_destination_is_not_deleted(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'run'; source.mkdir()
            output = Path(directory) / 'run-shareable'; output.mkdir()
            (output / 'keep').write_text('keep')
            with self.assertRaises(SystemExit):
                anon.main(['anon', str(source)])
            self.assertEqual((output / 'keep').read_text(), 'keep')

    def test_save_and_cache_are_excluded(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'run'; source.mkdir()
            for folder in ('save-baseline-1', 'fixture', 'cache'):
                (source / folder).mkdir()
                (source / folder / 'private.json').write_text('private')
            (source / 'baseline-1.log').write_text(LOG)
            output = Path(directory) / 'clean'
            anon.anonymize(source, output)
            self.assertEqual([p.name for p in output.iterdir()], ['baseline-1.log'])

    def run_it(self, **options):
        root = Path(tempfile.mkdtemp())
        source = root / 'run'
        source.mkdir()
        (source / 'a.log').write_text(LOG, encoding='utf-8')
        (source / 'info.txt').write_text(INFO, encoding='utf-8')
        (source / 'shot.bmp').write_bytes(b'BM')
        (source / 'tool.exe').write_bytes(b'Alice stays in a binary')
        destination = root / 'out'
        result = anon.anonymize(source, destination, **options)
        return destination, result

    def test_names_ids_and_locale_are_removed_and_the_timings_stay(self):
        destination, (copied, replaced, left_out) = self.run_it(also=['MYPC-01'])
        log = (destination / 'a.log').read_text(encoding='utf-8')
        info = (destination / 'info.txt').read_text(encoding='utf-8')
        for secret in ('Alice', 'bob', 'carol', 'MYPC-01', 'E000C56DDFFD8EA0', 'iso=TW', '0x404'):
            self.assertNotIn(secret, log + info)
        self.assertIn('C:\\Users\\USER\\Documents\\game', log)
        self.assertIn('/home/USER/game', log)
        self.assertIn('seconds=1.000', log)
        self.assertIn('presented=1', log)
        self.assertIn('generated=<removed>', info)
        self.assertIn('i7-6850K', info)
        self.assertIn('RTX 3080 Ti', info)
        self.assertIn('commit=3872cb4', info)
        self.assertEqual((copied, left_out), (2, 2))
        self.assertGreater(replaced, 6)

    def test_screenshots_stay_only_when_asked_for(self):
        destination, (_, _, left_out) = self.run_it(keep_screenshots=True)
        self.assertTrue((destination / 'shot.bmp').exists())
        self.assertEqual(left_out, 1)
        destination, _ = self.run_it()
        self.assertFalse((destination / 'shot.bmp').exists())

    def test_the_map_and_the_profile_function_table_are_withheld(self):
        # They name the generated code's functions; the summary and the logs still go.
        root = Path(tempfile.mkdtemp())
        source = root / 'run'
        source.mkdir()
        (source / 'sfr_cpu_diagnostic.map').write_text(' 0001:00000000 sub_82000000 0000000140001000 f ppc_recomp.0.obj\n', encoding='utf-8')
        (source / 'profile.md').write_text('| sub_82000000 | 12% |\n', encoding='utf-8')
        (source / 'summary.md').write_text('| baseline | 27.3 |\n', encoding='utf-8')
        (source / 'profile-1.log').write_text('HOST_PROFILE rva=0x1000 42\n', encoding='utf-8')
        copied, _, left_out = anon.anonymize(source, root / 'out')
        self.assertEqual(sorted(p.name for p in (root / 'out').iterdir()), ['profile-1.log', 'summary.md'])
        self.assertEqual((copied, left_out), (2, 2))

    def test_unknown_binary_files_are_excluded(self):
        destination, _ = self.run_it()
        self.assertFalse((destination / 'tool.exe').exists())

    def test_the_hardware_probe_lines_carry_nothing_to_remove_and_stay_whole(self):
        # scripts/hardware_probe.ps1 records no name, serial, address or path:
        # every line of its block comes through as it is, and a computer name
        # given with --also still goes.
        probe = ('cpu_cores=24 cores, 32 threads, max 3200 MHz (hybrid: about 8 P-cores and 16 E-cores, read from the counts)\n'
                 'cpu_features=avx=yes avx2=yes avx512f=no (the game needs AVX; "no" on Windows before 10 2004 says nothing)\n'
                 'memory=32 GB, 2 modules at 6000 MT/s\n'
                 'gpu0=NVIDIA GeForce RTX 4090 driver=32.0.15.6094 (2024-05-12) vram_mb=24564 display=3840x2160@120\n'
                 'chassis=desktop (type 3)\npower_mode=mains=best performance\ngpu_scheduling=on\ngame_mode=default (on)\n'
                 'vbs=running memory_integrity=on\nos_build=Windows 11 Pro 24H2 build 26100.2894\n'
                 'd3d12_runtime=system d3d12.dll 10.0.26100.1, Agility SDK beside the program 1.619.6\n'
                 'vulkan_loader=vulkan-1.dll 1.3.290.0, drivers: nv-vk64.json\n')
        data, count = anon.scrub(probe.encode(), anon.patterns([]))
        self.assertEqual((data, count), (probe.encode(), 0))
        data, _ = anon.scrub((probe + 'model=DESKTOP-ABC123 custom build\n').encode(), anon.patterns(['DESKTOP-ABC123']))
        self.assertNotIn(b'DESKTOP-ABC123', data)


class ArchiveLimitTest(unittest.TestCase):
    def folder(self, files):
        root = Path(tempfile.mkdtemp())
        folder = root / 'run-shareable'
        folder.mkdir()
        for name, data in files.items():
            (folder / name).write_bytes(data)
        return folder

    def test_a_result_that_fits_is_one_plain_zip(self):
        folder = self.folder({'a.log': b'x' * 1000, 'b.log': b'y' * 1000})
        names = anon.make_archives(folder, 1_000_000)
        self.assertEqual([Path(n).name for n in names], ['run-shareable.zip'])

    def test_a_result_that_does_not_fit_is_split_into_whole_file_parts_under_the_limit(self):
        # incompressible, so the zip is as big as the data
        files = {f'run-{i}.log': os.urandom(40_000) for i in range(6)}
        folder = self.folder(files)
        limit = 100_000
        names = anon.make_archives(folder, limit)
        self.assertGreater(len(names), 1)
        self.assertEqual([Path(n).name for n in names][0], f'run-shareable-part1of{len(names)}.zip')
        seen = {}
        for name in names:
            self.assertLessEqual(Path(name).stat().st_size, limit)
            with zipfile.ZipFile(name) as archive:
                for member in archive.namelist():
                    self.assertNotIn(member, seen)
                    seen[member] = archive.read(member)
        self.assertEqual(seen, files)

    def test_a_file_over_the_limit_alone_goes_in_pieces_that_join_back(self):
        data = os.urandom(150_000)
        folder = self.folder({'big.log': data, 'small.log': b'z' * 100})
        limit = 60_000
        names = anon.make_archives(folder, limit)
        pieces = {}
        for name in names:
            self.assertLessEqual(Path(name).stat().st_size, limit)
            with zipfile.ZipFile(name) as archive:
                for member in archive.namelist():
                    pieces[member] = archive.read(member)
        joined = b''.join(pieces[k] for k in sorted(pieces) if k.startswith('big.log.'))
        self.assertEqual(joined, data)
        self.assertEqual(pieces['small.log'], b'z' * 100)
        self.assertNotIn('big.log', pieces)

    def test_an_older_zip_of_the_same_run_is_preserved(self):
        folder = self.folder({'a.log': b'x'})
        stale = folder.parent / 'run-shareable-part9of9.zip'
        stale.write_bytes(b'old')
        with self.assertRaises(SystemExit):
            anon.make_archives(folder, 1_000_000)
        self.assertEqual(stale.read_bytes(), b'old')

    def test_unrelated_prefix_archive_is_preserved(self):
        folder = self.folder({'a.log': b'x'})
        report = folder.parent / 'run-shareable-report.zip'
        report.write_bytes(b'report')
        anon.make_archives(folder, 1_000_000)
        self.assertEqual(report.read_bytes(), b'report')


if __name__ == '__main__':
    unittest.main()
