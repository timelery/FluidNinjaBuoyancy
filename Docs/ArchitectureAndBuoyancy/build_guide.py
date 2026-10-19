from pathlib import Path
import re, json, html, hashlib, argparse, os

HERE = Path(__file__).resolve().parent
def find_plugin_root():
    parser = argparse.ArgumentParser(description='Build the standalone FluidNinja architecture and buoyancy guide.')
    parser.add_argument('--plugin-root', type=Path, default=os.environ.get('FNB_PLUGIN_ROOT'),
                        help='Root of the FluidNinjaBuoyancy plugin source package.')
    args = parser.parse_args()
    candidates = [Path(args.plugin_root)] if args.plugin_root else []
    for base in (HERE, *HERE.parents, Path.cwd()):
        candidates.extend((base, base / 'Plugins/FluidNinjaBuoyancy'))
    for candidate in candidates:
        if (candidate / 'Source/FluidNinjaBuoyancy/Public/FluidNinjaBuoyancyComponent.h').is_file():
            return candidate.resolve()
    parser.error('Plugin source was not found. Pass --plugin-root or set FNB_PLUGIN_ROOT.')

PLUGIN = find_plugin_root()
SOURCES = {
 'S1': ('Shared buoyancy component', 'Source/FluidNinjaBuoyancy/Public/FluidNinjaBuoyancyComponent.h'),
 'S2': ('Hull construction and hydrodynamic integration', 'Source/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyCore.cpp'),
 'S3': ('Physics callback and stable steps', 'Source/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyCallback.cpp'),
 'S4': ('Surface capture and runtime orchestration', 'Source/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancySubsystem.cpp'),
 'S5': ('Global settings and wave controls', 'Source/FluidNinjaBuoyancy/Public/FluidNinjaBuoyancySettings.h'),
 'S6': ('Wave coupling and self fields', 'Source/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyWaves.cpp'),
 'S7': ('Wave step shader', 'Tools/CorePatch/FluidNinjaWaves/Ninja_WaveStep.hlsl'),
 'S8': ('Wave export shader', 'Tools/CorePatch/FluidNinjaWaves/Ninja_WaveExport.hlsl'),
 'S9': ('Core patch tooling', 'Tools/CorePatch/apply_fluidninja_waves.py'),
 'S10': ('Relative tracking design', 'Docs/RelativeTrackingProposal.md'),
 'S11': ('Preset definitions', 'Tools/make_presets.py'),
 'S12': ('Preset application', 'Source/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyPresetLibrary.cpp'),
 'S13': ('Unified water query', 'Source/FluidNinjaBuoyancy/Public/FluidNinjaBuoyancyWaterQuery.h'),
 'S14': ('Per simulation wave settings', 'Source/FluidNinjaBuoyancy/Public/FluidNinjaWaveSimComponent.h'),
 'S15': ('Source gather shader', 'Shaders/Private/FluidNinjaBuoyancyGather.usf'),
 'S16': ('Wave source and self field shader', 'Shaders/Private/FluidNinjaBuoyancyWaveSource.usf'),
 'S17': ('Preset data asset schema', 'Source/FluidNinjaBuoyancy/Public/FluidNinjaBuoyancyPreset.h'),
 'S18': ('Component setup and effects', 'Source/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyComponent.cpp'),
 'S19': ('Quality tier configuration', 'Config/DefaultFluidNinjaBuoyancy.ini'),
 'S20': ('Developer test bench', 'Source/FluidNinjaBuoyancyDeveloper/Private/FluidNinjaBuoyancyTestBench.cpp'),
 'S21': ('Public subsystem and integration API', 'Source/FluidNinjaBuoyancy/Public/FluidNinjaBuoyancySubsystem.h'),
 'S22': ('Swimmer component API', 'Source/FluidNinjaBuoyancy/Public/FluidNinjaBuoyancySwimmerComponent.h'),
 'S23': ('Module registration and settings constructor', 'Source/FluidNinjaBuoyancy/Private/FluidNinjaBuoyancyModule.cpp'),
 'S24': ('Swimmer movement adapter interface', 'Source/FluidNinjaBuoyancy/Public/FluidNinjaBuoyancySwimmerProvider.h'),
 'S25': ('Swimmer skeleton and contact profile', 'Source/FluidNinjaBuoyancy/Public/FluidNinjaBuoyancySwimmerProfile.h'),
}

