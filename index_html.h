// index_html.h - the web page the ESP32 serves. Edit freely.
#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Lyrics</title>
<style>
:root{--bg:#e8ecf1;--card:#fff;--line:#d3d9e2;--ink:#1a2330;--mut:#667285;--acc:#2f54eb;--oled:#05070a;--px:#cfe6ff}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,-apple-system,sans-serif;max-width:520px;margin-inline:auto}
header{display:flex;align-items:center;justify-content:space-between;margin:2px 0 14px}
h1{font-size:20px;margin:0;font-weight:700}
.chip{font-size:13px;color:var(--mut);display:flex;align-items:center;gap:6px}
.dot{width:9px;height:9px;border-radius:50%;background:#e5484d}
.on .dot{background:#2fb344}
.screen{background:var(--oled);border-radius:10px;padding:14px 12px;margin-bottom:14px;border:3px solid #2b3340}
#cur{font:600 20px/1.35 ui-monospace,Menlo,Consolas,monospace;color:var(--px);min-height:5.4em;display:flex;align-items:center;justify-content:center;text-align:center;word-break:break-word}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-bottom:14px}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px}
label.btn,button{display:inline-block;background:var(--acc);color:#fff;border:0;border-radius:8px;padding:10px 14px;font:600 15px system-ui,sans-serif;cursor:pointer}
label.ghost{background:transparent;color:var(--acc);border:1px solid var(--acc)}
button:focus-visible,label.btn:focus-within,input:focus-visible{outline:3px solid #9db4ff;outline-offset:2px}
input[type=text]{width:100%;padding:10px;margin:0 0 8px;border-radius:8px;border:1px solid var(--line);font-size:16px;color:var(--ink);background:#fff}
input[type=file]{position:absolute;opacity:0;width:1px;height:1px}
input[type=range]{width:100%;accent-color:var(--acc)}
audio{width:100%;margin-top:10px}
.mut{color:var(--mut);font-size:14px}
</style></head><body>
<header><h1>ESP32 Lyrics</h1>
<span class="chip" id="chip"><span class="dot"></span><span id="cs">connecting</span></span></header>

<div class="screen"><div id="cur">Pick a song</div></div>

<div class="card">
<div class="row">
<label class="btn" for="song">Choose song</label><input type="file" id="song" accept="audio/*">
<label class="btn ghost" for="lrcf">Load .lrc file</label><input type="file" id="lrcf" accept=".lrc,.txt">
</div>
<input type="text" id="artist" placeholder="Artist">
<input type="text" id="title" placeholder="Song title">
<div class="row"><button id="find">Find lyrics</button></div>
<div class="mut" id="st">Name files like "Artist - Title.mp3" to fill these in automatically.</div>
<audio id="au" controls></audio>
</div>

<div class="card">
<div class="mut">Sync offset: <b id="ov">0</b> ms. Raise it if the display lags behind the music.</div>
<input type="range" id="off" min="-1000" max="1000" step="50" value="0">
<div class="mut">Keep this page open and the screen on while playing.</div>
</div>

<script>
const $=i=>document.getElementById(i),au=$('au');
let ws,lines=[],idx=-2,lastSend=0,off=0;

const clean=s=>(s||'').normalize('NFD').replace(/[\u0300-\u036f]/g,'')
 .replace(/[\u2018\u2019]/g,"'").replace(/[\u201c\u201d]/g,'"').replace(/[\u2013\u2014]/g,'-')
 .replace(/[^\x20-\x7E]/g,'').replace(/\s+/g,' ').trim();
const st=m=>$('st').textContent=m;
const put=(k,v)=>{try{localStorage.setItem(k,v)}catch(e){}};
const get=k=>{try{return localStorage.getItem(k)}catch(e){return null}};
const key=()=>'lrc:'+($('artist').value+'|'+$('title').value).toLowerCase().trim();

// ---- Hindi (Devanagari) -> Roman letters, because the OLED font is Latin-only ----
const CON=['k','kh','g','gh','n','ch','chh','j','jh','n','t','th','d','dh','n','t','th','d','dh','n','n','p','ph','b','bh','m','y','r','r','l','l','l','v','sh','sh','s','h'];
const NUK=['q','kh','g','z','r','rh','f','y'];
const NMAP={k:'q',j:'z',d:'r',dh:'rh',ph:'f'};
const VOW=['a','a','a','i','i','u','u','ri','li','e','e','e','ai','o','o','o','au'];
const MAT=['a','i','i','u','u','ri','ri','e','e','e','ai','o','o','o','au'];
function dev(w){
 const U=[];
 for(let i=0;i<w.length;i++){
  const c=w.charCodeAt(i),p=U[U.length-1];
  if(c>=0x915&&c<=0x939){
   if(c===0x91E&&p&&p.c==='j'&&p.v===''){p.c='g';U.push({c:'y',v:'a',h:1})}
   else U.push({c:CON[c-0x915],v:'a',h:1});
  }
  else if(c>=0x958&&c<=0x95F)U.push({c:NUK[c-0x958],v:'a',h:1});
  else if(c===0x93C){if(p&&p.c)p.c=NMAP[p.c]||p.c}
  else if(c>=0x93E&&c<=0x94C){if(p&&p.h){p.v=MAT[c-0x93E];p.h=0}else U.push({c:'',v:MAT[c-0x93E],h:0})}
  else if(c===0x94D){if(p&&p.h){p.v='';p.h=0}}
  else if(c>=0x904&&c<=0x914)U.push({c:'',v:VOW[c-0x904],h:0});
  else if(c>=0x901&&c<=0x903){const t=c===0x903?'h':'n';if(p){p.v+=t;p.h=0}else U.push({c:'',v:t,h:0})}
  else if(c>=0x966&&c<=0x96F)U.push({c:'',v:''+(c-0x966),h:0});
  else if(c===0x964||c===0x965||c===0x93D){}
  else U.push({c:'',v:w[i],h:0});
 }
 const n=U.length;
 if(n>1&&U[n-1].h&&U[n-1].c)U[n-1].v='';              // drop final schwa: tum, not tuma
 for(let i=1;i<n-1;i++){                                // drop medial schwa: samajh
  const u=U[i],a=U[i-1],b=U[i+1];
  if(u.h&&u.c&&a.v!==''&&!/n$/.test(a.v)&&b.c&&b.v!=='')u.v='';
 }
 return U.map(u=>u.c+u.v).join('');
}
const translit=s=>/[\u0900-\u097F]/.test(s)?s.replace(/[\u0900-\u097F]+/g,m=>dev(m)):s;

function send(s){if(ws&&ws.readyState===1)ws.send(s)}
function sendTitle(){
 const t=clean(translit($('artist').value+' - '+$('title').value)).replace(/^- |-$/g,'').trim();
 send('T|'+t);
}
function connect(){
 ws=new WebSocket('ws://'+location.host+'/ws');
 ws.onopen=()=>{$('chip').classList.add('on');$('cs').textContent='display connected';sendTitle();idx=-2;tick()};
 ws.onclose=()=>{$('chip').classList.remove('on');$('cs').textContent='reconnecting...';setTimeout(connect,1500)};
}
connect();

// ---- LRC ----
function parseLRC(txt){
 const out=[];
 for(const ln of txt.split(/\r?\n/)){
  const re=/\[(\d+):(\d+(?:[.:]\d+)?)\]/g,ts=[];let m;
  while((m=re.exec(ln)))ts.push((+m[1]*60+parseFloat(m[2].replace(':','.')))*1000);
  if(!ts.length)continue;
  const x=clean(translit(ln.replace(/\[[^\]]*\]/g,'').replace(/<[^>]*>/g,'')));
  ts.forEach(t=>out.push({t,x}));
 }
 return out.sort((a,b)=>a.t-b.t);
}
function useLRC(txt,src){
 const l=parseLRC(txt);
 if(!l.length){st('No timestamps found in those lyrics.');return false}
 lines=l;idx=-2;st(lines.length+' lines loaded ('+src+')');sendTitle();tick();return true;
}
async function findLyrics(){
 const a=$('artist').value.trim(),t=$('title').value.trim();
 if(!t){st('Enter a song title first.');return}
 const c=get(key());
 if(c&&useLRC(c,'saved on this phone'))return;
 st('Searching LRCLIB...');
 const q=encodeURIComponent;
 try{
  const d=Math.round(au.duration)||0;
  let r=await fetch('https://lrclib.net/api/get?artist_name='+q(a)+'&track_name='+q(t)+(d?'&duration='+d:''));
  let j=r.ok?await r.json():null;
  if(!j||!j.syncedLyrics){
   r=await fetch('https://lrclib.net/api/search?q='+q((a+' '+t).trim()));
   const arr=r.ok?await r.json():[];
   j=arr.find(x=>x.syncedLyrics)||null;
  }
  if(j&&j.syncedLyrics){put(key(),j.syncedLyrics);useLRC(j.syncedLyrics,'LRCLIB')}
  else st('No synced lyrics found. Check the spelling or load a .lrc file.');
 }catch(e){st('Could not reach LRCLIB. Check the phone has internet, or load a .lrc file.')}
}

// ---- sync ----
function find(t){
 let lo=0,hi=lines.length-1,r=-1;
 while(lo<=hi){const m=(lo+hi)>>1;if(lines[m].t<=t){r=m;lo=m+1}else hi=m-1}
 return r;
}
function sendLine(t,i){
 const s=i<0?0:lines[i].t,endAbs=(au.duration||0)*1000;
 const e=i+1<lines.length?lines[i+1].t:(endAbs>s?endAbs:s+4000);
 const x=i<0?'':lines[i].x;
 send('L|'+i+'|'+Math.max(0,Math.round(t-s))+'|'+Math.max(300,Math.round(e-s))+'|'+x);
 $('cur').textContent=x||'...';
}
function tick(force){
 if(au.paused&&!force)return;
 if(!au.src)return;
 if(!lines.length){
  if(idx!==-1||force){idx=-1;send('L|-1|'+Math.round(au.currentTime*1000)+'|600000|')}
  return;
 }
 const t=au.currentTime*1000+off,i=find(t),n=performance.now();
 if(force||i!==idx||n-lastSend>2000){idx=i;lastSend=n;sendLine(t,i)}
}
setInterval(()=>tick(),40);
au.onplay=()=>{idx=-2;tick()};
au.onpause=()=>send('P');
au.onended=()=>send('S');
au.onseeked=()=>{idx=-2;tick(true);if(au.paused)send('P')};

// ---- inputs ----
$('song').onchange=e=>{
 const f=e.target.files[0];if(!f)return;
 au.src=URL.createObjectURL(f);lines=[];idx=-2;
 const n=f.name.replace(/\.[^.]+$/,'').replace(/_/g,' '),p=n.split(/\s+-\s+/);
 $('artist').value=p.length>1?p[0]:'';
 $('title').value=p.length>1?p.slice(1).join(' - '):n;
 sendTitle();st('Loading song...');
 au.addEventListener('loadedmetadata',findLyrics,{once:true});
};
$('find').onclick=()=>{sendTitle();findLyrics()};
$('lrcf').onchange=async e=>{
 const f=e.target.files[0];if(!f)return;
 const txt=await f.text();
 if(useLRC(txt,'file'))put(key(),txt);
};
$('off').oninput=e=>{off=+e.target.value;$('ov').textContent=off};
</script></body></html>)rawliteral";
