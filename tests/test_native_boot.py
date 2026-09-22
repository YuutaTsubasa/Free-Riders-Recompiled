import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get('SFR_CPU_DIAGNOSTIC'), 'set SFR_CPU_DIAGNOSTIC for native boot tests')
class NativeBootTests(unittest.TestCase):
    def run_native(self, directory, *, game_region=False):
        command = [os.environ['SFR_CPU_DIAGNOSTIC'], str(directory)]
        if os.environ.get('SFR_ASSET_DIRECTORY'):
            command.append(os.environ['SFR_ASSET_DIRECTORY'])
        if game_region:
            command.append('--game-region=ntsc-us')
        return subprocess.run(command,
                              capture_output=True, text=True, timeout=15)

    def test_rejects_same_size_unknown_image_before_guest_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'complete.txt').write_text('complete')
            with (directory / 'image.bin').open('wb') as stream:
                stream.truncate(34209792)
            (directory / 'import_variables.tsv').write_text('name\taddress\tordinal\nTest\t2181039992\t403\n')
            result = self.run_native(directory)
            self.assertEqual(result.returncode, 1)
            self.assertIn('fingerprint', result.stderr)
            self.assertNotIn('ENTER ', result.stderr)

    @unittest.skipUnless(os.environ.get('SFR_IMAGE_DIRECTORY') and os.environ.get('SFR_ASSET_DIRECTORY'),
                         'requires locally prepared game image and extracted assets')
    def test_actual_entry_passes_heap_initialization_to_next_missing_service(self):
        result = self.run_native(os.environ['SFR_IMAGE_DIRECTORY'], game_region=True)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertNotRegex(result.stderr, r'STOP (?:worker-)?unsupported-function @0x824e46a0:')
        self.assertRegex(result.stderr, r'NUI_DEVICE_STATUS output=0x[0-9a-f]+ bytes=24 status=0x3 backend=emulated-kinect\n')
        self.assertIn('XAM_SYSTEM_VERSION value=0x20308000 target=2.0.12416.0 source=verified-game-abi\n', result.stderr)
        self.assertRegex(result.stderr, r'RESULT XexGetModuleHandle name=0x82000910 output=0x[0-9a-f]+ status=0x0 module=0x71500000 retained=0\n')
        missing_ex = result.stderr.find('UNAVAILABLE XexGetProcedureAddress module=xam.xex ordinal=0x24')
        self.assertGreaterEqual(missing_ex, 0)
        startup = re.search(r'NATIVE_WSA_STARTUP caller=1 requested=0x2 output=0x[0-9a-f]+ '
                            r'bytes=400 status=0x0 version=0x2 high_version=0x[0-9a-f]+ '
                            r'acquisitions=1 lr=0x8270ce9c backend=windows-winsock\n', result.stderr)
        self.assertIsNotNone(startup, 'original fallback reaches actual native startup with its unchanged arguments')
        self.assertLess(missing_ex, startup.start())
        listener = re.search(r'NATIVE_NOTIFICATION_CREATE mask=0x1 maximum_version=6 handle=(0x[0-9a-f]+) '
                             r'queued=0 lr=0x82232e3c backend=windows-event\n', result.stderr)
        self.assertIsNotNone(listener, 'original constructor creates an owned native notification listener')
        self.assertLess(startup.start(), listener.start())
        self.assertIn('NATIVE_NOTIFICATION_POSITION flags=0x2 horizontal=center vertical=bottom '
                      'lr=0x82232e48 state=retained-placement\n', result.stderr)
        self.assertRegex(result.stderr, rf'ORIGINAL_NOTIFICATION_HANDLE owner=0x[0-9a-f]+ '
                         rf'offset=0x7ee0 handle={listener[1]}\n')
        for index in range(4):
            query = result.stderr.index(f'NATIVE_USER_SIGNIN index={index} state=0 lr=0x822344d8 '
                                        'backend=unselected-game-users\n')
            reset = re.search(rf'ORIGINAL_UNSELECTED_USER index={index} record=0x[0-9a-f]+ '
                              r'state=0 selected=-1 names_cleared=1 profile_cleared=1 reset_virtual=0x82234ec0\n',
                              result.stderr)
            self.assertIsNotNone(reset)
            returned = result.stderr.index(f'ORIGINAL_USER_NAME_CONVERSION_RETURN index={index} '
                                           'characters=1 lr=0x822345ec\n')
            self.assertLess(query, reset.start())
            self.assertLess(reset.start(), returned)
        self.assertRegex(result.stderr, r'NATIVE_MULTIBYTE_UNICODE input=0x[0-9a-f]+ input_bytes=1 '
                         r'output=0x[0-9a-f]+ capacity=32 written_output=0x0 output_bytes=2 status=0x0 '
                         r'lr=0x824d0e3c encoding=ascii-subset\n')
        self.assertIn('NATIVE_GAME_REGION source=command-line profile=ntsc-us value=0xff', result.stderr)
        self.assertIn('ENTER _xstart @0x824d22f0', result.stderr)
        self.assertIn('GRAPHICS_ENTRY name=sub_824F4CF0 address=0x824f4cf0 lr=0x8249b414 sp=0x70131440 r3=0x0 r4=0x1 r5=0x0 r6=0x0 r7=0xffffffff83e53ca8 r8=0xffffffff83e53ca4', result.stderr)
        self.assertRegex(result.stderr, r'NATIVE_GRAPHICS backend=D3D12 adapter=\S[^\n]*\n')
        self.assertIn('NATIVE_CREATE_DEVICE device=0x71600000 width=1280 height=720 result=0', result.stderr)
        self.assertIn('NATIVE_CLEAR source=0x824f6dc8 device=0x71600000 flags=0x3f argb=0x0 target=0x71700000 depth=0 stencil=0 submitted=1', result.stderr)
        self.assertIn('NATIVE_VIEWPORT_REQUEST address=0x70131500 words=0,0,500,2d0,3f800000,0', result.stderr)
        self.assertIn('NATIVE_VIEWPORT source=0x824e96c8 x=0 y=0 width=1280 height=720 min_depth=1 max_depth=0 updated=1', result.stderr)
        self.assertIn('IMPORT RtlInitAnsiString destination=0x70131318 source=0x821a8fe4 length=37 maximum_length=38', result.stderr)
        self.assertIn('RESULT NtCreateFile status=0x0 handle=0x72000004 size=93676 path=game:\\shader\\Xbox360BasicShader.fxobj', result.stderr)
        self.assertRegex(result.stderr, r'RESULT NtQueryInformationFile handle=0x72000004 class=34 length=56 status=0x0 eof=93676 allocation=[0-9]+')
        self.assertIn('RESULT NtReadFile handle=0x72000004 buffer=0x40001ba0 requested=93676 offset=0 transferred=93676 status=0x0 sha1=96b652112909d62121e25f2fdb24bfd3b23869d2', result.stderr)
        self.assertIn('RESULT NtClose handle=0x72000004 status=0x0', result.stderr)
        async_open = re.search(r'RESULT NtCreateFile status=0x0 handle=(0x[0-9a-f]+) '
                               r'size=14774 path=game:\\FNT_SE\n', result.stderr)
        self.assertIsNotNone(async_open)
        self.assertRegex(result.stderr, rf'NATIVE_FILE_MODE handle={async_open[1]} access=0x120089 '
                         r'mode=0x8 alignment_requirement=0x[0-9a-f]+ backend=windows-file\n')
        self.assertIn(f'RESULT NtQueryInformationFile handle={async_open[1]} '
                      'class=27 length=4 status=0xc000000d', result.stderr)
        self.assertIn('RESULT NtSetEvent guest_id=3 handle=0x7210002c previous_output=0x0 '
                      'status=0x0 lr=0x824dd700 backend=windows-event', result.stderr)
        self.assertIn('RESULT NtWaitForSingleObjectEx guest_id=10 handle=0x7210002c status=0x0 '
                      'milliseconds=0 infinite=1 backend=windows-sync', result.stderr)
        self.assertRegex(result.stderr, rf'FILE_READ_ABI guest_id=10 lr=0x824dd208 handle={async_open[1]} '
                         r'event=0x[0-9a-f]+ apc=0x0 context=(0x[0-9a-f]+) io=\1 '
                         r'buffer=0x[0-9a-f]+ length=0x20000 offset_pointer=0x[0-9a-f]+\n')
        completed_read = re.search(
            rf'NATIVE_ASYNC_FILE_DATA handle={async_open[1]} event=(0x[0-9a-f]+) '
            r'io=0x82b50114 buffer=(0x[0-9a-f]+) offset=0 requested=131072 transferred=14774 '
            r'status=0x0 sha1=a0002b1d9e8f7eadd3be7ce07779a1c4ddd80003\n', result.stderr)
        self.assertIsNotNone(completed_read, 'actual guest bytes match the original asset')
        read_result = re.search(
            rf'RESULT NtReadFile handle={async_open[1]} buffer={completed_read[2]} '
            r'requested=131072 offset=0 transferred=(\d+) status=0x([0-9a-f]+)', result.stderr)
        self.assertIsNotNone(read_result)
        if read_result[2] == '103':
            self.assertEqual(read_result[1], '0')
            read_wakeup = result.stderr.index(
                f'RESULT NtWaitForSingleObjectEx guest_id=10 handle={completed_read[1]} status=0x0 ')
            self.assertLess(read_result.start(), completed_read.start())
            self.assertLess(completed_read.start(), read_wakeup,
                            'native bytes and IO status are published before the read event wakes the game')
        else:
            self.assertEqual((read_result[1], read_result[2]), ('14774', '0'))
        header_signal = re.search(
            r'RESULT NtSetEvent guest_id=10 handle=(0x[0-9a-f]+) previous_output=0x0 '
            r'status=0x0 lr=0x824d433c backend=windows-event', result.stderr)
        self.assertIsNotNone(header_signal)
        self.assertNotEqual(header_signal[1], completed_read[1])
        self.assertLess(completed_read.start(), header_signal.start())
        header_wakeup = result.stderr.index(
            f'RESULT NtWaitForSingleObjectEx guest_id=3 handle={header_signal[1]} status=0x0 ')
        self.assertLess(header_signal.start(), header_wakeup,
                        'original worker signals its separate header completion before the caller resumes')
        self.assertIn('WORKER_ENTER guest_id=10 name=sub_824DDCF8 address=0x824ddcf8', result.stderr)
        self.assertIn('IMPORT RtlNtStatusToDosError status=0x103 result=997\n', result.stderr)
        self.assertIn('ORIGINAL_EFFECT_LOADER source=0x825e7f98 device=0x71600000 blob=0x40001ba0', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x82a56044:', result.stderr)
        self.assertIn('ORIGINAL_SHADER_CREATE source=0x824ed770 stage=vertex container=0x40019b18 words=102a1101,178,dc bytes=596 sha1=0aa2cf2beb066de65d5cb9bb9b26bb1110487192', result.stderr)
        self.assertIn('NATIVE_SHADER_CREATE stage=vertex handle=0x71800000', result.stderr)
        self.assertGreaterEqual(result.stderr.count('NATIVE_SHADER_CREATE stage=vertex'), 37)
        self.assertGreaterEqual(result.stderr.count('NATIVE_SHADER_CREATE stage=pixel'), 37)
        self.assertGreaterEqual(result.stderr.count('specialization_mask=0 state=native-stage'), 37)
        self.assertGreaterEqual(result.stderr.count('specialization_mask=2 state=retained-library'), 37)
        self.assertIn('ENTER sub_824DCF70 @0x824dcf70', result.stderr)
        self.assertIn('IMPORT RtlImageXexHeaderField key=0x20401 result=0x0', result.stderr)
        self.assertNotIn('STOP import-variable @0x82000778', result.stderr)
        self.assertIn('REQUEST NtAllocateVirtualMemory', result.stderr)
        self.assertIn('RESULT NtAllocateVirtualMemory status=0x0 base=0x40000000 size=0x100000', result.stderr)
        self.assertIn('base=0x40000000 size=0x10000 type=0x60001000 protect=0x4 debug=0x0', result.stderr)
        self.assertIn('RESULT NtAllocateVirtualMemory status=0x0 base=0x40000000 size=0x10000\n', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb99c: __imp__NtAllocateVirtualMemory', result.stderr)
        self.assertIn('IMPORT KeGetCurrentProcessType result=0x1', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb59c: __imp__KeGetCurrentProcessType', result.stderr)
        self.assertIn('IMPORT RtlInitializeCriticalSection address=0x40000618', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb49c: __imp__RtlInitializeCriticalSection', result.stderr)
        self.assertIn('ENTER sub_824DF608 @0x824df608', result.stderr)
        self.assertIn('BIND XboxHardwareInfo address=0x71200000 flags=0x20', result.stderr)
        self.assertNotIn('STOP import-variable @0x82000798: __imp__XboxHardwareInfo', result.stderr)
        self.assertIn('ENTER sub_824DCE98 @0x824dce98', result.stderr)
        self.assertIn('IMPORT RtlEnterCriticalSection address=0x82ad08f0 owner=0x70000bb0 recursion=1 lock_count=0x0', result.stderr)
        self.assertIn('IMPORT RtlLeaveCriticalSection address=0x82ad08f0 owner=0x0 recursion=0 lock_count=0xffffffff', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb4ac: __imp__RtlEnterCriticalSection', result.stderr)
        self.assertIn('IMPORT XexCheckExecutablePrivilege privilege=10 result=0', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb5ac: __imp__XexCheckExecutablePrivilege', result.stderr)
        self.assertIn('IMPORT RtlInitializeCriticalSectionAndSpinCount address=0x82c31af8 spin=4000 encoded=16 status=0', result.stderr)
        self.assertEqual(result.stderr.count(' spin=4000 encoded=16 status=0'), 14)
        self.assertNotIn('STOP import-function @0x82acc2dc: __imp__RtlInitializeCriticalSectionAndSpinCount', result.stderr)
        self.assertIn('TLS static=0x71300000 dynamic=0x7130009c slots=64 data_size=156 raw_size=156', result.stderr)
        self.assertIn('IMPORT KeTlsAlloc result=0x0', result.stderr)
        self.assertNotIn('STOP import-function @0x82acbccc: __imp__KeTlsAlloc', result.stderr)
        self.assertIn('IMPORT KeTlsSetValue index=0x0 value=0x400006a0 result=0x1', result.stderr)
        self.assertRegex(result.stderr, r'IMPORT KeQuerySystemTime output=0x7013fc70 ticks=[0-9]+')
        self.assertNotIn('STOP import-function @0x82acb94c: __imp__KeQuerySystemTime', result.stderr)
        self.assertRegex(result.stderr, r'BIND KeTimeStampBundle address=0x71400000 uptime_ms=[0-9]+')
        self.assertNotIn('STOP import-variable @0x82000758: __imp__KeTimeStampBundle', result.stderr)
        self.assertRegex(result.stderr, r'TIME_BASE ticks=[0-9]+ frequency=49875000')
        self.assertNotIn('STOP unsupported-function @0x82a52f10:', result.stderr)
        self.assertIn('STORE_CONDITIONAL address=0x82afee7c value=0x0 success=1', result.stderr)
        self.assertIn('STORE_CONDITIONAL address=0x82afee7c value=0x1 success=1', result.stderr)
        self.assertIn('IMPORT RtlInitializeCriticalSection address=0x82c31940', result.stderr)
        self.assertIn('IMPORT RtlInitializeCriticalSection address=0x82c31994', result.stderr)
        self.assertIn('VECTOR_LOAD address=0x820bb520', result.stderr)
        self.assertIn('VECTOR_STORE address=0x82b60ab0', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x82ac9470:', result.stderr)
        self.assertIn('IMPORT KeQueryPerformanceFrequency result=49875000', result.stderr)
        self.assertIn('IMPORT RtlInitializeCriticalSection address=0x82b7674c', result.stderr)
        self.assertIn('UNAVAILABLE __imp__EtxProducerRegister address=0x82acc28c status=0xc0000002', result.stderr)
        self.assertNotIn('STOP import-function @0x82acc28c: __imp__EtxProducerRegister', result.stderr)
        self.assertIn('IMPORT RtlInitializeCriticalSection address=0x82afee00', result.stderr)
        self.assertIn('IMPORT XexLoadImage module=xam.xex flags=0x9 min_version=0x0 output=0x7013fbd0 handle=0x71500000 load_count=1 status=0x0', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb71c: __imp__XexLoadImage', result.stderr)
        for ordinal in ('aff', 'b00', 'b0b', 'b10', '305', '30b'):
            self.assertIn(f'UNAVAILABLE XexGetProcedureAddress module=xam.xex ordinal=0x{ordinal} output=0x7013fbc0 result=0x0 status=0xc0000263', result.stderr)
        self.assertEqual(result.stderr.count('IMPORT RtlNtStatusToDosError status=0xc0000263 result=127'), 6)
        self.assertIn('IMPORT XexUnloadImage handle=0x71500000 resident=1 load_count=1 status=0x0', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb70c: __imp__XexGetProcedureAddress', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x82211798:', result.stderr)
        self.assertIn('IMPORT MmQueryStatistics output=0x70131510 status=0x0 budget_pages=131072', result.stderr)
        self.assertNotIn('STOP import-function @0x82acc44c: __imp__MmQueryStatistics', result.stderr)
        self.assertIn('kernel_pages=17 available_pages=122622 virtual_capacity=536870912 reserved_virtual=1048576 stack_pages=64 image_pages=8352 virtual_pages=17', result.stderr)
        self.assertRegex(result.stderr, r'IMPORT XGetVideoMode output=0x70131530 source=windows-current-display width=[1-9][0-9]* height=[1-9][0-9]* refresh_hz=[0-9.]+ interlaced=0 widescreen=[01] high_definition=[01] guest_profile=reference-compatible')
        self.assertNotIn('STOP import-function @0x82acb23c: __imp__XGetVideoMode', result.stderr)
        self.assertIn('IMPORT XGetVideoMode output=0x70131600 source=windows-current-display', result.stderr)
        # Accounting includes both the monitor and video-global backing pages.
        # The game derives its heap from the updated availability (three pages
        # fewer since the kernel version and XMA register pages are mapped).
        self.assertIn('REQUEST MmAllocatePhysicalMemoryEx flags=0x0 size=0x71fe000 protect=0x4 min=0x0 max=0xffffffff alignment=0x4', result.stderr)
        self.assertIn('RESULT MmAllocatePhysicalMemoryEx base=0xf8b02000 committed_bytes=0x71fe000', result.stderr)
        self.assertNotIn('STOP import-function @0x82acba0c: __imp__MmAllocatePhysicalMemoryEx', result.stderr)
        self.assertNotIn('__imp__VdInitializeEngines', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x824d1858:', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x824f44f8:', result.stderr)
        self.assertIn('IMPORT RtlEnterCriticalSection address=0x82ad440c owner=0x70000bb0 recursion=1 lock_count=0x0', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x825e47d8:', result.stderr)
        self.assertIn('IMPORT RtlLeaveCriticalSection address=0x82ad440c owner=0x0 recursion=0 lock_count=0xffffffff', result.stderr)
        for index, count in enumerate((2, 3, 3, 4)):
            self.assertRegex(result.stderr, rf'ORIGINAL_VERTEX_DECLARATION index={index} pointer=0x[0-9a-f]+ count={count} metadata_valid=1 elements_match=1')
        self.assertNotIn('STOP native-graphics-function @0x824bbce8:', result.stderr)
        self.assertIn('RESULT NtCreateSemaphore output=0x701313a0 handle=0x72100004 status=0x0 initial=1 maximum=1 backend=windows-semaphore', result.stderr)
        self.assertIn('RESULT NtCreateEvent output=0x70131330 handle=0x72100008 status=0x0 type=1 initial=0 backend=windows-event', result.stderr)
        self.assertIn('RESULT NtClearEvent handle=0x72100008 status=0x0', result.stderr)
        self.assertRegex(result.stderr, r'RESULT ExCreateThread output=0x70131360 handle=0x72200004 status=0x0 startup=0x824dd0a8 worker=0x824b1e58 argument=0xffcfd240 flags=0x1 pcr=0x73000000 thread=0x73001000 tls=0x73002000 stack_limit=0x73021000 stack_base=0x73025000 guest_id=2 native_id=[1-9][0-9]* suspended=1 entry_started=0 backend=windows-thread')
        self.assertIn('ORIGINAL_WORKER_TARGET context=0xffcfd240 vtable=0x821aa704 target=0x824b2320', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb77c: __imp__ExCreateThread', result.stderr)
        self.assertIn('BIND ExThreadObjectType identity=0x72300000 descriptor=guarded', result.stderr)
        self.assertNotIn('STOP import-variable @0x820007e8: __imp__ExThreadObjectType', result.stderr)
        for object_address, increment in (('73001000', -1), ('73201000', 1)):
            self.assertIn(f'RESULT KeSetBasePriorityThread object=0x{object_address} increment={increment} previous=0 current=0 backend=windows-thread', result.stderr)
            self.assertRegex(result.stderr, rf'RESULT KeSetAffinityThread object=0x{object_address} mask=0x8 status=0x0 previous=0x1 host_mask=0x[1-9a-f][0-9a-f]* backend=windows-thread')
            self.assertEqual(result.stderr.count(f'RESULT ObDereferenceObject object=0x{object_address} references=0'), 2)
        self.assertRegex(result.stderr, r'RESULT ExCreateThread output=0x701313d0 handle=0x72200008 status=0x0 startup=0x824dd0a8 worker=0x824b1e58 argument=0xffcfd1b0 flags=0x1 pcr=0x73200000 thread=0x73201000 tls=0x73202000 stack_limit=0x73221000 stack_base=0x73225000 guest_id=3 native_id=[1-9][0-9]* suspended=1 entry_started=0 backend=windows-thread')
        self.assertIn('ORIGINAL_WORKER_TARGET context=0xffcfd1b0 vtable=0x821aa704 target=0x824b2320', result.stderr)
        self.assertIn('RESULT NtResumeThread handle=0x72200008 output=0x70131450 status=0x0 previous=1 guest_id=3 backend=windows-thread', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb54c: __imp__NtResumeThread', result.stderr)
        created = re.search(r'RESULT ExCreateThread output=0x701313d0[^\n]*native_id=([1-9][0-9]*) ', result.stderr)
        begun = re.search(r'ORIGINAL_WORKER_BEGIN guest_id=3 host_thread=([1-9][0-9]*) startup=0x824dd0a8 worker=0x824b1e58 argument=0xffcfd1b0 pcr=0x73200000', result.stderr)
        self.assertIsNotNone(created)
        self.assertIsNotNone(begun)
        self.assertEqual(created.group(1), begun.group(1), 'the original worker executes on its created native thread')
        previous_position = begun.start()
        for name, address in (('sub_824DD0A8', '824dd0a8'), ('sub_824DCE98', '824dce98'),
                              ('sub_824B1E58', '824b1e58'), ('sub_824B2320', '824b2320')):
            position = result.stderr.index(f'WORKER_ENTER guest_id=3 name={name} address=0x{address}')
            self.assertGreater(position, previous_position)
            previous_position = position
        self.assertIn('IMPORT RtlEnterCriticalSection address=0x82ad08f0 owner=0x73201000 recursion=1 lock_count=0x0', result.stderr)
        self.assertIn('ORIGINAL_WAIT_REQUEST guest_id=3 handle=0x7210000c mode=0x1 alertable=0x0 timeout_ptr=0x73224c20 timeout_bits=0xfffffffffffec780', result.stderr)
        self.assertNotIn('STOP worker-import-function @0x82acb63c:', result.stderr)
        created_workers = dict(re.findall(r'RESULT ExCreateThread[^\n]*guest_id=(\d+) native_id=(\d+)', result.stderr))
        running_workers = dict(re.findall(r'ORIGINAL_WORKER_BEGIN guest_id=(\d+) host_thread=(\d+)', result.stderr))
        # The bounded boot now runs far enough for later workers (music player,
        # streaming) too; the startup workers must all be present.
        self.assertLessEqual({str(i) for i in range(2, 17)}, set(created_workers))
        self.assertLessEqual({'3', '4', '5', '6', '10', '11', '12', '13', '14', '15', '16'}, set(running_workers))
        for guest_id, host_id in running_workers.items():
            self.assertEqual(host_id, created_workers[guest_id], 'each original worker uses its created native thread')
        for guest_id, handle in ((4, '72100010'), (5, '72100014')):
            self.assertIn(f'ORIGINAL_WAIT_REQUEST guest_id={guest_id} handle=0x{handle} mode=0x1 alertable=0x0 timeout_ptr=0x0\n', result.stderr)
        self.assertIn('ORIGINAL_WAIT_REQUEST guest_id=6 handle=0x72100018 mode=0x1 alertable=0x0 timeout_ptr=0x73824c30 timeout_bits=0xffffffffffffb1e0', result.stderr)
        # Relative wait completion/count depends on host scheduling; only actual timeout is valid here.
        for guest_id, handle, status, milliseconds in re.findall(
                r'RESULT NtWaitForSingleObjectEx guest_id=(\d+) handle=0x([0-9a-f]+) status=0x([0-9a-f]+) milliseconds=(\d+) infinite=0 backend=windows-sync', result.stderr):
            self.assertIn((guest_id, handle, milliseconds), {
                ('3', '7210000c', '8'), ('4', '72100010', '2'), ('6', '72100018', '2')})
            self.assertEqual(status, '102', 'unsignaled original events must never report invented success')
        # Lock-free list pushes: node pointer (heap address, allocation-order
        # dependent) in the high word, depth in the low word; nodes are 16 apart.
        first = re.search(r'STORE_CONDITIONAL_DOUBLEWORD address=0x400019a8 value=0x(400[0-9a-f]{5})00000001 success=1', result.stderr)
        self.assertIsNotNone(first)
        node = int(first[1], 16)
        for offset, depth in ((0x10, 1), (0x20, 2), (0x30, 3)):
            self.assertIn(f'STORE_CONDITIONAL_DOUBLEWORD address=0x40001980 value=0x{node - offset:08x}{depth:08x} success=1',
                          result.stderr)
        self.assertNotIn('STOP unsupported-function @0x8274f4b8:', result.stderr)
        for guest_id, pcr, argument in ((7, '73a', '83e544f4'), (8, '73c', '83e544fc'), (9, '73e', '83e54504')):
            self.assertRegex(result.stderr, rf'RESULT ExCreateThread[^\n]*startup=0x824dd0a8 worker=0x8249ef00 argument=0x{argument} flags=0x1 pcr=0x{pcr}00000 thread=0x{pcr}01000 tls=0x{pcr}02000 stack_limit=0x{pcr}21000 stack_base=0x{pcr}61000 guest_id={guest_id} native_id=[1-9][0-9]* suspended=1 entry_started=0 backend=windows-thread')
        self.assertIn('ORIGINAL_RENDERER_STATE object=0x83e53710 device=0x71600000 context=0x83e53720 manager_field=0xffcffd40 identity_matrices=3', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x824c1048:', result.stderr)
        self.assertIn('BIND KeDebugMonitorData cell=0x72400000 monitor=0x0 profile=absent-xbox-monitor', result.stderr)
        self.assertNotIn('STOP import-variable @0x820007d4:', result.stderr)
        self.assertIn('RESULT NtCreateEvent output=0x82b50080 handle=0x72100028 status=0x0 type=0 initial=1 backend=windows-event', result.stderr)
        self.assertIn('RESULT NtCreateEvent output=0x82b50124 handle=0x72100034 status=0x0 type=1 initial=0 backend=windows-event', result.stderr)
        self.assertNotIn('STOP thread-flags @0x8000001:', result.stderr)
        self.assertRegex(result.stderr, r'RESULT ExCreateThread output=0x82b50140 handle=0x72200024 status=0x0 startup=0x824d2a10 worker=0x824d4388 argument=0x82b4ffc8 flags=0x8000001 pcr=0x74000000 thread=0x74001000 tls=0x74002000 stack_limit=0x74021000 stack_base=0x74061000 guest_id=10 native_id=[1-9][0-9]* suspended=1 entry_started=0 backend=windows-thread')
        processor = re.search(r'THREAD_CREATION_PROCESSOR guest_id=10 parent_cpu=0 pcr_cpu=3 thread_cpu=3 host_mask=0x([0-9a-f]+)', result.stderr)
        self.assertIsNotNone(processor, 'original explicit CPU selection is reflected in both guest structures')
        host_mask = int(processor.group(1), 16)
        self.assertGreater(host_mask, 0)
        self.assertEqual(host_mask & (host_mask - 1), 0, 'actual native thread is bound to one allowed processor')
        self.assertIn('RESULT NtResumeThread handle=0x72200024 output=0x0 status=0x0 previous=1 guest_id=10 backend=windows-thread', result.stderr)
        for name, address in (('sub_824D2A10', '824d2a10'), ('sub_824D4388', '824d4388'),
                              ('sub_824D42A8', '824d42a8'), ('sub_824DD718', '824dd718')):
            self.assertIn(f'WORKER_ENTER guest_id=10 name={name} address=0x{address}', result.stderr)
        self.assertIn('ORIGINAL_WAIT_REQUEST guest_id=10 handle=0x7210002c mode=0x1 alertable=0x0 timeout_ptr=0x0', result.stderr)
        queue_signal = result.stderr.index('RESULT NtSetEvent guest_id=3 handle=0x7210002c ')
        queue_wakeup = result.stderr.index('RESULT NtWaitForSingleObjectEx guest_id=10 handle=0x7210002c status=0x0 ')
        self.assertLess(queue_signal, queue_wakeup, 'original producer signals the real queue before the worker resumes')
        self.assertLess(queue_wakeup, result.stderr.index('FILE_READ_ABI guest_id=10 lr=0x824dd208 '),
                        'original queue worker issues the file read after its actual wakeup')
        self.assertIn('RESULT XexGetModuleHandle name=0x0 output=0x701316b0 status=0x0 module=0x71000040 retained=0', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb73c:', result.stderr)
        self.assertIn('RESULT XexGetModuleSection module=0x71000040 name_ptr=0x8219816c status=0x0 data=0x84099e80 size=0x1080 sha1=2aae5962f6b55ed309919dd3d32c7c2867908e8e', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb72c:', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x8253a6a8:', result.stderr)
        self.assertIn('ORIGINAL_DDS_PARSER_REQUEST parser=0x70131090 data=0x84099e80 size=0x1080 info_output=0x0 flags=0x1 lr=0x8250e25c', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x8250e960:', result.stderr)
        self.assertIn('ORIGINAL_DDS_COPY_REQUEST parser=0x70131090 format=0x1a200054 source=0x84099f00 destination=0x40001ba0 row_pitch=0x100 slice_pitch=0x1000 width=64 height=64 depth=1 type=3 next_mip=0 next_face=0', result.stderr)
        self.assertIn('RESULT MmQueryAddressProtect address=0x40001ba0 protection=0x4 lr=0x825f87c4', result.stderr)
        self.assertIn('RESULT MmQueryAddressProtect address=0x84099f00 protection=0x2 lr=0x825f87e4', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb6dc:', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x825f8230:', result.stderr)
        self.assertIn('ORIGINAL_DDS_BLOCK_COPY destination=0x40001ba0 source=0x84099f00 mode=0x20001 stride=0x2 count=0x800 sp=0x70130ce0', result.stderr)
        self.assertIn('CACHE_BLOCK_ZERO effective=0x40001c00 aligned=0x40001c00 bytes=32', result.stderr)
        self.assertRegex(result.stderr, r'ORIGINAL_DDS_BLOCK_COPY_RETURN destination=0x40001ba0 source=0x84099f00 bytes=4096 swapped_pairs=1 source_unchanged=1 cache_zeroes=[1-9][0-9]*')
        self.assertNotIn('STOP native-graphics-function @0x8250d6e0:', result.stderr)
        self.assertIn('ORIGINAL_DDS_PARSER_STATE parser=0x70131090 pixels=0x40001ba0 format=0x1a200054 owned=1 width=64 height=64 depth=1', result.stderr)
        self.assertIn('REQUEST MmAllocatePhysicalMemoryEx flags=0x0 size=0x2000 protect=0x404 min=0x0 max=0xffffffff alignment=0x1000', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x8250c848:', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x824f29e8:', result.stderr)
        # Heap and physical addresses depend on allocation order (the original
        # shader creators allocate real shader objects), so check structure
        # and the relations between the addresses instead of fixed values.
        address = r'(0x[0-9a-f]+)'
        transfer = re.search(r'ORIGINAL_TEXTURE_TRANSFER_REQUEST r3=' + address + r' r4=0x0 r5=0x0 r6=0x40001ba0 '
                             r'r7=0x1a200054 r8=0x100 r9=0x70131140 r10=0x701310a8 sp=0x70130ff0 lr=0x8250e6e8', result.stderr)
        self.assertIsNotNone(transfer)
        surface = transfer[1]
        access = re.search(r'ORIGINAL_RESOURCE_ACCESS_REQUEST r3=0x400007c0 r4=0x0 r5=0xe r6=0x0 r7=' + address +
                           r' r8=0x0 r9=\1 r10=0x2000 sp=0x70130a60 lr=0x824f3560 option84=0x0 option92=0x2000000', result.stderr)
        self.assertIsNotNone(access)
        physical = access[1]
        self.assertRegex(result.stderr, 'RESULT MmAllocatePhysicalMemoryEx base=' + physical +
                         r' committed_bytes=0x[0-9a-f]+ cache=write-combined')
        self.assertIn(f'NATIVE_RESOURCE_COHERENCY resource=0x400007c0 address={physical} size=0x2000 '
                      'mask=0x2000000 caller=0x824f3560 operation=14 ownership=cpu-only queue_completed=1', result.stderr)
        self.assertIn(f'ORIGINAL_TEXTURE_TRANSFER_RETURN surface={surface} caller=0x8250e718 result=0', result.stderr)
        self.assertIn('ORIGINAL_TEXTURE_UNLOCK_STATE resource=0x400007c0 header=0x100003', result.stderr)
        self.assertIn(f'VIDEO_GLOBAL_READ cell=0x72500000 value=0x0 resource={surface} caller=0x824f1fb8', result.stderr)
        self.assertIn(f'ORIGINAL_SHARED_SURFACE_HEAP_RESULT resource={surface} result=1', result.stderr)
        self.assertNotIn('STOP import-variable @0x82000664:', result.stderr)
        # The heap decommits a trailing and interior 64 KiB range, then the
        # large DDS buffer is released whole.
        self.assertRegex(result.stderr, r'ORIGINAL_VIRTUAL_FREE_REQUEST lr=0x824e0dd8 base_ptr=0x70130a94 size_ptr=0x70130a90 '
                                        r'type=0x4000 base=0x400[0-9a-f]0000 size=0x10000')
        self.assertRegex(result.stderr, r'RESULT NtFreeVirtualMemory status=0x0 base=0x400[0-9a-f]0000 size=0x30000 '
                                        r'reclaimed=0x30000 reserved=0x260000')
        self.assertIn('ORIGINAL_DDS_PARSER_STATE parser=0x70131090 pixels=0x40260030 format=0x1a200054 owned=1 width=1024 height=1024 depth=1', result.stderr)
        self.assertIn('ORIGINAL_VIRTUAL_FREE_REQUEST lr=0x824e2134 base_ptr=0x70130ef0 size_ptr=0x70130ee0 type=0x8000 base=0x40260000 size=0x0', result.stderr)
        self.assertIn('RESULT NtFreeVirtualMemory status=0x0 base=0x40260000 size=0x410000 reclaimed=0x410000 reserved=0x260000', result.stderr)
        # The first 21 distinct original textures are written in order to
        # consecutive outputs; later loading creates more elsewhere.
        created = re.findall(r'ORIGINAL_TEXTURE_CREATE_RETURN result=0 output=(0x[0-9a-f]+) texture=(0x[0-9a-f]+)', result.stderr)[:21]
        self.assertEqual([int(output, 16) for output, _ in created], [0x701320a4 + 8 * i for i in range(21)])
        self.assertEqual(created[0][1], '0x400007c0')
        self.assertEqual(created[1][1], surface)
        self.assertEqual(len({texture for _, texture in created}), 21)
        self.assertNotIn('STOP virtual-memory-request @0x40260000:', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb98c:', result.stderr)
        self.assertIn('ORIGINAL_RENDERER_INIT_REQUEST device=0x71600000 parameters=0x83e53ca8 vertex_selector=4 pixel_selector=4', result.stderr)
        self.assertIn('ORIGINAL_RENDERER_MATRIX_COPY_RETURN destination=0x82b628d0 bytes=64 match=1 source_unchanged=1', result.stderr)
        self.assertIn('ORIGINAL_RENDERER_CPU_SETUP device=0x71600000 caps_bytes=304 caps_match=1 parameters_bytes=124 parameters_match=1 width=1280 height=720 vertex_selector=3 pixel_selector=3', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x827f61a0:', result.stderr)
        self.assertIn('NATIVE_BLEND_CONTROL source=0x824e9218 target=0 packed=0x7010706 state=retained-for-pipeline', result.stderr)
        defaults = result.stderr.index('NATIVE_BLEND_DEFAULTS table=0x82ad0a60 requested=0x10001 '
                                       'flags=0x240000 effective=0x10001 targets=4\n')
        previous_blend = defaults
        for address, value, requested, effective, lr in [
                ('824e6a40','1','10001','10001','8221171c'),
                ('824e6b60','6','10006','60006','82211734'),
                ('824e6bf0','7','10706','7060706','82211740')]:
            current_blend = result.stderr.index(f'NATIVE_BLEND_REQUEST source=0x{address} value=0x{value} '
                f'requested=0x{requested} flags=0x80240000 effective=0x{effective} '
                f'lr=0x{lr} state=retained-for-pipeline\n')
            self.assertLess(previous_blend, current_blend)
            previous_blend = current_blend
            self.assertNotIn(f'STOP native-graphics-function @0x{address}:', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x824e9218:', result.stderr)
        sampler_default = result.stderr.index('NATIVE_SAMPLER_DEFAULTS table=0x82ad0f20 slots=26 '
            'word3=0x1000000 word4=0x0 requested_anisotropy=1 volume_control=0\n')
        sampler_min = result.stderr.index('NATIVE_SAMPLER_FILTER source=0x824e8248 slot=0 value=0x1 '
            'word3=0x1200000 word4=0x2 lr=0x82211750 state=retained-fetch-fields\n')
        sampler_mag = result.stderr.index('NATIVE_SAMPLER_FILTER source=0x824e83f0 slot=0 value=0x1 '
            'word3=0x1280000 word4=0x3 lr=0x82211760 state=retained-fetch-fields\n')
        sampler_inline = re.search(r'ORIGINAL_SAMPLER_INLINE_MIP_RETURN source=0x8221176c slot=0 '
            r'word3=0xa80000 word4=0x3 dirty=0x[0-9a-f]+ next=0x8222bdb0 filters=linear/linear/linear\n',
            result.stderr)
        self.assertIsNotNone(sampler_inline)
        self.assertLess(sampler_default, sampler_min)
        self.assertLess(sampler_min, sampler_mag)
        self.assertLess(sampler_mag, sampler_inline.start())
        # The loading screen now runs until the bounded call budget: every
        # frame after the first draws its textured sprites natively.
        self.assertRegex(result.stderr, r'NATIVE_PRESENT source=0x824e65a0 device=0x71600000 lr=0x8249bc70 draws=4 textured=4 [^\n]*presented=1')
        for address, offset, value in [('824e6a08','60','1'),('824e6ec8','68','4'),
                                       ('824e6e68','64','0'),('824e70d0','28','1'),
                                       ('824e7140','2c','3'),('824e7110','30','1'),('824e69a8','38','6')]:
            self.assertIn(f'NATIVE_RENDER_STATE source=0x{address} offset=0x{offset} value=0x{value} retained=1', result.stderr)
            self.assertNotIn(f'STOP native-graphics-function @0x{address}:', result.stderr)
        for address, offset, value, lr in [('824e70d0','28','1','827f617c'),
                                            ('824e7140','2c','6','827f6188'),
                                            ('824e7110','30','1','827f6194'),
                                            ('824e69a8','38','6','822116d0')]:
            self.assertIn(f'NATIVE_RENDER_STATE source=0x{address} offset=0x{offset} '
                          f'value=0x{value} retained=1 lr=0x{lr}', result.stderr)
        for slot in range(8):
            self.assertIn(f'NATIVE_TEXTURE_BIND source=0x824f4220 slot={slot} texture=0x0 mask=0x{0x80000000 >> slot:x} lr=0x827f5fc8 state=unbound', result.stderr)
        # The audited null bindings (LR 827F5FC8) pass; a later real-texture
        # binding from another caller may still stop at the same setter.
        stopped = re.search(r'NATIVE_GRAPHICS_BOUNDARY address=0x824f4220 lr=0x([0-9a-f]+)', result.stderr)
        if stopped:
            self.assertNotEqual(stopped[1], '827f5fc8')
        self.assertIn('ORIGINAL_RENDERER_FLOAT_TABLE_RETURN source=0x82811050 destination=0x83e59580 bytes=1440 sha1=', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x82811050:', result.stderr)
        self.assertIn('ORIGINAL_SHADER_CREATE source=0x824ed770 stage=vertex container=0x820d2600 words=102a1101,d4,54 bytes=296 sha1=137f901318ccfc84bca32413841e7e86c80be385', result.stderr)
        self.assertNotIn('original shader is absent from the prepared native cache', result.stderr)
        for source in ('820d2600', '820d2728', '820d2848'):
            self.assertIn(f'ORIGINAL_SHADER_CREATE source=0x824ed770 stage=vertex container=0x{source} ', result.stderr)
        for source in ('820d2988', '820d2a28', '820d2b10'):
            self.assertIn(f'ORIGINAL_SHADER_CREATE source=0x824ed588 stage=pixel container=0x{source} ', result.stderr)
        declarations = re.findall(r'ORIGINAL_RENDERER_DECLARATION index=(\d+) source=0x([0-9a-f]+) '
                                  r'global=0x([0-9a-f]+) pointer=0x([0-9a-f]+) count=(\d+) '
                                  r'metadata_valid=1 elements_match=1 terminator_valid=1', result.stderr)
        # The tracing entry now runs again later in the boot; every pass must
        # report the same twelve declarations.
        self.assertTrue(declarations and len(declarations) % 12 == 0)
        self.assertEqual(declarations, declarations[:12] * (len(declarations) // 12))
        declarations = declarations[:12]
        self.assertEqual([int(row[0]) for row in declarations], list(range(12)))
        self.assertEqual([int(row[1], 16) for row in declarations], [0x820D2BF8 + i * 60 for i in range(12)])
        self.assertEqual([int(row[2], 16) for row in declarations], [0x82B62938 + i * 4 for i in range(12)])
        pointers = {int(row[3], 16) for row in declarations}
        self.assertEqual(len(pointers), 12)
        self.assertNotIn(0, pointers)
        self.assertEqual([int(row[4]) for row in declarations], [1,2,3,1,2,3,2,3,2,3,4,4])
        self.assertRegex(result.stderr, r'ORIGINAL_RENDERER_SHADER_PUBLICATION result=0 '
                         r'handles=0x[0-9a-f]+(?:,0x[0-9a-f]+){5} owners_valid=1 stages_valid=1 sources_match=1 unique_handles=1')
        self.assertIn('NATIVE_PRIMITIVE_RESTART source=0x824e7f68 value=0x1 enabled=1 retained=1', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x824e7f68:', result.stderr)
        self.assertNotIn('STOP unsupported-function @0x8222b520:', result.stderr)
        language = re.search(r'NATIVE_USER_LANGUAGE source=GetUserDefaultUILanguage '
                             r'windows_langid=0x([0-9a-f]+) xbox_language=(\d+)\n', result.stderr)
        self.assertIsNotNone(language)
        language_id = int(language[2])
        self.assertIn(language_id, range(1, 13))
        self.assertIn('IMPORT ExGetXConfigSetting category=3 setting=9 buffer=0x701316b4 '
                      'capacity=0x4 required=0x701316b0 status=0x0 language=' + str(language_id) + '\n', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb5fc:', result.stderr)
        # The original caller queries region for Xbox languages outside its six assets.
        if language_id >= 7:
            self.assertIn('IMPORT XGetGameRegion source=configured-profile result=0xff lr=0x8222af00', result.stderr)
        self.assertNotIn('STOP import-function @0x82acb24c:', result.stderr)
        self.assertNotIn('STOP game-region', result.stderr)
        if language_id >= 7:
            self.assertIn('ORIGINAL_LANGUAGE_SELECTION_RETURN object=0x70131d10 next=0x82213218 '
                          'index=0 asset_byte=69 japanese=0 fallback_byte=69', result.stderr)
        self.assertIn('REQUEST NtCreateEvent output=0x70131640 attributes=0x70131650 '
                      'type=0x0 initial=0x0 lr=0x824d01fc', result.stderr)
        skeleton_event = re.search(r'RESULT NtCreateEvent output=0x70131640 '
                                   r'handle=(0x[0-9a-f]+) status=0x0 type=0 initial=0 '
                                   r'backend=windows-event name=Event_NuiGetSkeleton\n', result.stderr)
        self.assertIsNotNone(skeleton_event)
        self.assertNotIn('STOP sync-attributes @0x70131650:', result.stderr)
        self.assertIn(f'RESULT NtClearEvent handle={skeleton_event[1]} status=0x0', result.stderr)
        self.assertRegex(result.stderr, r'RESULT ExCreateThread output=0x70131670 handle=0x72200028 '
                         r'status=0x0 startup=0x824dd0a8 worker=0x824395e8 argument=0x701327e0 '
                         r'flags=0x1 pcr=0x74200000 thread=0x74201000 tls=0x74202000 '
                         r'stack_limit=0x74221000 stack_base=0x74261000 guest_id=11 native_id=\d+ '
                         r'suspended=1 entry_started=0 backend=windows-thread')
        self.assertIn('RESULT KeSetAffinityThread object=0x74201000 mask=0x4 status=0x0 '
                      'previous=0x1 host_mask=0x4 backend=windows-thread', result.stderr)
        self.assertIn('RESULT NtResumeThread handle=0x72200028 output=0x70131670 status=0x0 '
                      'previous=1 guest_id=11 backend=windows-thread', result.stderr)
        self.assertIn('WORKER_ENTER guest_id=11 name=sub_824395E8 address=0x824395e8', result.stderr)
        self.assertIn(f'ORIGINAL_WAIT_REQUEST guest_id=11 handle={skeleton_event[1]} mode=0x1 '
                      'alertable=0x0 timeout_ptr=0x0', result.stderr)
        self.assertIn('REQUEST ExGetXConfigSetting category=3 setting=14 buffer=0x701311d0 '
                      'capacity=0x1 required=0x701311d2 lr=0x824d1bd4', result.stderr)
        country = re.search(r'NATIVE_USER_COUNTRY source=GetUserDefaultGeoName iso=([A-Z]{2}) '
                            r'xbox_country=(\d+)\n', result.stderr)
        self.assertIsNotNone(country)
        self.assertIn('IMPORT ExGetXConfigSetting category=3 setting=14 buffer=0x701311d0 '
                      'capacity=0x1 required=0x701311d2 status=0x0 country=' + country[2] + '\n', result.stderr)
        self.assertNotIn('STOP system-config @0x701311d0:', result.stderr)
        translated = re.search(r'ORIGINAL_COUNTRY_TRANSLATION_RETURN value=(\d+) '
                               r'next=0x824d0b58 lr=0x82439678\n', result.stderr)
        self.assertIsNotNone(translated)
        if country[1] == 'TW':
            self.assertEqual(int(translated[1]), 34)
        elif country[1] == 'US':
            self.assertEqual(int(translated[1]), 36)
        self.assertNotIn('STOP indirect-call @0x824d1d2c:', result.stderr)
        self.assertLess(result.stderr.index('NATIVE_RESOURCE_COHERENCY '), result.stderr.index('ORIGINAL_TEXTURE_TRANSFER_RETURN '))
        # Audited packet writers run on the native device; their packets are discarded.
        self.assertNotIn('STOP native-graphics-function @0x824ec0a8:', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x824eb5d0:', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x824e66b8:', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x82a5606c:', result.stderr)
        # The first original present: resolve RT0 into the front buffer and swap.
        self.assertRegex(result.stderr, r'NATIVE_PRESENT source=0x824e65a0 device=0x71600000 lr=0x8249bc70 draws=0 textured=0 [^\n]*presented=1')
        self.assertNotIn('STOP native-graphics-function @0x824e65a0:', result.stderr)
        # The worker polls gamepad 0 (keyboard-backed, so always connected).
        self.assertRegex(result.stderr, r'NATIVE_INPUT user=0 status=0x0 packet=\d+ buttons=0x[0-9a-f]+ '
                                        r'lr=0x824c64d0 backend=xinput\+keyboard')
        self.assertNotIn('STOP worker-import-function @0x82acc14c:', result.stderr)
        self.assertIn('NATIVE_BLEND_REQUEST source=0x824e6ad0 value=0x0 ', result.stderr)
        self.assertNotIn('STOP native-graphics-function @0x824e6ad0:', result.stderr)
        # First real texture: the original fetch-constant merge of a 256x256 DXT5.
        self.assertRegex(result.stderr, r'NATIVE_TEXTURE_BIND source=0x824f4220 slot=0 texture=0x[0-9a-f]+ mask=0x80000000 '
                                        r'fetch=[0-9a-f,]+ type=2 format=20 endian=1 dimension=1 size=256x256x1 tiled=0 '
                                        r'pitch=8 base=0x[0-9a-f]+ mips=0x0 levels=0\.\.0 packed=0 lr=0x824328e8 state=fetch-retained')
        # Original creators build real shader objects; each maps to one native shader.
        self.assertRegex(result.stderr, r'NATIVE_SHADER_CREATE stage=vertex handle=0x71800000 object=0x[0-9a-f]+ ')
        for address in ('824ebf20', '824ebd08', '824ebb00', '825e8568', '825e85b8'):
            self.assertNotIn(f'STOP native-graphics-function @0x{address}:', result.stderr)
            self.assertNotIn(f'STOP unsupported-function @0x{address}:', result.stderr)

    @unittest.skipUnless(os.environ.get('SFR_IMAGE_DIRECTORY') and os.environ.get('SFR_ASSET_DIRECTORY'),
                         'requires locally prepared game image and extracted assets')
    def test_missing_game_region_does_not_invent_a_console_setting(self):
        result = self.run_native(os.environ['SFR_IMAGE_DIRECTORY'])
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn('NATIVE_GAME_REGION source=unconfigured', result.stderr)
        self.assertNotIn('IMPORT XGetGameRegion source=configured-profile', result.stderr)
        language = re.search(r'xbox_language=(\d+)\n', result.stderr)
        self.assertIsNotNone(language)
        if int(language[1]) >= 7:
            self.assertIn('STOP game-region @0x82acb24c:', result.stderr)

    def test_unknown_game_region_options_fail_before_guest_entry(self):
        for option in ('--game-region=', '--game-region=0xffff', '--game-region=unknown',
                       '--game-region=NTSC-US'):
            for arguments in ([option], ['missing-assets', option]):
                with self.subTest(arguments=arguments):
                    result = subprocess.run([os.environ['SFR_CPU_DIAGNOSTIC'], 'missing-image',
                                             *arguments], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 3, result.stderr)
                    self.assertIn('STOP game-region @0x0:', result.stderr)
                    self.assertNotIn('ENTER ', result.stderr)
        result = subprocess.run([os.environ['SFR_CPU_DIAGNOSTIC'], 'missing-image',
                                 '--game-region=ntsc-us', '--game-region=ntsc-us'],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn('STOP game-region @0x0:', result.stderr)
        self.assertNotIn('ENTER ', result.stderr)
        result = subprocess.run([os.environ['SFR_CPU_DIAGNOSTIC'], 'missing-image', 'missing-assets', ''],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn('STOP game-region @0x0:', result.stderr)
        self.assertNotIn('ENTER ', result.stderr)

    @unittest.skipUnless(os.environ.get('SFR_IMAGE_DIRECTORY'), 'requires locally prepared game image')
    def test_actual_file_request_requires_explicit_asset_mount(self):
        for options in ([], ['--game-region=ntsc-us']):
            with self.subTest(options=options):
                result = subprocess.run([os.environ['SFR_CPU_DIAGNOSTIC'], os.environ['SFR_IMAGE_DIRECTORY'],
                                         *options], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 3, result.stderr)
                self.assertIn('STOP asset-mount @0x82acb58c:', result.stderr)
                self.assertNotIn('RESULT NtCreateFile', result.stderr)

    @unittest.skipUnless(os.environ.get('SFR_IMAGE_DIRECTORY'), 'requires locally prepared game image')
    def test_rejects_missing_or_changed_header_before_guest_execution(self):
        source = Path(os.environ['SFR_IMAGE_DIRECTORY'])
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for name in ('image.bin', 'import_variables.tsv', 'complete.txt'):
                shutil.copyfile(source / name, directory / name)
            for payload in (None, bytes(0x5000), bytes(32)):
                with self.subTest(size=None if payload is None else len(payload)):
                    if payload is not None:
                        (directory / 'xex_header.bin').write_bytes(payload)
                    result = self.run_native(directory)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn('header', result.stderr.lower())
                    self.assertNotIn('ENTER ', result.stderr)

    @unittest.skipUnless(os.environ.get('SFR_IMAGE_DIRECTORY'), 'requires locally prepared game image')
    def test_rejects_changed_import_table_before_guest_execution(self):
        source = Path(os.environ['SFR_IMAGE_DIRECTORY'])
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            shutil.copyfile(source / 'image.bin', directory / 'image.bin')
            (directory / 'complete.txt').write_text('complete')
            table = (source / 'import_variables.tsv').read_text()
            (directory / 'import_variables.tsv').write_text(table.replace('XexExecutableModuleHandle', 'WrongVariable'))
            result = self.run_native(directory)
            self.assertEqual(result.returncode, 1)
            self.assertIn('fingerprint', result.stderr)
            self.assertNotIn('ENTER ', result.stderr)
