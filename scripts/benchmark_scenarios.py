"""Stage one isolated, guided desktop benchmark; never change simulation timing."""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time
import uuid


SCENARIOS = {
    '1p': 'Solo controller/keyboard; standard rider; confirm the chosen course and route.',
    '2p': 'Join two-player mode; confirm two active riders and fixed device assignments.',
    'camera': 'Webcam motion; confirm full-body tracking and no controller takeover.',
    'vrm': 'Solo controller/keyboard; confirm the supplied avatar is visible in the race.',
}
EXCLUDED = {'game', 'save', 'settings.ini', 'settings.ini.new', 'game.log',
            'shader-cache', 'pipeline-cache', 'pipeline.bin', 'benchmarks', 'out',
            '.git', '.worktrees'}
PATH_OPTIONS = ('package', 'settings', 'image', 'assets', 'save', 'output', 'protocol',
                'avatar_model', 'pose_model', 'pose_detector', 'pipeline_cache', 'shader_cache')
# Match read_flag/read_number in launcher_settings.cpp. Unsupported values stay
# literal so fallback/default rewrites still invalidate the requested settings.
BOOLEAN_SETTINGS = {'fullscreen', 'vsync', 'audio', 'skip_movies', 'vertex_cache',
                    'gpu_pipeline', 'parallel', 'ui_sounds', 'vulkan', 'touch_controls',
                    'tilt', 'camera_mirror', 'voice', 'camera_debug'}
NUMBER_SETTINGS = {'window_width': range(160, 16385), 'window_height': range(160, 16385),
                   'volume': range(101), 'race_render_every': range(1, 5),
                   'render_scale': (50, 75, 100, 150, 200)}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in PATH_OPTIONS:
        parser.add_argument('--' + name.replace('_', '-'), type=Path,
                            required=name in ('package', 'settings', 'image', 'assets', 'save', 'output'))
    parser.add_argument('--scenario', required=True, choices=SCENARIOS)
    parser.add_argument('--backend', required=True, choices=('vulkan', 'd3d12'))
    parser.add_argument('--render-scale', type=int, default=100, choices=(50, 75, 100, 150, 200))
    parser.add_argument('--camera-device', default='')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--dry-run', action='store_true', help='print plan without checking paths or writing')
    modes.add_argument('--validate', action='store_true', help='check prerequisites without writing or launching')
    return parser.parse_args(argv)


def make_plan(args, environ):
    plan = vars(args).copy()
    for key in PATH_OPTIONS:
        plan[key] = str(plan[key].resolve()) if plan[key] is not None else None
    suffix = '.exe' if os.name == 'nt' else ''
    plan.update(launcher_name='FreeRidersRecompiled' + suffix,
                game_name='sfr_cpu_diagnostic' + suffix, frame_limit=60,
                validation='not-validated', instructions=SCENARIOS[args.scenario],
                removed_inherited_sfr={k: v for k, v in environ.items() if k.upper().startswith('SFR_')})
    return plan


def settings_values(text):
    values = {}
    for line in text.splitlines():
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            key, value = key.strip(), value.strip()
            if key in BOOLEAN_SETTINGS and value in ('0', '1', 'false', 'true'):
                value = '1' if value in ('1', 'true') else '0'
            elif key in NUMBER_SETTINGS and value.isascii() and value.isdecimal():
                significant = value.lstrip('0') or '0'
                if len(significant) <= 5 and int(significant) in NUMBER_SETTINGS[key]:
                    value = str(int(significant))
            values[key] = value
    return values


