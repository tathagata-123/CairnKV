#!/usr/bin/env python3
"""Compile and run the same workload for CairnKV and an installed LevelDB."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', default='build/python')
parser.add_argument('--compiler', default='g++')
parser.add_argument('--quick', action='store_true')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
build = (repo / args.build_dir).resolve()
build.mkdir(parents=True, exist_ok=True)
source = repo / 'tools/compare_leveldb.cpp'
flags = [args.compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread']
commands = {
    'cairnkv': flags + ['-I' + str(repo / 'include'), str(source), str(build / 'libminilsm.a'), '-o', str(build / 'compare_cairnkv')],
    'leveldb': flags + ['-DCAIRNKV_USE_LEVELDB', str(source), '-lleveldb', '-o', str(build / 'compare_leveldb')],
}
summary = {}
for engine, command in commands.items():
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (build / (engine + '-comparison-build.log')).write_text(result.stdout)
    if result.returncode:
        summary[engine] = {'status': 'not_built', 'reason': 'See ' + engine + '-comparison-build.log; LevelDB requires its development headers/library.'}
        continue
    result = subprocess.run([str(build / ('compare_' + engine))] + (['--quick'] if args.quick else []),
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
    (build / (engine + '-comparison.jsonl')).write_text(result.stdout)
    (build / (engine + '-comparison.stderr')).write_text(result.stderr)
    summary[engine] = {'status': 'measured' if result.returncode == 0 else 'run_failed', 'exit_code': result.returncode}
(build / 'comparison-status.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
raise SystemExit(any(r['status'] != 'measured' for r in summary.values()))
