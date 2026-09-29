#!/usr/bin/env python3
"""Dependency-free fallback build, test and benchmark runner (C++20 + ar)."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shlex
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', default='build/python')
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--mode', choices=['debug', 'release'], default='release')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--sanitize', action='store_true', help='Address + Undefined sanitizers')
    parser.add_argument('--tsan', action='store_true', help='Thread sanitizer, separately from ASan')
    parser.add_argument('--filter', default='', help='Only run tests whose name contains this text')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--bench', action='store_true', help='Also run quick benchmark workloads')
    args = parser.parse_args()
    if args.sanitize and args.tsan:
        parser.error('--sanitize and --tsan must use separate builds')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    repo = Path(__file__).resolve().parents[1]
    build = (repo / args.build_dir).resolve()
    build.mkdir(parents=True, exist_ok=True)
    optimization = '-O1' if args.sanitize or args.tsan else ('-O0' if args.mode == 'debug' else '-O3')
    flags = [args.compiler, '-std=c++20', optimization,
             '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread',
             '-I' + str(repo / 'include'), '-I' + str(repo / 'src'), '-I' + str(repo / 'tests')]
    if args.sanitize or args.tsan:
        flags += ['-fno-omit-frame-pointer', '-fsanitize=' + ('thread' if args.tsan else 'address,undefined')]
    results = []

    def run(command, name, timeout=180):
        start = time.monotonic()
        try:
            result = subprocess.run(command, cwd=repo, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=timeout)
            code, output = result.returncode, result.stdout
        except subprocess.TimeoutExpired as error:
            code = 124
            raw = error.stdout or b''
            output = (raw.decode(errors='replace') if isinstance(raw, bytes) else raw) + '\nTIMEOUT\n'
        except OSError as error:
            code, output = 127, str(error)
        (build / (name + '.log')).write_text(shlex.join(command) + '\n' + output)
        return {'name': name, 'command': command, 'exit_code': code,
                'seconds': round(time.monotonic() - start, 3), 'log': name + '.log'}

    def compile_source(path):
        obj = build / (str(path.relative_to(repo)).replace('/', '_') + '.o')
        return run(flags + ['-c', str(path), '-o', str(obj)], obj.stem)

    sources = sorted((repo / 'src').rglob('*.cpp'))
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results += list(pool.map(compile_source, sources))
    if not any(r['exit_code'] for r in results):
        objects = [str(build / (str(p.relative_to(repo)).replace('/', '_') + '.o')) for p in sources]
        archive = build / 'libminilsm.a'
        # Recreate the archive so deleted source files cannot leave stale members.
        archive.unlink(missing_ok=True)
        results.append(run(['ar', 'rcs', str(archive)] + objects, 'archive'))
    if not any(r['exit_code'] for r in results):
        targets = sorted((repo / 'tests').glob('*_test.cpp'))
        targets += [p for p in sorted((repo / 'tools').glob('*.cpp')) if p.stem != 'compare_leveldb']

        def link(path):
            executable = build / path.stem
            result = run(flags + [str(path), str(build / 'libminilsm.a'), '-o', str(executable)],
                         path.stem + '-build')
            if result['exit_code'] == 0:
                executable.chmod(executable.stat().st_mode | 0o111)
            return result

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results += list(pool.map(link, targets))
        if not args.build_only and not any(r['exit_code'] for r in results):
            for path in targets:
                if path.stem.endswith('_test') and args.filter in path.stem:
                    result = run([str(build / path.stem)], path.stem + '-run')
                    results.append(result)
                    print(f"{path.stem}: {'PASS' if not result['exit_code'] else 'FAIL'} ({result['seconds']}s)", flush=True)
            if args.bench:
                for name in ['cairnkv_bench', 'cairnkv_experiments']:
                    if (build / name).exists():
                        results.append(run([str(build / name), '--quick'], name + '-quick', timeout=600))
    (build / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    failures = [r for r in results if r['exit_code']]
    for failure in failures:
        print(f"FAILED {failure['name']}: {build / failure['log']}")
    print(f"Results: {build / 'results.json'}")
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
