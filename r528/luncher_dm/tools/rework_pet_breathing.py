#!/usr/bin/env python3
"""Derive and verify idle breathing from asset 0, without resampling alpha.

RGB in the horizontal y=170..285 strip is translated by an integer 3px.
The top/bottom 10px blend the translated strip with the original. Transparent
pixels and source samples outside the silhouette are left untouched.
Only assets/pet, pet_actions.c and the specified SIM resource copy are updated.
"""
import argparse
import hashlib
import io
import json
import re
from pathlib import Path

import numpy as np
from PIL import Image


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def derive(base, offset):
    y, x = np.indices(base.shape[:2])
    source_y = np.clip(y - offset, 0, base.shape[0] - 1)
    feather = np.minimum(np.clip((y - 170) / 10, 0, 1),
                         np.clip((285 - y) / 10, 0, 1))
    mask = (base[:, :, 3] > 0) & (base[source_y, x, 3] > 0)
    weight = (feather * mask)[:, :, None]
    frame = base.copy()
    frame[:, :, :3] = np.rint(base[:, :, :3] * (1 - weight) +
                              base[source_y, x, :3] * weight).astype(np.uint8)
    return frame


def measure(base, frame):
    delta = np.abs(frame.astype(np.int16) - base.astype(np.int16))
    changed = np.any(delta != 0, axis=2)
    y, x = np.where(changed)
    bbox = [int(x.min()), int(y.min()), int(x.max() + 1), int(y.max() + 1)]
    silhouette, other = base[:, :, 3] > 0, frame[:, :, 3] > 0
    iou = float((silhouette & other).sum() / (silhouette | other).sum())
    strong = delta.sum(axis=2) > 15
    sy, sx = np.where(strong)
    strong_bbox = [int(sx.min()), int(sy.min()), int(sx.max() + 1), int(sy.max() + 1)]
    outside = np.ones(changed.shape, dtype=bool)
    outside[170:286] = False
    outside |= ~silhouette
    assert not np.any(changed & outside)
    assert np.array_equal(base[:, :, 3], frame[:, :, 3])
    assert bbox[2] - bbox[0] >= 180 and strong.sum() >= 3000 and iou > 0.995
    return dict(difference_bbox=bbox, bbox_width=bbox[2] - bbox[0],
                sum_channel_difference_gt15_pixels=int(strong.sum()),
                strong_difference_bbox=strong_bbox, alpha_iou=iou,
                outside_strip_or_silhouette_changed_pixels=int((changed & outside).sum()),
                alpha_changed_pixels=int(np.count_nonzero(delta[:, :, 3])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sim-dir', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    app = Path(__file__).resolve().parents[1]
    assets = app / 'assets/pet'
    manifest = json.loads((assets / 'manifest.json').read_text())
    base = np.array(Image.open(assets / manifest['assets'][0]['file']).convert('RGBA'))
    assert base.shape == (320, 320, 4)
    previous_files = [manifest['assets'][i]['file'] for i in (80, 81)]
    outputs, metrics = {}, []
    for ident, offset in ((80, -3), (81, 3)):
        frame = derive(base, offset)
        stream = io.BytesIO()
        Image.fromarray(frame).save(stream, format='PNG')
        raw = stream.getvalue()
        digest = sha(raw)
        entry = dict(sha256=digest, id=ident, file=digest + '.png', bytes=len(raw))
        manifest['assets'][ident] = entry
        metrics.append(dict(asset=ident, offset_px=offset, **measure(base, frame)))
        for dest in (assets, args.sim_dir):
            outputs[dest / entry['file']] = raw
    idle = next(a for a in manifest['actions'] if a['id'] == 'idle_sit')
    idle['frames'] = [dict(asset=a, duration_ms=d) for a, d in
                      ((0, 1200), (80, 1600), (81, 1200))]
    idle['playback'] = 'loop'
    idle['derivation'] = dict(base_asset=0, strip_y=[170, 285], feather_px=10,
                              offsets_px=[-3, 3], preserve_alpha=True)
    manifest['frame_slots'] = sum(len(a['frames']) for a in manifest['actions'])
    manifest['unique_pngs'] = len(manifest['assets'])
    manifest.pop('revision', None)
    revision = sha(json.dumps(manifest, ensure_ascii=False, sort_keys=True).encode())[:16]
    manifest['revision'] = revision
    for dest in (assets, args.sim_dir):
        outputs[dest / 'manifest.json'] = (json.dumps(manifest, ensure_ascii=False, indent=2) + '\n').encode()
        outputs[dest / 'revision'] = (revision + '\n').encode()
    source = (app / 'pet_actions.c').read_text()
    source = re.sub(r'static const pet_action_frame_t f_idle_sit\[\] = \{.*?\};',
                    'static const pet_action_frame_t f_idle_sit[] = {\n'
                    '    {0, 1200},\n    {80, 1600},\n    {81, 1200},\n};',
                    source, count=1, flags=re.S)
    source = re.sub(r'(const char \*const g_pet_asset_files\[\] = \{\n).*?(\};)',
                    lambda m: m[1] + ''.join('    "%s",\n' % a['file'] for a in manifest['assets']) + m[2],
                    source, count=1, flags=re.S)
    source = re.sub(r'(const char g_pet_resource_revision\[\] = ")[^"]+',
                    lambda m: m[1] + revision, source, count=1)
    outputs[app / 'pet_actions.c'] = source.encode()
    for path, raw in outputs.items():
        if args.check:
            assert path.read_bytes() == raw, str(path)
        else:
            path.write_bytes(raw)
    if not args.check:
        referenced = {a['file'] for a in manifest['assets']}
        for old in previous_files:
            if old not in referenced:
                for dest in (assets, args.sim_dir):
                    (dest / old).unlink(missing_ok=True)
    # Decode actual saved PNGs, and verify all 82 resources and metadata copies.
    for entry in manifest['assets']:
        raw = (assets / entry['file']).read_bytes()
        assert sha(raw) == entry['sha256'] and len(raw) == entry['bytes']
        assert raw == (args.sim_dir / entry['file']).read_bytes()
    for ident in (80, 81):
        saved = np.array(Image.open(assets / manifest['assets'][ident]['file']))
        assert np.array_equal(saved, derive(base, -3 if ident == 80 else 3))
        assert measure(base, saved) == {k: v for k, v in metrics[ident - 80].items()
                                         if k not in ('asset', 'offset_px')}
    report = dict(revision=revision, synced_pngs=len(manifest['assets']), frames=metrics)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
