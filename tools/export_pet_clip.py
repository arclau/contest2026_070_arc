#!/usr/bin/env python3
"""Export a local pet preview's canvas frames, without screenshots or emulator."""
import argparse
from pathlib import Path

from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('clip_dir', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = args.clip_dir.resolve()
    source.relative_to(root / 'project_docs/specs/pet')
    import json
    spec = json.loads((source / 'clip.json').read_text())
    target = source.parents[1] / 'frames' / spec['action']
    target.mkdir(parents=True, exist_ok=True)
    outputs = [target / f'{i:02d}.png' for i in range(len(spec['frames']))]
    if any(path.exists() for path in outputs):
        raise SystemExit('Existing frame output: refusing overwrite.')
    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        page = browser.new_page(accept_downloads=True)
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto((source / 'preview.html').as_uri())
        page.wait_for_function('ready === true')
        page.evaluate('playing=false')
        for index, dst in enumerate(outputs):
            page.evaluate('(i)=>{frame=i;render();}', index)
            with page.expect_download() as download:
                page.click('#download')
            download.value.save_as(dst)
        if errors:
            raise SystemExit(f'Preview JavaScript errors: {len(errors)}')
        browser.close()
    print(f'Exported {len(outputs)} canvas PNGs to {target.relative_to(root)}; no screenshots.')


if __name__ == '__main__':
    main()