def scenario_settings(plan, common):
    values = settings_values(common)
    values.update(image_directory=plan['image'], asset_directory=plan['assets'],
                  vulkan='1' if plan['backend'] == 'vulkan' else '0',
                  render_scale=str(plan['render_scale']), race_render_every='1',
                  camera='motion' if plan['scenario'] == 'camera' else 'off',
                  camera_device=plan['camera_device'] if plan['scenario'] == 'camera' else '',
                  camera_debug='0', voice='0',
                  avatar_model=(plan['avatar_model'] or '') if plan['scenario'] == 'vrm' else '',
                  player2_device=values.get('player2_device', 'gamepad') if plan['scenario'] == '2p' else 'off')
    return '# Isolated benchmark scenario settings\n' + ''.join(f'{key}={value}\n' for key, value in values.items())


def require_path(value, label, directory=False):
    if not value or not (Path(value).is_dir() if directory else Path(value).is_file()):
        raise ValueError(f'{label}: expected an existing {"directory" if directory else "file"}: {value}')


def copied_files(root, exclude=()):
    """Reject links/junctions in copied input trees so copies cannot share writable state."""
    root = Path(root)
    pending = [root]
    while pending:
        entry = pending.pop()
        if entry != root and entry.parent == root and entry.name in exclude:
            continue
        if entry.is_symlink() or (entry.stat().st_file_attributes & 0x400 if os.name == 'nt' else False):
            raise ValueError(f'symbolic link or reparse point in copied input: {entry}')
        if entry.is_dir():
            pending.extend(sorted(entry.iterdir(), reverse=True))
        elif entry.is_file():
            yield entry


def validate_plan(plan):
    for key in ('package', 'image', 'assets', 'save'):
        require_path(plan[key], key, directory=True)
    for key in ('settings', 'protocol'):
        require_path(plan[key], key)
    for name in (plan['launcher_name'], plan['game_name'], 'shaders.pack'):
        require_path(Path(plan['package']) / name, name)
    for name in ('complete.txt', 'image.bin'):
        require_path(Path(plan['image']) / name, name)
    require_path(Path(plan['assets']) / 'default.xex', 'default.xex')
    for key in PATH_OPTIONS:
        if plan[key] and any(c in plan[key] for c in '\r\n'):
            raise ValueError(f'newlines are not supported in {key}')
    if plan['backend'] == 'd3d12' and os.name != 'nt':
        raise ValueError('d3d12 requires Windows')
    if plan['scenario'] == 'vrm':
        require_path(plan['avatar_model'], 'avatar model')
    if plan['scenario'] == 'camera':
        require_path(plan['pose_model'], 'pose model')
        require_path(plan['pose_detector'], 'pose detector')
        if not plan['camera_device'] or len(plan['camera_device']) > 128 or any(c in plan['camera_device'] for c in '\r\n'):
            raise ValueError('camera requires an explicit camera device name of 1-128 characters')
    for key in ('avatar_model', 'pose_model', 'pose_detector', 'pipeline_cache', 'shader_cache'):
        if plan[key]:
            require_path(plan[key], key, directory=key == 'shader_cache')
    output = Path(plan['output'])
    for key in PATH_OPTIONS:
        if key != 'output' and plan[key]:
            source = Path(plan[key])
            if output == source or output.is_relative_to(source) or source.is_relative_to(output):
                raise ValueError(f'output must not overlap {key}: {source}')
    for key in ('package', 'save', 'shader_cache'):
        if plan[key]:
            # Exhaust the generator to validate before making output directories.
            for _ in copied_files(plan[key], EXCLUDED if key == 'package' else ()):
                pass
    settings = scenario_settings(plan, Path(plan['settings']).read_text(encoding='utf-8-sig'))
    if plan['scenario'] == '2p' and settings_values(settings)['player2_device'] not in ('keyboard', 'gamepad'):
        raise ValueError('2p requires player2_device=keyboard or gamepad in common settings')
    plan['validation'] = 'paths-validated-runtime-unverified'


def file_record(path):
    path = Path(path)
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest.hexdigest())


