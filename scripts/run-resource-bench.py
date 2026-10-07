#!/usr/bin/env python3
"""Run the declared software corpus and check tuple-specific regression envelopes."""
import argparse
import csv
import json
import platform
from pathlib import Path
import statistics
import subprocess
import tempfile


def run(binary, count, timeout):
    with tempfile.TemporaryDirectory(prefix='mddlog-resources-') as directory:
        rss_file = Path(directory) / 'rss.txt'
        process = subprocess.run(['/usr/bin/time', '-f', '%M', '-o', str(rss_file),
                                  str(binary.resolve()), str(count)], text=True,
                                 stdout=subprocess.PIPE, timeout=timeout, check=True)
        rows = list(csv.reader(process.stdout.splitlines()))
        if len({row[0] for row in rows}) != len(rows):
            raise ValueError('duplicate measurements')
        return {'peakRssKiB': int(rss_file.read_text().strip()),
                'operations': {name: {'nanoseconds': int(elapsed), 'units': int(units)}
                               for name, elapsed, units in rows}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--profile', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'docs/resource-budgets/linux-x86_64.json')
    parser.add_argument('--count', type=int, default=16384, choices=[4096, 16384, 49152])
    parser.add_argument('--runs', type=int, default=3, choices=range(1, 11))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--host-binary', type=Path)
    parser.add_argument('--service-binary', type=Path)
    parser.add_argument('--baseline', action='store_true', help='record v0.2 without applying the new-reader envelope')
    args = parser.parse_args()
    profile = json.loads(args.profile.read_text())
    runs = [run(args.binary, args.count, profile['measurementEnvelope']['timeoutSeconds'])
            for _ in range(args.runs)]
    result = {'profile': profile['name'], 'platform': platform.platform(),
              'corpusEvents': args.count, 'runs': runs,
              'medianNanoseconds': {name: statistics.median(sample['operations'][name]['nanoseconds']
                                    for sample in runs) for name in runs[0]['operations']}}
    if args.host_binary:
        result['host'] = [run(args.host_binary, args.count, profile['measurementEnvelope']['timeoutSeconds']) for _ in range(args.runs)]
    if args.service_binary:
        result['service'] = [run(args.service_binary, args.count, profile['measurementEnvelope']['timeoutSeconds']) for _ in range(args.runs)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    failures = []
    if not args.baseline:
        for sample in runs:
            if sample['peakRssKiB'] > profile['measurementEnvelope']['peakRssKiB']:
                failures.append('peak RSS exceeded')
            operations = sample['operations']
            for name, bound in profile['measurementEnvelope']['operationNanoseconds'].items():
                if name not in operations or operations[name]['nanoseconds'] > bound:
                    failures.append(f'{name}: missing or exceeded regression envelope')
            if operations['max_read_request']['units'] > profile['limits']['readChunkBytes']:
                failures.append('chunk size exceeded')
            if args.count == 49152 and (operations['rotation']['units'] == 0 or operations['retention']['units'] == 0):
                failures.append('large corpus did not exercise reclamation')
    for sample in result.get('host', []):
        if sample['peakRssKiB'] > 65536:
            failures.append('host peak RSS exceeded')
        if sample['operations']['open_descriptors']['units'] > 40:
            failures.append('file backend descriptor budget exceeded')
        for name in ['context', 'format_context', 'file_startup', 'file_append_sync', 'file_close']:
            if sample['operations'][name]['nanoseconds'] > 10000000000:
                failures.append(f'{name}: host regression envelope exceeded')
    for sample in result.get('service', []):
        if sample['peakRssKiB'] > profile['measurementEnvelope']['peakRssKiB']:
            failures.append('service RSS exceeded')
        for name in ['service_poll_max', 'service_observer_max', 'service_stop']:
            if sample['operations'][name]['nanoseconds'] > 1000000000:
                failures.append(f'{name}: software regression envelope exceeded')
        if sample['operations']['service_total']['nanoseconds'] > 30000000000:
            failures.append('service completion envelope exceeded')
    print(json.dumps({'profile': result['profile'], 'runs': len(runs),
                      'maxRssKiB': max(sample['peakRssKiB'] for sample in runs),
                      'failures': failures}, indent=2))
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
