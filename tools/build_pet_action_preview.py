#!/usr/bin/env python3
"""Build an offline canvas preview from an explicit pet clip, without screenshots.

Does not invent poses or write runtime pixels. export_pet_clip.py downloads the
browser's 320px canvases using the same shared transform and alpha rule as blink.
"""
import argparse
import base64
import json
from pathlib import Path


HTML = r'''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<title>Pet action preview</title><style>
body{font:16px system-ui;background:#e4f2f4;color:#243746;margin:32px}
.views{display:flex;gap:32px;align-items:end;flex-wrap:wrap}
canvas{background:transparent;image-rendering:auto}
button,select{font:inherit;margin:10px 8px 10px 0;padding:8px 14px}
.small{max-width:800px;font-size:14px;line-height:1.6}
</style><h1 id="title"></h1><p id="state">加载中</p>
<div class="views"><div><p>V1 显示 256px</p><canvas id="actual" width="320" height="320" style="width:256px;height:256px"></canvas></div>
<div><p>运行画布 320px</p><canvas id="detail" width="320" height="320"></canvas></div></div>
<button id="play">暂停</button><button id="once">重播一次</button>
<button id="step">下一帧</button><button id="download">导出当前 320px 帧</button>
<select id="bg"><option value="#e4f2f4">浅蓝</option><option value="#ffffff">白</option><option value="#30363d">深灰</option></select>
<p class="small">只预览资源，不连接模拟器。按住抚摸动作的保持/释放区间见 clip.json；预览自动完整演示进入和退出。
原图保持不变。所有动作共享母版缩放和脚底参考，不逐帧紧裁或拉伸。生成姿态仍需用户观感复核，未接入固件。</p>
<script>
const SPEC=__SPEC__, SOURCES=__SOURCES__;
const canvases=[document.querySelector('#actual'),document.querySelector('#detail')];
let images={},playing=true,frame=0,ready=false,deadline=0;
document.querySelector('#title').textContent=SPEC.action;
function draw(c,index){const ctx=c.getContext('2d');ctx.clearRect(0,0,320,320);const im=images[SPEC.frames[index].source];const s=SPEC.source_to_runtime_scale;const a=SPEC.frames[index].source_anchor_px||SPEC.source_anchor_px;
ctx.imageSmoothingEnabled=true;ctx.imageSmoothingQuality='high';
ctx.drawImage(im,SPEC.anchor_px[0]-a[0]*s,SPEC.anchor_px[1]-a[1]*s,im.width*s,im.height*s);
const d=ctx.getImageData(0,0,320,320),cut=SPEC.alpha_render_cutoff;
for(let i=3;i<d.data.length;i+=4){const a=d.data[i];d.data[i]=a<=cut?0:Math.round((a-cut)*255/(255-cut));}ctx.putImageData(d,0,0);}
function render(){if(!ready)return;canvases.forEach(c=>draw(c,frame));document.querySelector('#state').textContent=(frame+1)+'/'+SPEC.frames.length+' · '+SPEC.frames[frame].expression+' · '+SPEC.frames[frame].duration_ms+'ms';}
function tick(now){if(ready&&playing&&now>=deadline){if(frame+1<SPEC.frames.length){frame++;deadline=now+SPEC.frames[frame].duration_ms;}else{frame=0;deadline=now+SPEC.frames[0].duration_ms+(SPEC.playback==='loop'?0:1200);}render();}requestAnimationFrame(tick);}
document.querySelector('#play').onclick=()=>{playing=!playing;document.querySelector('#play').textContent=playing?'暂停':'继续';deadline=performance.now()+SPEC.frames[frame].duration_ms;};
document.querySelector('#once').onclick=()=>{frame=0;playing=true;deadline=performance.now()+SPEC.frames[0].duration_ms;render();};
document.querySelector('#step').onclick=()=>{playing=false;frame=(frame+1)%SPEC.frames.length;render();};
document.querySelector('#bg').onchange=e=>document.body.style.background=e.target.value;
document.querySelector('#download').onclick=()=>{if(!ready)return;const c=document.createElement('canvas');c.width=c.height=320;draw(c,frame);const a=document.createElement('a');a.download=SPEC.action+'_'+String(frame).padStart(2,'0')+'.png';a.href=c.toDataURL('image/png');a.click();};
Promise.all(Object.entries(SOURCES).map(([name,src])=>new Promise((resolve,reject)=>{const im=new Image();im.onload=()=>{images[name]=im;resolve();};im.onerror=reject;im.src=src;}))).then(()=>{ready=true;deadline=performance.now()+SPEC.frames[0].duration_ms;render();requestAnimationFrame(tick);}).catch(()=>document.querySelector('#state').textContent='图片加载失败');
</script></html>'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('clip_dir', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = args.clip_dir.resolve()
    base = root / 'project_docs/specs/pet/redesign-20261029'
    source.relative_to(base)
    spec = json.loads((source / 'clip.json').read_text())
    assert spec['runtime_canvas'] == [320, 320]
    assert spec['anchor_px'] == [160, 288]
    assert spec['frames'] and all(f['duration_ms'] > 0 for f in spec['frames'])
    images = {}
    for frame in spec['frames']:
        path = (source / frame['source']).resolve()
        path.relative_to(base)
        images[frame['source']] = 'data:image/png;base64,' + base64.b64encode(path.read_bytes()).decode()
    output = source / 'preview.html'
    if output.exists():
        raise SystemExit('Existing preview: refusing overwrite.')
    output.write_text(HTML.replace('__SPEC__', json.dumps(spec, ensure_ascii=False)).replace('__SOURCES__', json.dumps(images)))
    print(f"Preview: {output.relative_to(root)}; {len(spec['frames'])} slots, {sum(f['duration_ms'] for f in spec['frames'])}ms")


if __name__ == '__main__':
    main()
