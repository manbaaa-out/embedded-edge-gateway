#!/usr/bin/env python3
"""Build a self-contained interview book and immutable local source snapshots."""
import argparse
import hashlib
import html
import json
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from catalog import ROOT, NODE, SOURCES, REFS

HERE = Path(__file__).resolve().parent
E = html.escape

def git_info(root):
    def run(*args):
        return subprocess.check_output(['git','-C',str(root),*args],text=True).strip()
    return {'commit':run('rev-parse','HEAD'),'branch':run('branch','--show-current')}

def snapshot_sources(refresh=False):
    destination = HERE/'source'
    destination.mkdir(exist_ok=True)
    manifest_path=HERE/'source-baseline.json'
    previous=json.loads(manifest_path.read_text()) if manifest_path.exists() else None
    manifest=dict(verified_on='2026-10-08',captured_at=datetime.now(timezone.utc).isoformat(),
                  gateway=git_info(ROOT),node=git_info(NODE),files={},
                  scope='Working-tree contents; unrelated resume modifications are excluded.',
                  freertos='Vendored development snapshot, not a verified release tag; see kernel-version.txt.')
    for key,(repo,relative,token) in SOURCES.items():
        root=ROOT if repo=='gateway' else NODE
        path=root/relative
        raw=path.read_bytes()
        sha=hashlib.sha256(raw).hexdigest()
        text=raw.decode('utf-8').replace('\r\n','\n')
        assert token in text,(key,token)
        line=text[:text.index(token)].count('\n')+1
        if previous and not refresh:
            assert previous['files'][key]['sha256']==sha, f'Source drift: {key}; review content before --refresh-baseline'
        label=('网关 / ' if repo=='gateway' else '节点 / ')+relative
        rows='\n'.join(f'<span class="line" id="L{i}"><a href="#L{i}" aria-label="第 {i} 行">{i}</a><code>{E(t)}</code></span>' for i,t in enumerate(text.splitlines(),1))
        page='<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
        page+=f'<title>{E(label)} · 源码快照</title><style>body{{margin:24px;font:15px/1.6 system-ui;color:#17333f}}a{{color:#087f85}}h1{{font-size:20px;overflow-wrap:anywhere}}.line{{display:flex;min-width:0;scroll-margin-top:12px}}.line>a{{min-width:5em;color:#71828c;text-decoration:none;user-select:none}}code{{white-space:pre-wrap;overflow-wrap:anywhere;tab-size:4;font:13px/1.7 monospace;flex:1;min-width:0}}.line:target{{background:#ffefb8}}.meta{{overflow-wrap:anywhere;color:#62747e}}</style>'
        page+=f'<a href="../index.html">返回总面经</a> · <a href="../source-index.html">源码索引</a><h1>{E(label)}</h1><p class="meta">源码基线：2026-10-08 · SHA-256 {sha}</p><p>这是编写面经时的离线只读快照。行号保留原文件位置；代码更改后请先审阅答案，再刷新基线。</p><div>{rows}</div></html>'
        (destination/(key+'.html')).write_text(page)
        manifest['files'][key]=dict(repository=repo,path=relative,sha256=sha,line=line,
                                   lines=len(text.splitlines()),token=token)
    # Removed implementation files must not remain as apparently current evidence.
    for obsolete in destination.glob('*.html'):
        if obsolete.stem not in SOURCES:
            obsolete.unlink()
    extras=[('node',NODE/'Middlewares/Third_Party/FreeRTOS-Kernel/VERSION.txt','kernel-version.txt'),
            ('node',NODE/'Middlewares/Third_Party/FreeRTOS-Kernel/LICENSE.md','FreeRTOS-LICENSE.txt'),
            ('node',NODE/'Drivers/STM32F1xx_HAL_Driver/LICENSE.txt','ST-HAL-LICENSE.txt'),
            ('gateway',ROOT/'LICENSE','gateway-LICENSE.txt')]
    for _,src,name in extras:
        if src.exists(): (destination/name).write_bytes(src.read_bytes())
    if previous and not refresh:
        manifest['captured_at']=previous['captured_at']
    manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
    return manifest