def inventory(root, exclude=()):
    root = Path(root)
    return {str(path.relative_to(root)): file_record(path) for path in copied_files(root, exclude)}


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def launch_environment(plan, staged, environ):
    env = {k: v for k, v in environ.items() if not k.upper().startswith('SFR_')}
    env.update(SFR_FRAME_LIMIT='60', SFR_RENDER_EVERY='1', SFR_RENDER_SCALE=str(plan['render_scale']),
               SFR_FRAME_METRICS='1', SFR_TRACE_INPUT='0', SFR_TRACE_GRAPHICS='0',
               SFR_PIPELINE_CACHE_PATH=str(staged / 'pipeline.bin'),
               SFR_RUNTIME_SHADER_CACHE=str(staged / 'shader-cache'),
               SFR_SAVE_DIRECTORY=str(staged / 'save'))
    if plan['scenario'] == 'camera':
        env.update(SFR_POSE_MODEL=plan['pose_model'], SFR_POSE_DETECTOR=plan['pose_detector'])
    return env


def prepare_run(plan):
    validate_plan(plan)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    run = Path(plan['output']) / f'{stamp}-{plan["scenario"]}-{uuid.uuid4().hex[:8]}'
    run.mkdir(parents=True, exist_ok=False)
    staged = run / 'package'
    package = Path(plan['package'])
    shutil.copytree(package, staged,
                    ignore=lambda directory, names: EXCLUDED.intersection(names) if Path(directory) == package else [])
    shutil.copytree(plan['save'], staged / 'save')
    if plan['shader_cache']:
        shutil.copytree(plan['shader_cache'], staged / 'shader-cache')
    else:
        (staged / 'shader-cache').mkdir()
    if plan['pipeline_cache']:
        shutil.copy2(plan['pipeline_cache'], staged / 'pipeline.bin')
    common = Path(plan['settings']).read_text(encoding='utf-8-sig')
    settings = scenario_settings(plan, common)
    shutil.copy2(plan['settings'], run / 'settings.common.ini')
    shutil.copy2(plan['protocol'], run / 'protocol.txt')
    for path in (staged / 'settings.ini', run / 'settings.before.ini'):
        path.write_text(settings, encoding='utf-8')
    metadata = dict(schema=1, status='prepared', plan=plan,
                    launcher=file_record(staged / plan['launcher_name']),
                    game=file_record(staged / plan['game_name']),
                    requested_backend=plan['backend'],
                    internal_resolution=dict(requested_scale_percent=plan['render_scale'], verified=False),
                    frame_limit=60, race_render_every=1,
                    host=dict(system=platform.platform(), machine=platform.machine(),
                              processor=platform.processor(), logical_processors=os.cpu_count()),
                    command=[str(staged / plan['launcher_name'])], cwd=str(staged),
                    environment_scope='controlled launcher environment; child applies settings.ini; verify runtime log',
                    input_files={key: file_record(plan[key]) for key in
                                 ('settings', 'protocol', 'avatar_model', 'pose_model', 'pose_detector', 'pipeline_cache') if plan[key]},
                    package_inventory=inventory(package, EXCLUDED), save_seed_inventory=inventory(plan['save']),
                    shader_cache_seed_inventory=inventory(plan['shader_cache']) if plan['shader_cache'] else {},
                    image_files={name: file_record(Path(plan['image']) / name) for name in ('image.bin', 'complete.txt')},
                    asset_xex=file_record(Path(plan['assets']) / 'default.xex'),
                    asset_directory_note='Referenced read-only by convention; only default.xex is hashed. Keep all disc assets unchanged.')
    write_json(run / 'run.json', metadata)
    return run, metadata


