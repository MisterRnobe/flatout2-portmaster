#!/usr/bin/env python3
"""Summarize startup evidence without treating sparse log gaps as CPU samples."""
import argparse
import json
from pathlib import Path
import re

TIMED = re.compile(r'\[t\+([\d.]+)s\]\s*(.*)')
SURFACE = re.compile(r'\[FO2 STARTUP D3D\] (enter|leave) token=([0-9a-f]+) object=([0-9a-f]+) (.*)')
PRESENT = re.compile(r'first Metal present ([\d.]+) seconds')
PIPELINE = re.compile(r'\[FO2 PIPELINE\] frame=0 new state ([\d.]+)ms')


def summarize(text):
    first_present = None
    pipeline_ms = 0.0
    events, calls, locked, spans = {}, [], {}, []
    gaps = []
    previous = None
    for line in text.splitlines():
        present = PRESENT.search(line)
        if present:
            first_present = float(present[1])
            break
        pipeline = PIPELINE.search(line)
        if pipeline:
            pipeline_ms += float(pipeline[1])
        timed = TIMED.search(line)
        if not timed:
            continue
        seconds, message = float(timed[1]), timed[2]
        # Other threads (notably audio) timestamp their own messages too.
        # Gaps below describe log coverage, not CPU usage or a stalled thread.
        if previous and seconds >= previous[0]:
            gaps.append({'seconds': round(seconds-previous[0], 3),
                         'after': previous[1], 'before': message})
        previous = (seconds, message)
        event = SURFACE.search(message)
        if not event:
            continue
        phase, token, obj, fields = event.groups()
        key = (token, obj)
        values = dict(item.split('=', 1) for item in fields.split() if '=' in item)
        if phase == 'enter':
            events[key] = (seconds, values)
            if token == '7e0050e0' and obj in locked:
                start, caller = locked.pop(obj)
                if seconds >= start:
                    spans.append({'object': obj, 'lock_caller': caller,
                                  'unlock_caller': values.get('caller'),
                                  'guest_interval_seconds': round(seconds-start, 3)})
        else:
            begin = events.pop(key, None)
            if begin is None:
                continue
            calls.append({'token': token, 'object': obj,
                          'caller': begin[1].get('caller'),
                          'result': values.get('result'),
                          'bridge_ms': float(values['bridge_ms'])})
            if token == '7e0050d0' and int(values.get('result', '80000000'), 16) < 0x80000000:
                locked[obj] = (seconds, begin[1].get('caller'))
    return {'first_present_renderer_seconds': first_present,
            'logged_slow_frame_zero_pipeline_ms': round(pipeline_ms, 3),
            'surface_bridge_calls': calls,
            'surface_guest_intervals': sorted(spans, key=lambda x: x['guest_interval_seconds'], reverse=True),
            'largest_sparse_log_gaps': sorted(gaps, key=lambda x: x['seconds'], reverse=True)[:5],
            'unpaired_surface_entries': len(events),
            'note': 'Renderer and runtime timestamps have different origins. Pipeline totals include only logged slow states. Guest intervals are wall time between successful lock return and unlock entry, not CPU samples; concurrent intervals must not be summed.'}


def session_report(folder):
    report = summarize((folder/'play.log').read_text(errors='replace'))
    report['session'] = str(folder)
    status = folder/'scenario-status.json'
    if status.exists():
        report['scenario_status'] = json.loads(status.read_text())
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = json.dumps(session_report(args.session), indent=2, ensure_ascii=False)+'\n'
    if args.output:
        args.output.write_text(report)
    else:
        print(report, end='')
