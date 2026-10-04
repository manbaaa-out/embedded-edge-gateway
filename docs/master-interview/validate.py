#!/usr/bin/env python3
"""Check local links, precise T references, immutable source evidence and PDF text."""
import hashlib,json,re,subprocess
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote,urlsplit
from catalog import ROOT,NODE
from book_data import load
HERE=Path(__file__).resolve().parent
class Links(HTMLParser):
    def __init__(self):super().__init__();self.links=[];self.ids=[]
    def handle_starttag(self,tag,attrs):
        attrs=dict(attrs)
        if 'id' in attrs:self.ids.append(attrs['id'])
        if tag=='a' and attrs.get('href'):self.links.append(attrs['href'])
def main():
    files=sorted(HERE.glob('*.html'))+sorted((HERE/'source').glob('*.html'))
    cache={};bad=[];duplicates=[];count=0
    for path in files:
        p=Links();p.feed(path.read_text());cache[path.resolve()]=p
        if len(p.ids)!=len(set(p.ids)):duplicates.append(path.name)
    for path,p in cache.items():
        for href in p.links:
            u=urlsplit(href)
            if u.scheme or u.netloc:continue
            target=(path.parent/unquote(u.path)).resolve() if u.path else path;count+=1
            if not target.exists():bad.append([str(path.relative_to(HERE)),href,'missing'])
            elif u.fragment and target.suffix=='.html':
                if target not in cache or unquote(u.fragment) not in cache[target].ids:bad.append([str(path.relative_to(HERE)),href,'bad anchor'])
    theory,project=load();tqs=[q for c in theory for q in c['questions']];pqs=[q for c in project for q in c['questions']]
    assert len([x for x in cache[HERE/'theory.html'].ids if re.fullmatch('T[0-9]{3}',x)])==len(tqs)
    assert not any(re.fullmatch('P[0-9]{3}',x) for x in cache[HERE/'theory.html'].ids)
    assert len([x for x in cache[HERE/'project.html'].ids if re.fullmatch('P[0-9]{3}',x)])==len(pqs)
    assert not any(re.fullmatch('T[0-9]{3}',x) for x in cache[HERE/'project.html'].ids)
    backlinks=[h for h in cache[HERE/'project.html'].links if h.startswith('theory.html#T')]
    assert len(backlinks)==sum(len(q['theory']) for q in pqs)
    manifest=json.loads((HERE/'source-baseline.json').read_text());drift=[]
    for key,s in manifest['files'].items():
        root=ROOT if s['repository']=='gateway' else NODE
        if hashlib.sha256((root/s['path']).read_bytes()).hexdigest()!=s['sha256']:drift.append(key)
    pdfs={}
    for filename,expected in [('第一部分_纯八股.pdf',[q['id'] for q in tqs]),('第二部分_项目面经.pdf',[q['id'] for q in pqs]),('网关与节点总面经.pdf',[q['id'] for q in tqs+pqs])]:
        text=subprocess.check_output(['pdftotext','-layout',str(HERE/filename),'-'],text=True)
        pages=text.split('\f');pages=pages[:-1] if not pages[-1].strip() else pages
        entries=[]
        for i,page in enumerate(pages,1):
            for key in re.findall(r'\b([TP]\d{3})\s*(?:原理问答|项目问答)',page):entries.append(dict(question=key,page=i))
        assert [v['question'] for v in entries]==expected,(filename,len(entries))
        pdfs[filename]=dict(pages=len(pages),all_question_headers_in_order=True,question_page_map=entries)
    sync=subprocess.run(['bash',str(ROOT/'scripts/check_proto_sync.sh'),str(NODE/'Protocol/edge_proto')],text=True,capture_output=True,check=True)
    report=dict(local_links_checked=count,broken_local_links=bad,duplicate_id_pages=duplicates,html_files_checked=len(files),source_files_checked=len(manifest['files']),source_drift=drift,cross_part_references=len(backlinks),parts_independent=True,protocol_sync=sync.stdout.strip(),pdfs=pdfs)
    (HERE/'link-validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    assert not bad,bad;assert not drift,drift;assert not duplicates,duplicates
    print(json.dumps({k:v for k,v in report.items() if k!='pdfs'},ensure_ascii=False,indent=2))
if __name__=='__main__':main()