@contextmanager
def exclusive_run(path=None):
    """OS-held lock survives a stale lock file and serializes distinct output roots."""
    path = Path(path) if path is not None else Path(tempfile.gettempdir()) / 'sfr-benchmark-scenarios.lock'
    with path.open('a+b') as lock:
        lock.seek(0, os.SEEK_END)
        if not lock.tell():
            lock.write(b'0')
            lock.flush()
        lock.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError('another benchmark runner is active; finish it before starting a new capture') from error
        try:
            yield
        finally:
            lock.seek(0)
            if os.name == 'nt':
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def capture(plan):
    with exclusive_run():
        print('Staging isolated package and hashing inputs; no game is running yet.', flush=True)
        run, metadata = prepare_run(plan)
        staged = run / 'package'
        environment = launch_environment(plan, staged, os.environ)
        metadata['launcher_sfr_environment'] = {k: v for k, v in environment.items() if k.startswith('SFR_')}
        metadata.update(started_utc=datetime.now(timezone.utc).isoformat(), status='running')
        write_json(run / 'run.json', metadata)
        print(f'{plan["instructions"]}\nFollow {run / "protocol.txt"}. Play once, then close the game and launcher.\nOutput: {run}', flush=True)
        started = time.monotonic()
        try:
            process = subprocess.Popen(metadata['command'], cwd=staged, env=environment)
            while True:
                try:
                    metadata['launcher_exit_code'] = process.wait()
                    break
                except KeyboardInterrupt:
                    print('Close the game and launcher to finish; keeping the capture lock until they exit.', flush=True)
            log = staged / 'game.log'
            metadata['present_rows'] = 0
            metadata['runtime_evidence'] = []
            if log.is_file():
                with log.open(encoding='utf-8', errors='replace') as source:
                    for line in source:
                        if line.startswith('NATIVE_PRESENT '):
                            metadata['present_rows'] += 1
                        if line.startswith(('NATIVE_GRAPHICS ', 'NATIVE_GRAPHICS_FALLBACK ', 'NATIVE_RENDER_SCALE ', 'STOP ', 'RUNTIME_STOP', 'HANG_REPORT', 'Diagnostic error:')):
                            metadata['runtime_evidence'].append(line.rstrip())
                metadata['log'] = file_record(log)
            after = staged / 'settings.ini'
            shutil.copy2(after, run / 'settings.after.ini')
            before_values = settings_values((run / 'settings.before.ini').read_text(encoding='utf-8'))
            after_values = settings_values(after.read_text(encoding='utf-8-sig'))
            metadata['changed_settings'] = {key: dict(before=value, after=after_values.get(key))
                                            for key, value in before_values.items() if after_values.get(key) != value}
            failed_stops = [line for line in metadata['runtime_evidence']
                            if line.startswith(('STOP ', 'RUNTIME_STOP', 'HANG_REPORT', 'Diagnostic error:'))
                            and line.split(maxsplit=2)[:2] != ['STOP', 'window-closed']]
            metadata['normal_child_termination'] = any(
                line.split(maxsplit=2)[:2] == ['STOP', 'window-closed']
                for line in metadata['runtime_evidence'])
            metadata['status'] = ('invalid-launcher-exit' if metadata['launcher_exit_code'] else
                                  'invalid-runtime-stop' if failed_stops else
                                  'invalid-settings-change' if metadata['changed_settings'] else
                                  'invalid-no-frames' if not metadata['present_rows'] else
                                  'invalid-incomplete-log' if not metadata['normal_child_termination'] else
                                  'captured-unverified')
        except (OSError, ValueError):
            metadata['status'] = 'failed'
            raise
        finally:
            metadata.update(ended_utc=datetime.now(timezone.utc).isoformat(), elapsed_seconds=time.monotonic() - started)
            write_json(run / 'run.json', metadata)
        print(f'{metadata["status"]}: {run}', flush=True)
        return 0 if metadata['status'] == 'captured-unverified' else 1


def main(argv=None):
    args = parse_args(argv)
    plan = make_plan(args, os.environ)
    try:
        if args.dry_run or args.validate:
            if args.validate:
                validate_plan(plan)
            print(json.dumps(plan, indent=2, ensure_ascii=False))
            return 0
        return capture(plan)
    except (OSError, ValueError, RuntimeError) as error:
        print(f'Benchmark capture failed: {error}')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
