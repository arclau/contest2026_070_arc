#!/usr/bin/env python3
"""Embed exported pet animation drafts in one offline review page."""
import base64
import hashlib
import json
from pathlib import Path


PAGE = r'''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<title>V1 宠物动画 · 18 动作草稿</title><style>
body{font:16px system-ui;background:#e4f2f4;color:#263a46;margin:28px;line-height:1.6}
main{max-width:1000px;margin:auto}h1{font-size:25px;margin-bottom:4px}
.muted{font-size:14px;color:#566977}select,button{font:inherit;padding:8px 12px;margin:8px 8px 8px 0}
.stage{display:flex;align-items:center;justify-content:center;min-height:350px;border:1px solid #a3b7bf;border-radius:16px;background:#e4f2f4}
canvas{width:256px;height:256px}.checker{background:conic-gradient(#ddd 25%,white 0 50%,#ddd 0 75%,white 0) 0 0/24px 24px!important}
input{width:100%}.warn{background:#fff6da;padding:12px;border-radius:8px}a{color:#176c8c}
</style><main><h1>V1 宠物动画 · 18 动作草稿</h1>
<p class="muted">89 个 320px 帧位 · 34 张去重图 · 原始源帧至少 1024px · 未接入固件</p>
<select id="actions" aria-label="动作"></select><select id="size" aria-label="显示尺寸"><option value="256">显示 256px</option><option value="320">画布 320px</option></select>
<select id="background" aria-label="背景"><option value="#e4f2f4">浅蓝</option><option value="#fff">白</option><option value="#30363d">深灰</option><option value="checker">棋盘</option></select>
<div class="stage" id="stage"><canvas width="320" height="320" id="sprite"></canvas></div>
<p id="state">加载中</p><input id="scrub" type="range" min="0" value="0" aria-label="逐帧">
<button id="play">暂停</button><button id="again">重播</button><button id="previous">上一帧</button><button id="next">下一帧</button>
<select id="speed" aria-label="速度"><option value="1">正常速度</option><option value="0.5">半速</option><option value="2">两倍速度</option></select>
<button id="hold">按住演示抚摸</button>
<p id="detail" class="muted"></p><p class="warn">这是资源审阅页，动作以实有关键姿态构成，原计划帧位预算仍保留。
转身/坐稳与睡眠过渡较粗，走路的步幅和位移尚未在播放器中配合；体型、循环观感及透明边缘仍需复核。
听、想、说只演示图片；尚未绑定真实录音、请求或 TTS。</p>
<p><a href="PRODUCTION.md">制作记录、帧数及待修项</a> · <a href="actions.plan.json">动作清单</a></p></main>
<script>
const DATA=__DATA__,IMAGES=__IMAGES__;
const select=document.querySelector('#actions'),canvas=document.querySelector('#sprite'),ctx=canvas.getContext('2d'),scrub=document.querySelector('#scrub');
let images={},action=0,frame=0,playing=true,ready=false,deadline=0,speed=1,holding=false;
for(let i=0;i<DATA.length;i++){const opt=document.createElement('option');opt.value=i;opt.textContent=DATA[i].label+' · '+DATA[i].frames.length+'帧';select.appendChild(opt);}
function current(){return DATA[action];}
function render(){if(!ready)return;const a=current();ctx.clearRect(0,0,320,320);ctx.drawImage(images[a.frames[frame].image],0,0);scrub.max=a.frames.length-1;scrub.value=frame;
document.querySelector('#state').textContent=a.label+' · '+(frame+1)+'/'+a.frames.length+' · '+a.frames[frame].expression+' · '+a.frames[frame].duration_ms+'ms';
document.querySelector('#detail').textContent='时序 '+a.duration_ms+'ms · '+a.playback+' · '+a.review;
document.querySelector('#hold').hidden=a.id!=='pet_nuzzle';}
function reset(){frame=0;holding=false;deadline=performance.now()+current().frames[0].duration_ms/speed;render();}
function tick(now){if(ready&&playing&&now>=deadline){const a=current();
if(holding&&frame===a.hold_frame){deadline=now+60;}
else if(frame+1<a.frames.length){frame++;deadline=now+a.frames[frame].duration_ms/speed;}
else{frame=0;const gap=a.id==='blink'?3000+Math.random()*4000:(a.playback==='loop'?0:1200);deadline=now+(a.frames[0].duration_ms+gap)/speed;}render();}requestAnimationFrame(tick);}
select.onchange=()=>{action=Number(select.value);reset();};
document.querySelector('#play').onclick=()=>{playing=!playing;document.querySelector('#play').textContent=playing?'暂停':'继续';deadline=performance.now()+current().frames[frame].duration_ms/speed;};
document.querySelector('#again').onclick=()=>{playing=true;document.querySelector('#play').textContent='暂停';reset();};
function step(delta){playing=false;holding=false;document.querySelector('#play').textContent='继续';frame=(frame+delta+current().frames.length)%current().frames.length;render();}
document.querySelector('#previous').onclick=()=>step(-1);document.querySelector('#next').onclick=()=>step(1);
scrub.oninput=()=>{playing=false;holding=false;frame=Number(scrub.value);document.querySelector('#play').textContent='继续';render();};
document.querySelector('#speed').onchange=e=>{speed=Number(e.target.value);deadline=performance.now()+current().frames[frame].duration_ms/speed;};
document.querySelector('#size').onchange=e=>{canvas.style.width=canvas.style.height=e.target.value+'px';};
document.querySelector('#background').onchange=e=>{const stage=document.querySelector('#stage');stage.classList.toggle('checker',e.target.value==='checker');if(e.target.value!=='checker')stage.style.background=e.target.value;};
const hold=document.querySelector('#hold');hold.onpointerdown=e=>{hold.setPointerCapture(e.pointerId);reset();holding=true;playing=true;};
function release(){if(!holding)return;holding=false;if(frame>=current().hold_frame)frame=current().release_start_frame;deadline=performance.now()+current().frames[frame].duration_ms/speed;render();}
hold.onpointerup=release;hold.onpointercancel=release;hold.onlostpointercapture=release;
Promise.all(Object.entries(IMAGES).map(([key,src])=>new Promise((resolve,reject)=>{const im=new Image();im.onload=()=>{images[key]=im;resolve();};im.onerror=reject;im.src=src;}))).then(()=>{ready=true;reset();requestAnimationFrame(tick);}).catch(()=>document.querySelector('#state').textContent='图片加载失败');
</script></html>'''