def esc(s): return html.escape(str(s))

def properties(path, start):
    text = path.read_text(encoding='utf-8-sig')
    text = text[text.index(start):]
    lines = text.splitlines()
    out, comment, i = [], '', 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith('/**'):
            parts = [line]
            while '*/' not in lines[i] and i + 1 < len(lines):
                i += 1; parts.append(lines[i].strip())
            comment = re.sub(r'/\*\*|\*/|^\s*\*', '', '\n'.join(parts), flags=re.M)
            comment = re.sub(r'\s+', ' ', comment).strip()
        elif line.startswith('UPROPERTY('):
            attr = line
            while attr.count('(') > attr.count(')') and i + 1 < len(lines):
                i += 1; attr += ' ' + lines[i].strip()
            i += 1
            decl = lines[i].strip() if i < len(lines) else ''
            while ';' not in decl and i + 1 < len(lines):
                i += 1; decl += ' ' + lines[i].strip()
            if 'EditAnywhere' in attr:
                m = re.match(r'(.+?)\s+(\w+)\s*(?:=\s*(.*?))?;', decl)
                cat = re.search(r'Category\s*=\s*"([^"]+)"', attr)
                if m:
                    typ, name, default = m.groups()
                    unit = re.search(r'Units\s*=\s*"([^"]+)"', attr)
                    # Source comments are context, not a claim of current asset state.
                    out.append(dict(name=name, type=typ, default=default or 'Constructor / empty',
                                    category=cat.group(1) if cat else 'Settings',
                                    units=unit.group(1) if unit else '', comment=comment))
            comment = ''
        elif line and not line.startswith('//') and not line.startswith('UFUNCTION'):
            if not line.startswith('*'): comment = ''
        i += 1
    return out

OVERRIDES = {
 'WakeGain': 'Gain for data channel wake points. This is an artistic feedback control; source comments about never overshooting are not a conservation guarantee. See Feedback and effects.',
 'PlungeGain': 'Gain for the outward entry burst. A symmetric authored burst can still bias the nonlinear FluidNinja river field; measure it before increasing.',
 'RelativeTrackingSwitch': 'Name of the capability flag checked by the plugin. The custom core implements TrackVelocityRelativeToWater; stock FluidNinja does not.',
 'bWake': 'Enable data channel wake points when the global wake setting and Interaction mode also allow them. This does not disable the separate Waves system or tracking by itself.',
 'bWaves': 'Participation in the separate wave system. Component: generate and feel waves; project or simulation: enable the corresponding wave field.',
 'SplashMinWatts': 'Floor for repeated slam splashes. Entry splashes and bow spray have separate paths; zero does not disable all splash effects.',
 'WaveImpulseScale': 'Scales body impulses sent to the wave source. Current header default is 0.35; older prose describing 1 is superseded.',
 'WaveBreakingSlope': 'Slope threshold for breaking-wave damping. Current header default is 1; older 0.4 guidance is superseded.',
 'WaveAddedMassScale': 'Scale for added water mass in the physics callback. Current source default is 1; use measured stability and period tests when changing it.',
 'Pontoons': 'Explicit sphere centers, radii, LiftWeight and DragWeight. Positions and radii are scaled body-frame centimeters. Empty array uses generated support rows.',
 'bRadiationDamping': 'Restores resistance from the body’s own waves in heave, roll, and pitch. Current implementation estimates motion frequency and selects damping bins, with a natural-frequency fallback.',
 'FoamDecay': 'Exponential foam decay rate in 1/s. At 0.7 the e-folding time is 1.43 s, leaving about 37% then; it is not fully gone after that time.',
 'CaptureWidthCm': 'Width of the captured water square in cm. Runtime follows the configured simulation or centers relevant body bounds, selecting a nearby simulation cluster when bodies are too spread out.',
 'CenterOfMassOffset': 'Offset in the mesh body frame, applied as COMNudge. Scaling conventions differ from explicit pontoons; use unit-scale meshes and verify Analyze Hull.',
}

