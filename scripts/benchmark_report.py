#!/usr/bin/env python3
"""Makes a report of a benchmark folder to hand to someone else.

    py scripts\\benchmark_report.py out\\bench\\20261001-194227-shareable --machine "My Laptop" --notes notes.md

Writes <folder>-report beside it, and a .zip of it:

- report.md: the machine, the method, the runs that count in a table, the runs
  that were left out and why, the paired comparison, then the notes you give.
- runs.csv: one row per run that counts.
- frames/<run>.csv: every measured race frame of those runs (time, draws, what
  the main thread held), small enough to send where the 17 MB logs are not.

Give it a folder that scripts\\anonymize_benchmark.py made: the report copies
what info.txt says, and the logs are not included.
"""
import argparse
import csv
import shutil
import sys
from pathlib import Path

import benchmark_summary as bench

FRAME_COLUMNS = ('ms', 'draws', 'draw_ms', 'main_held_ms', 'main_queued_ms', 'main_blocked_ms', 'pipelines', 'pipeline_ms')
RUN_COLUMNS = ('fps', 'median_ms', 'p95_ms', 'p99_ms', 'main_held_ms', 'main_queued_ms', 'draw_ms', 'slow50', 'slow80',
               'hitch30', 'pipelines', 'frames')


def read_info(folder):
    info = {}
    path = Path(folder) / 'info.txt'
    if path.exists():
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            key, _, value = line.partition('=')
            if value:
                info[key.strip()] = value.strip()
    return info


def build(folder, destination, machine, notes, skip, list_left_out=True):
    rows = []
    for log in sorted(Path(folder).glob('*.log')):
        config, _, repeat = log.stem.rpartition('-')
        if config and repeat.isdigit():
            frames, seen, ended = bench.read_run(log, skip)
            rows.append((config, int(repeat), frames, seen, ended, log))
    summary_rows = [(c, r, bench.describe(f) if f else None, e) for c, r, f, _, e, _ in rows]
    good, problems = bench.valid_runs(summary_rows)
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'frames').mkdir(exist_ok=True)

    with open(destination / 'runs.csv', 'w', newline='', encoding='utf-8') as out:
        writer = csv.writer(out)
        writer.writerow(('config', 'round') + RUN_COLUMNS)
        for config, repeat, stats in good:
            writer.writerow((config, repeat) + tuple(round(stats[c], 3) for c in RUN_COLUMNS))
    kept = {(config, repeat) for config, repeat, _ in good}
    for config, repeat, frames, _, _, _ in rows:
        if (config, repeat) not in kept:
            continue
        with open(destination / 'frames' / f'{config}-{repeat}.csv', 'w', newline='', encoding='utf-8') as out:
            writer = csv.writer(out)
            writer.writerow(('race_frame',) + FRAME_COLUMNS)
            for index, frame in enumerate(frames, 1):
                writer.writerow((index,) + tuple(round(frame[c], 3) for c in FRAME_COLUMNS))

    info = read_info(folder)
    lines = [f'# Benchmark 報告：{machine or info.get("model", "（機型未註明）")}', '', '## 機器與環境', '',
             '| 項目 | 內容 |', '| --- | --- |']
    for label, key in (('CPU', 'cpu'), ('GPU', 'gpu'), ('驅動', 'driver'), ('作業系統', 'os'), ('電源', 'power'),
                       ('commit', 'commit'), ('冷 pipeline 快取', 'cold_pipelines')):
        if info.get(key):
            lines.append(f'| {label} | {info[key]} |')
    if machine:
        lines.insert(lines.index('| --- | --- |') + 1, f'| 機型 | {machine} |')
    lines += ['', '## 方法', '',
              '- 遊戲自己從開機走到 Free Race，沒人操作，比賽中由 AI 對手陪跑，跑到第 15600 次 present 自動停止；'
              '不封頂（量的是每格成本，不是封頂 60 fps 下的體驗）。',
              '- 同一個執行檔、同一份存檔，設定之間只差一個環境變數；各設定輪流跑，所以同一輪的幾趟處於相近的機器狀態。',
              f'- 只算比賽中的格，並去掉開頭的 {skip} 格（倒數與第一次使用的著色器）。一格的時間是它與前一次 present 的間隔。',
              '- 沒有 Kinect、沒有聲音、沒有影片、單人；一個賽道一組選單。', '',
              '## 納入比較的各趟', '',
              '| 設定 | 輪 | 平均 fps | 中位數 ms | P95 ms | P99 ms | 主執行緒持有 ms | 繪製 ms | >50 ms 格 | 編譯卡頓格 | 現場建 pipeline |',
              '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for config, repeat, stats in sorted(good, key=lambda g: (g[0], g[1])):
        lines.append(f"| {config} | {repeat} | {stats['fps']:.1f} | {stats['median_ms']:.1f} | {stats['p95_ms']:.1f} | "
                     f"{stats['p99_ms']:.1f} | {stats['main_held_ms']:.1f} | {stats['draw_ms']:.1f} | {stats['slow50']} | "
                     f"{stats['hitch30']} | {stats['pipelines']} |")
    if problems and list_left_out:
        lines += ['', '## 沒有納入的趟', ''] + [f'- {p}' for p in problems]
    # Without the list, the comparison sees only the runs that count.
    compared = summary_rows if list_left_out else [row for row in summary_rows if (row[0], row[1]) in kept]
    lines += ['', '## 比較', '', bench.compare(compared)]
    if notes:
        lines += ['', Path(notes).read_text(encoding='utf-8').rstrip()]
    lines += ['', '## 附檔', '',
              '- `runs.csv`：上表（納入的趟）。',
              '- `frames/<設定>-<輪>.csv`：這些趟每一個量到的比賽格（欄位：時間、draw 數、繪製、主執行緒持有／排隊／阻塞、'
              '該格現場建的 pipeline 數與耗時）。',
              '- 原始 log（每趟約 17 MB）未附；需要時可以再提供。']
    (destination / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return len(good), len(problems)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('folder')
    parser.add_argument('--machine', default='', help='the computer model, for the title')
    parser.add_argument('--notes', default='', help='a markdown file of conclusions to put at the end')
    parser.add_argument('--skip', type=int, default=600)
    parser.add_argument('--hide-left-out', action='store_true', help='do not list the runs that were left out')
    args = parser.parse_args(argv[1:])
    source = Path(args.folder)
    destination = source.with_name(source.name + '-report')
    if destination.exists():
        shutil.rmtree(destination)
    kept, left_out = build(source, destination, args.machine, args.notes or None, args.skip, not args.hide_left_out)
    archive = shutil.make_archive(str(destination), 'zip', destination)
    print(f'{kept} runs in the report, {left_out} left out')
    print(f'Folder: {destination}')
    print(f'Zip:    {archive}')


if __name__ == '__main__':
    main(sys.argv)