def main():
    root = Path(__file__).resolve().parents[2]
    base = root / 'project_docs/specs/pet/redesign-20261029'
    plan = json.loads((base / 'actions.plan.json').read_text())
    names = ['坐姿待机', '眨眼', '发现用户', '抚摸回应', '伸懒腰', '入睡', '蜷卧呼吸', '醒来', '吃饭', '舔爪洗脸', '开心', '饥饿请求', '走路', '起身转向', '落稳坐下', '倾听', '思考', '说话']
    data, images = [], {}
    for item, name in zip(plan['actions'], names):
        clip = base / f"batch-{item['batch'].lower()}" / item['id'] / 'clip.json'
        spec = json.loads(clip.read_text())
        entry = {'id': item['id'], 'label': f"{item['batch']} · {name} ({item['id']})", 'playback': item['playback'], 'frames': [], 'review': '关键帧草稿，观感待复核'}
        for key in ('hold_frame', 'release_start_frame'):
            if key in spec:
                entry[key] = spec[key]
        for index, frame in enumerate(spec['frames']):
            raw = (base / 'frames' / item['id'] / f'{index:02d}.png').read_bytes()
            digest = hashlib.sha256(raw).hexdigest()
            images[digest] = 'data:image/png;base64,' + base64.b64encode(raw).decode()
            entry['frames'].append({'image': digest, 'duration_ms': frame['duration_ms'], 'expression': frame['expression']})
        entry['duration_ms'] = sum(f['duration_ms'] for f in entry['frames'])
        data.append(entry)
    target = base / 'preview-all.html'
    if target.exists():
        raise SystemExit('Existing gallery: refusing overwrite.')
    target.write_text(PAGE.replace('__DATA__', json.dumps(data, ensure_ascii=False)).replace('__IMAGES__', json.dumps(images)))
    print(f"Gallery: {target.relative_to(root)}; {len(data)} actions, {sum(len(x['frames']) for x in data)} slots, {len(images)} unique PNGs")


if __name__ == '__main__':
    main()