def property_reference():
    specs = [
      ('Body component', 'S1', 'class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancyComponent'),
      ('Project settings', 'S5', 'class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancySettings'),
      ('Simulation component', 'S14', 'class FLUIDNINJABUOYANCY_API UFluidNinjaWaveSimComponent'),
      ('Preset asset', 'S17', 'class FLUIDNINJABUOYANCY_API UFluidNinjaBuoyancyPreset'),
    ]
    groups=[]; inventory=[]
    for label, sid, start in specs:
        props=properties(PLUGIN/SOURCES[sid][1], start)
        inventory.extend([dict(owner=label, source=sid, **p) for p in props])
        cats=list(dict.fromkeys(p['category'] for p in props))
        for cat in cats:
            rows=[]
            for p in props:
                if p['category'] != cat: continue
                desc=OVERRIDES.get(p['name'], p['comment'])
                # Keep the reference concise; full definitions are linked in source index.
                if len(desc)>350:
                    sentence=re.split(r'(?<=[.!?])\s+(?=[A-Z])',desc)[0]
                    desc=sentence if len(sentence)<=350 else desc[:345].rsplit(' ',1)[0]+'…'
                default=p['default'].replace('::',' :: ').replace('TEXT(', '(')
                rows.append(f'<tr><td><code>{esc(p["name"])}</code>{("<small>"+esc(p["units"])+"</small>") if p["units"] else ""}</td><td><code>{esc(default)}</code></td><td>{esc(desc or "See the source declaration and its owning settings page.")}</td></tr>')
            groups.append(f'<div class="propgroup"><h3>{esc(label)} / {esc(cat)} <a class="ref" href="#{sid}">[{sid}]</a></h3><table class="properties"><thead><tr><th>Exact property</th><th>Header default</th><th>Meaning</th></tr></thead><tbody>'+''.join(rows)+'</tbody></table></div>')
    (HERE/'property_inventory.json').write_text(json.dumps(inventory,indent=2,ensure_ascii=False),encoding='utf-8')
    return ''.join(groups), len(inventory)

