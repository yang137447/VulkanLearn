"""Export a self-contained inspector for an existing native motion feature cache."""

import argparse
import json
from pathlib import Path


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>GASP Motion Features</title><link rel="icon" href="data:,"/>
<style>
*{box-sizing:border-box}body{margin:0;background:#18191b;color:#ececed;font:14px "Segoe UI",sans-serif}
header{padding:16px 24px;border-bottom:1px solid #45474b}h1{margin:0 0 6px;font-size:22px;letter-spacing:0}
small{color:#b0b4b9;overflow-wrap:anywhere}main{padding:16px 24px;max-width:1440px;margin:auto}
.controls{display:flex;flex-wrap:wrap;gap:12px;align-items:center;margin-bottom:16px}
label{display:flex;gap:8px;align-items:center}select,input{font:inherit;color:inherit;background:#2c2e32;border:1px solid #696d72;border-radius:3px;padding:5px;max-width:100%}
#clip{width:400px}#frame{width:100%;padding:0;accent-color:#e9aa39}#index{width:90px}
.layout{display:grid;grid-template-columns:minmax(280px,1fr) minmax(350px,1fr);gap:24px}
figure{margin:0;min-width:0}canvas{width:100%;aspect-ratio:1;display:block;background:#202225}
figcaption{display:flex;gap:16px;flex-wrap:wrap;padding:10px 0;font-size:12px;color:#c6c9cc}
.trajectory{color:#ffbf45}.joint{color:#9fe666}h2{font-size:16px;margin:0 0 12px}
table{width:100%;border-collapse:collapse;font-size:12px}th,td{padding:6px 4px;border-bottom:1px solid #3d4044;text-align:right;font-variant-numeric:tabular-nums}
th:first-child,td:first-child{text-align:left;overflow-wrap:anywhere;max-width:190px}thead{color:#bbbfc5}
.stats{margin:12px 0;font-variant-numeric:tabular-nums;min-height:40px;line-height:1.5}
@media(max-width:760px){main,header{padding:12px}.layout{grid-template-columns:minmax(0,1fr)}.controls label{width:100%}#clip{width:100%}}
</style></head><body><header><h1>GASP Motion Features</h1><small id="identity"></small></header>
<main><div class="controls"><label>Clip <select id="clip"></select></label><label>Frame <input id="index" type="number" min="0" value="0" step="1"/></label><span id="count"></span></div>
<input id="frame" type="range" min="0" max="1" value="0" step="1" aria-label="Animation frame"/>
<div class="stats" id="status"></div><div class="layout"><figure><canvas id="plot" width="700" height="700"></canvas>
<figcaption><span class="trajectory">Trajectory / facing</span><span class="joint">Pose joints / velocity</span><span>Top view, root heading, cm</span></figcaption></figure>
<section><h2>Feature Channels</h2><table><thead><tr><th>Channel</th><th>Raw</th><th>Normalized</th><th>Weight</th></tr></thead><tbody id="channels"></tbody></table></section></div></main>
<script id="data" type="application/json">__DATA__</script><script>
"use strict";
const cache=JSON.parse(document.getElementById("data").textContent), db=cache.database;
const clip=document.getElementById("clip"), frame=document.getElementById("frame"), index=document.getElementById("index");
const canvas=document.getElementById("plot"), ctx=canvas.getContext("2d"), tbody=document.getElementById("channels");
const members=db.clipIds.map((name,id)=>({name,frames:db.frames.filter(f=>f.clip===id),loop:db.looping[id]}));
document.getElementById("identity").textContent=cache.identity;
for(let i=0;i<members.length;i++){let option=document.createElement("option");option.value=i;option.textContent=members[i].name;clip.append(option)}
const cells=db.channels.map(name=>{const row=document.createElement("tr");for(let i=0;i<4;i++)row.append(document.createElement("td"));row.children[0].textContent=name;tbody.append(row);return row.children});
const fmt=value=>Math.abs(value)<0.00005?"0":Number(value.toFixed(4)).toString();
function render(){
 const member=members[Number(clip.value)], n=member.frames.length;
 const at=Math.max(0,Math.min(n-1,Number(frame.value)||0));
 frame.disabled=index.disabled=!n; frame.max=index.max=Math.max(0,n-1);frame.value=index.value=at;
 document.getElementById("count").textContent=`${n} rows | ${db.channels.length} channels`;
 ctx.clearRect(0,0,700,700);
 if(!n){document.getElementById("status").textContent="No complete nonloop feature windows";for(const row of cells)for(let i=1;i<4;i++)row[i].textContent="--";return}
 const sample=member.frames[at], v=sample.features;
 document.getElementById("status").textContent=`t = ${sample.time.toFixed(4)} s | ${member.loop?"Loop extended":"Nonloop complete window"} | ${db.schema.sampleRate} Hz | pre-IK\nVelocity: backward difference, ${db.schema.velocityStep.toFixed(6)} s`;
 for(let i=0;i<v.length;i++){cells[i][1].textContent=fmt(v[i]);cells[i][2].textContent=fmt((v[i]-db.means[i])/db.scales[i]);cells[i][3].textContent=fmt(db.weights[i])}
 const points=db.schema.trajectoryTimes.map((time,i)=>({x:v[4*i],z:v[4*i+1],dx:v[4*i+2],dz:v[4*i+3],time}));
 const offset=4*points.length;
 const joints=db.schema.joints.map((name,i)=>({name,x:v[offset+6*i],z:v[offset+6*i+2],vx:v[offset+6*i+3],vz:v[offset+6*i+5]}));
 const extent=Math.max(80,...points.flatMap(p=>[Math.abs(p.x),Math.abs(p.z)]),...joints.flatMap(p=>[Math.abs(p.x),Math.abs(p.z)]))*1.2;
 const scale=310/extent, sx=x=>350+x*scale, sy=z=>350-z*scale;
 ctx.lineWidth=1;ctx.strokeStyle="#44484d";
 for(let i=-4;i<=4;i++){let p=350+i*77.5;ctx.beginPath();ctx.moveTo(p,0);ctx.lineTo(p,700);ctx.moveTo(0,p);ctx.lineTo(700,p);ctx.stroke()}
 ctx.fillStyle="#adb3bb";ctx.font="14px Segoe UI";ctx.fillText("+Z",358,22);ctx.fillText("+X",665,342);ctx.fillText(`grid ${(extent/4).toFixed(1)} cm`,12,682);
 ctx.lineWidth=2.5;ctx.strokeStyle="#ffbf45";ctx.beginPath();points.forEach((p,i)=>i?ctx.lineTo(sx(p.x),sy(p.z)):ctx.moveTo(sx(p.x),sy(p.z)));ctx.stroke();
 for(const p of points){const x=sx(p.x),y=sy(p.z);ctx.fillStyle="#ffbf45";ctx.beginPath();ctx.arc(x,y,4,0,2*Math.PI);ctx.fill();ctx.beginPath();ctx.moveTo(x,y);ctx.lineTo(x+p.dx*20,y-p.dz*20);ctx.stroke();ctx.fillText(`${p.time.toFixed(1)}s`,x-48,y+8)}
 ctx.strokeStyle="#9fe666";ctx.fillStyle="#9fe666";
 for(const p of joints){const x=sx(p.x),y=sy(p.z);ctx.beginPath();ctx.arc(x,y,5,0,2*Math.PI);ctx.fill();ctx.fillText(p.name,x+7,y-8);ctx.beginPath();ctx.moveTo(x,y);ctx.lineTo(sx(p.x+p.vx*0.1),sy(p.z+p.vz*0.1));ctx.stroke()}
}
clip.addEventListener("change",()=>{frame.value=0;render()});frame.addEventListener("input",render);
index.addEventListener("input",()=>{frame.value=index.value;render()});render();
</script></body></html>
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output already exists; choose a new HTML path")
    cache = json.loads(args.cache.read_text(encoding="utf-8"))
    db = cache["database"]
    if db["type"] != "motionFeatureDatabase" or db["version"] != 1:
        parser.error("Unsupported native feature database")
    data = json.dumps(cache, ensure_ascii=True, separators=(",", ":"), allow_nan=False)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(PAGE.replace("__DATA__", data.replace("<", "\\u003c")), encoding="utf-8")
    print(f"Browser: {args.output.resolve()} ({len(db['frames'])} rows)")


if __name__ == "__main__":
    main()
