#!/usr/bin/env python3
"""Exercise new worker controls through the actual persistent Python/native pipe."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from catalog_client import CatalogProcess


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--evidence-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--transport', choices=('jsonl', 'binary'), default='jsonl')
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit('Refusing to overwrite a previous protocol record')
    suite_path = args.evidence_source / 'baseline_suite.json'
    if not suite_path.exists():
        suite_path = args.evidence_source / 'suite.json'
    suite = json.loads(suite_path.read_text())
    project = suite['projects'][0]
    files = sorted((item for item in suite['source_files'] if item['project'] == 0),
                   key=lambda item: item['source_relative'].encode('utf-8'))
    expected = []
    for row, item in enumerate(files):
        data = (args.evidence_source / item['copied_relative']).read_bytes()
        assert sha(args.evidence_source / item['copied_relative']) == item['sha256']
        if len(data) > 1024:
            expected.append({'row': row, 'path': item['source_relative'], 'data': data})
    records = []
    for strategy in ('capture', 'recompute'):
        options = ['--population', strategy, '--max-worker-bytes', str(16 << 20),
                   '--max-worker-entries', '1']
        with CatalogProcess(args.binary, native_options=options, transport=args.transport) as runtime:
            runtime.open(project['name'], args.evidence_source / project['catalog_directory'], expected_revision=0)
            program = 'input size 0 0\noutput size > 1024\n'
            populated, rows = runtime.run(project['name'], program, cache='populate')
            assert rows == expected and populated['cache_stored'] and populated['worker_retained']
            before = runtime.stats()['stats']
            runtime.clear('workers')
            cleared = runtime.stats()['stats']
            assert before['worker_entries'] == 1 and cleared['worker_entries'] == 0
            assert cleared['plan_worker_bytes'] == 0
            assert before['plan_entries'] == cleared['plan_entries'] == 1
            assert before['result_entries'] == cleared['result_entries'] == 1
            hit, rows = runtime.run(project['name'], program)
            assert rows == expected and hit['cache_hit'] and not hit['worker_reused']
            assert runtime.stats()['stats']['worker_entries'] == 0
            runtime.clear('results')
            rebuilt, rows = runtime.run(project['name'], program, cache='bypass')
            assert rows == expected and rebuilt['plan_reused'] and not rebuilt['worker_reused']
            assert rebuilt['worker_retained'] and runtime.stats()['stats']['worker_entries'] == 1
            records.append({'strategy': strategy, 'populated': populated, 'before': before,
                            'cleared': cleared, 'cache_hit': hit, 'rebuilt': rebuilt})
    result = {'status': 'passed', 'transport': args.transport, 'strategies': len(records), 'complete_output_checks': 6,
              'rows_per_check': len(expected), 'bytes_per_check': sum(len(item['data']) for item in expected),
              'binary_sha256': sha(args.binary), 'client_sha256': sha(ROOT / 'scripts/catalog_client.py'),
              'test_sha256': sha(__file__), 'records': records,
              'scope': 'Actual pipe protocol; worker-only clear preserves prepared query and complete result, then recreates worker on miss.'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result[key] for key in ('status', 'strategies', 'complete_output_checks')}))


if __name__ == '__main__':
    main()