CSS = r'''
:root{--ink:#172b3b;--muted:#526575;--teal:#087d86;--blue:#246bc2;--line:#d6e1e6;--soft:#eff6f8;--warm:#fff4df}*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;font:17px/1.65 "Segoe UI",Arial,sans-serif;color:var(--ink);background:#f5f8fa}a{color:#11658c;text-underline-offset:3px}code{font:0.85em/1.4 Consolas,monospace;overflow-wrap:anywhere}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#edf3f5;padding:18px;border-radius:8px;font:13px/1.6 Consolas,monospace}h1,h2,h3,h4{line-height:1.2;letter-spacing:-.02em}h1{font-size:48px;margin:22px 0}h2{font-size:32px;margin:0 0 16px}h3{font-size:21px;margin:28px 0 12px}p{margin:0 0 16px}li{margin:8px 0}small{display:block;font-size:.76em;color:var(--muted)}nav{position:fixed;top:0;left:0;width:260px;height:100vh;overflow:auto;background:#112e40;padding:28px 22px;color:white}nav strong{display:block;font-size:20px;line-height:1.3;margin-bottom:12px}nav a{display:block;color:#d6e9ef;text-decoration:none;font-size:13px;line-height:1.35;padding:7px 0}nav a:hover{color:white;text-decoration:underline}nav .navtop{margin:18px 0;color:#8fdad8;font-size:12px;text-transform:uppercase;letter-spacing:.12em}main{max-width:1260px;margin-left:260px;padding:34px 56px 80px}section{background:white;border:1px solid #e1e9ed;border-radius:14px;margin:0 0 28px;padding:42px;scroll-margin-top:20px}.cover{background:linear-gradient(140deg,#fff 40%,#dff1f0);padding:50px 44px}.eyebrow{font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.16em;color:var(--teal);margin-bottom:18px}.lead{font-size:21px;line-height:1.5;color:#345366}.meta{font-size:13px;color:var(--muted);margin-top:25px}.callout{background:var(--soft);border-left:4px solid var(--teal);padding:16px 20px;margin:20px 0;border-radius:0 6px 6px 0}.note{background:var(--warm);border-left-color:#b37d20}.cards{display:grid;grid-template-columns:repeat(2,1fr);gap:16px;margin:22px 0}.card{border:1px solid var(--line);border-radius:9px;padding:22px}.card h3{margin:0 0 10px}.pill{font-size:11px;letter-spacing:.07em;text-transform:uppercase;padding:4px 9px;background:#dcefeb;border-radius:12px;font-weight:700}.metric{font-size:30px;font-weight:700;color:var(--teal);line-height:1.3;margin:10px 0}.math{background:#edf4f8;border:1px solid #d5e3ed;padding:16px 20px;font:17px/1.9 "Cambria Math",Cambria,serif;margin:18px 0;border-radius:8px}.ref{font-size:11px;white-space:nowrap;color:#607b87}table{border-collapse:collapse;width:100%;margin:18px 0 24px;font-size:14px;line-height:1.5;table-layout:fixed}th{background:#173e53;color:white;text-align:left;font-weight:600}td,th{border:1px solid #d1dde4;padding:10px 12px;vertical-align:top;overflow-wrap:anywhere}tbody tr:nth-child(even){background:#f3f7f9}.properties th:nth-child(1){width:29%}.properties th:nth-child(2){width:22%}.properties th:nth-child(3){width:49%}.properties{font-size:12px}figure{margin:25px 0}figure svg{display:block;width:100%;height:auto}figcaption{font-size:12px;line-height:1.5;color:var(--muted);margin-top:10px}svg text{font-family:"Segoe UI",Arial,sans-serif;fill:#173548}svg .label{font-size:14px}svg .small{font-size:11px}svg .big{font-size:17px;font-weight:700}svg .box{fill:#eef5f8;stroke:#b9d2db;stroke-width:1.5}svg .arrow{stroke:#207a95;stroke-width:2;fill:none;marker-end:url(#arrow)}.two{display:grid;grid-template-columns:1fr 1fr;gap:25px}.sources li{font-size:12px}.sources code{font-size:11px}input,select,button{font:inherit;border:1px solid #aac4d0;border-radius:6px;padding:9px 12px;background:white;color:var(--ink)}button{cursor:pointer;background:#0b6e79;color:white}label{display:block;font-size:14px;margin:10px 0}label input,label select{display:block;width:100%;margin-top:4px}.calcgrid{display:grid;grid-template-columns:1fr 1fr;gap:18px}.calcresult{background:#edf7f5;border-radius:8px;padding:22px}.searchbar{width:100%;margin:8px 0 20px}.sourcepath{display:block;word-break:break-word}.printonly{display:none}.noprint small{display:inline}footer{text-align:center;color:var(--muted);font-size:12px}.equation-note{font-size:13px;color:var(--muted)}
@media(max-width:1100px){nav{width:210px}main{margin-left:210px;padding:24px}section{padding:28px}h1{font-size:38px}}@media(max-width:750px){nav{position:static;width:auto;height:auto}nav .toc{display:none}main{margin:0;padding:12px}section{padding:23px}.two,.cards,.calcgrid{grid-template-columns:1fr}h1{font-size:34px}h2{font-size:27px}table{font-size:12px}td,th{padding:7px}}
@page{size:A4;margin:18mm 16mm 18mm} @media print{html{scroll-behavior:auto}body{background:white;font-size:10pt;line-height:1.5;color:#162f40}nav,.noprint{display:none!important}.printonly{display:block}main{margin:0;padding:0;max-width:none}section{border:0;border-radius:0;padding:0;margin:0;break-before:page}section:first-child{break-before:auto}.cover{background:white;padding:10mm 0}.eyebrow{font-size:9pt;margin-bottom:12pt}h1{font-size:32pt}h2{font-size:23pt;margin:0 0 14pt}h3{font-size:14pt;margin:18pt 0 8pt;break-after:avoid}p{margin-bottom:9pt;orphans:3;widows:3}.lead{font-size:13pt}.meta{font-size:9pt}.callout{padding:10pt 12pt;margin:12pt 0;break-inside:avoid}table{font-size:8.8pt;line-height:1.4;margin:12pt 0 16pt}td,th{padding:6pt 7pt}tr{break-inside:avoid}thead{display:table-header-group}.properties{font-size:8pt}.properties code{font-size:7.8pt}.cards{gap:10pt;margin:14pt 0}.card{padding:13pt;break-inside:avoid}.card h3{font-size:13pt}.metric{font-size:23pt}.math{font-size:12pt;padding:10pt 13pt;line-height:1.7;break-inside:avoid}figure{margin:16pt 0;break-inside:avoid}figcaption{font-size:8pt}a{color:#175775;text-decoration:none}.ref{font-size:7pt}.two{gap:18pt}.propgroup{break-inside:auto}.propgroup h3{break-after:avoid}pre{font-size:8.3pt;padding:10pt;break-inside:avoid}.sources li{font-size:8pt}.sources code{font-size:7.5pt}.equation-note{font-size:8.5pt}.pill{font-size:8pt}section#reference{break-before:page}*{-webkit-print-color-adjust:exact;print-color-adjust:exact}}
'''

