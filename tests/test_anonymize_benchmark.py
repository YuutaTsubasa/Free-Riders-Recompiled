from pathlib import Path
import sys
import tempfile
import os
import unittest
import zipfile

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
        self.assertEqual((copied, left_out), (3, 1))
        self.assertGreater(replaced, 6)

    def test_screenshots_stay_only_when_asked_for(self):
        destination, (_, _, left_out) = self.run_it(keep_screenshots=True)
        self.assertTrue((destination / 'shot.bmp').exists())
        self.assertEqual(left_out, 0)
        destination, _ = self.run_it()
        self.assertFalse((destination / 'shot.bmp').exists())

    def test_other_files_are_copied_untouched(self):
        destination, _ = self.run_it()
        self.assertEqual((destination / 'tool.exe').read_bytes(), b'Alice stays in a binary')


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

    def test_an_older_zip_of_the_same_run_is_replaced(self):
        folder = self.folder({'a.log': b'x'})
        stale = folder.parent / 'run-shareable-part9of9.zip'
        stale.write_bytes(b'old')
        anon.make_archives(folder, 1_000_000)
        self.assertFalse(stale.exists())


if __name__ == '__main__':
    unittest.main()