JS = r'''
const search=document.getElementById('propertySearch');
search?.addEventListener('input',()=>{const q=search.value.toLowerCase();document.querySelectorAll('.propgroup').forEach(g=>{let n=0;g.querySelectorAll('tbody tr').forEach(r=>{const ok=r.textContent.toLowerCase().includes(q)||g.querySelector('h3').textContent.toLowerCase().includes(q);r.hidden=!ok;if(ok)n++});g.hidden=!n})});
function calculate(){
 const shape=document.getElementById('shape').value, radius=+document.getElementById('radius').value/100, length=+document.getElementById('length').value/100, width=+document.getElementById('width').value/100, height=+document.getElementById('height').value/100, mass=+document.getElementById('mass').value, rho=+document.getElementById('rho').value;
 const out=document.getElementById('calcresult');
 if([radius,length,width,height,mass,rho].some(x=>!Number.isFinite(x)||x<=0)){out.textContent='Enter positive dimensions, mass, and water density.';return}
 let V=shape==='sphere'?4*Math.PI*radius**3/3:shape==='cylinder'?Math.PI*radius**2*length:shape==='vboat'?length*width*height/2:length*width*height;
 let limit=rho*V, ratio=mass/limit, depth=null;
 function submerged(d){if(shape==='sphere')return Math.PI*d*d*(radius-d/3);if(shape==='cylinder')return length*(radius*radius*Math.acos(Math.max(-1,Math.min(1,(radius-d)/radius)))-(radius-d)*Math.sqrt(Math.max(0,2*radius*d-d*d)));if(shape==='vboat')return length*width*d*d/(2*height);return length*width*d}
 if(ratio<1){let lo=0,hi=(shape==='box'||shape==='vboat')?height:2*radius;for(let i=0;i<70;i++){let mid=(lo+hi)/2;if(submerged(mid)<mass/rho)lo=mid;else hi=mid}depth=(lo+hi)/2}
 out.innerHTML=`<span class="pill">Ideal calm water calculation</span><div class="metric">${ratio>1?'Sinks':ratio===1?'Neutral when submerged':(100*depth).toFixed(2)+' cm draft'}</div><p>Full volume <b>${(V*1000).toFixed(2)} L</b><br>Maximum buoyant support <b>${limit.toFixed(2)} kg</b><br>Mean density <b>${(mass/V).toFixed(1)} kg/m³</b><br>Required displaced fraction <b>${(ratio*100).toFixed(2)}%</b></p><p>${ratio>1?'Mass exceeds the water displaced at full immersion.':ratio===1?'No unique waterline with freeboard. Any fully submerged depth is neutrally buoyant in this ideal model.':'At rest, upward buoyancy equals weight.'}</p><small>Geometry only. No waves, collisions, drag, flooding, surface tension, or stability prediction. These ideal hulls are teaching approximations, not measured boat hulls.</small>`;
 const marker=document.getElementById('watermark'); if(marker){const sub=Math.min(1,depth===null?1:depth/((shape==='box'||shape==='vboat')?height:2*radius));marker.setAttribute('y',String(195-150*sub));marker.setAttribute('height',String(150*sub));}
}
function presetCalc(which){const vals={ball:['sphere',30,300,120,50,.904778684,1000],log:['cylinder',20,300,120,50,188.4955592,1000],boat:['vboat',30,300,120,50,240,1000],steel:['sphere',40,300,120,50,2104.448199,1000]}[which];['shape','radius','length','width','height','mass','rho'].forEach((id,i)=>document.getElementById(id).value=vals[i]);calculate()}
document.querySelectorAll('#calculator input,#calculator select').forEach(e=>e.addEventListener('input',calculate));if(document.getElementById('calculator'))calculate();
'''

def build():
    body=(HERE/'guide_body.html').read_text(encoding='utf-8')
    reference,count=property_reference()
    body=body.replace('<!-- PROPERTY_REFERENCE -->',reference).replace('{{PROPERTY_COUNT}}',str(count))
    manifest=[]; sources=[]
    for sid,(label,rel) in SOURCES.items():
        p=PLUGIN/rel
        raw=p.read_bytes() # Text source only; never read Unreal binary packages.
        manifest.append(dict(id=sid,title=label,path=rel,sha256=hashlib.sha256(raw).hexdigest()))
        sources.append(f'<li id="{sid}"><b>{sid} · {esc(label)}</b><span class="sourcepath"><code>{esc(rel)}</code></span></li>')
    body=body.replace('<!-- SOURCE_LIST -->',''.join(sources))
    sections=re.findall(r'<section[^>]*id="([^"]+)"[^>]*>\s*(?:<div[^>]*>.*?</div>\s*)?<h[12]>(.*?)</h[12]>',body,re.S)
    nav=''.join(f'<a href="#{id}">{re.sub("<[^>]+>","",title)}</a>' for id,title in sections)
    tocfile=HERE/'print_page_map.json'
    pagemap=json.loads(tocfile.read_text(encoding='utf-8')) if tocfile.exists() else {}
    tocrows=''.join(f'<tr><td><a href="#{sid}">{re.sub("<[^>]+>","",title)}</a></td><td>{pagemap.get(sid,"—")}</td></tr>' for sid,title in sections if sid!='overview')
    toc='<section id="contents" class="printonly"><div class="eyebrow">Reading and reference</div><h2>Contents</h2><p>Read the architecture first, follow the object recipes, then use the property reference while implementing.</p><table class="printtoc"><thead><tr><th>Topic</th><th>Page</th></tr></thead><tbody>'+tocrows+'</tbody></table><p class="equation-note">Source defaults and calculated examples are labeled throughout. This guide does not claim a new runtime validation.</p></section>'
    body=body.replace('</section>','</section>'+toc,1)
    printcss='@media print{section{break-before:auto;margin-top:22pt}section.cover{break-after:page;margin-top:0}section#contents{break-after:page;margin-top:0}section#reference,section#sources{break-before:page;margin-top:0}h2,.eyebrow{break-after:avoid}.printtoc{font-size:9pt}.printtoc th:first-child{width:91%}.printtoc td{padding:5pt 7pt}.printtoc th:last-child,.printtoc td:last-child{text-align:right}.printtoc tr{background:white}}'
    document='<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>FluidNinja architecture and buoyancy guide</title><style>'+CSS+printcss+'</style></head><body><nav aria-label="Guide contents"><strong>FluidNinja<br>Architecture and buoyancy</strong><div class="navtop">Core and plugin guide</div><div class="toc">'+nav+'</div><a href="FluidNinja_Architecture_and_Buoyancy_Guide.pdf">Download printable PDF</a><a href="#calculator">Open buoyancy calculator</a><a href="#reference">Search properties</a></nav><main>'+body+'<footer class="noprint">Prepared 18 October 2026 · Source audited guide · Modified FluidNinja core and FluidNinja Buoyancy</footer></main><script>'+JS+'</script></body></html>'
    (HERE/'index.html').write_text(document,encoding='utf-8')
    (HERE/'source_manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print(json.dumps({'html':str(HERE/'index.html'),'properties':count,'sections':len(sections),'sources':len(manifest)}))

if __name__=='__main__': build()
