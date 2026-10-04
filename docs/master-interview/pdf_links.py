#!/usr/bin/env python3
"""Make PDF cross-part links portable; keep precise GoToR page destinations."""
import json,re,sys
from pathlib import Path
from urllib.parse import unquote,urlsplit
try:
    import pypdf
except ImportError:
    # Reuse an already installed local cache, without fetching dependencies.
    candidates=list((Path.home()/'.cache/uv/archive-v0').glob('*/pypdf/__init__.py'))
    if not candidates:raise SystemExit('PDF cross-file links require pypdf; install it in the Python environment.')
    sys.path.insert(0,str(candidates[0].parents[1]));import pypdf
from pypdf.generic import DictionaryObject,NameObject,TextStringObject,ArrayObject,NumberObject,BooleanObject
HERE=Path(__file__).resolve().parent
first=HERE/'第一部分_纯八股.pdf'
page_map={}
reader=pypdf.PdfReader(first)
for index,page in enumerate(reader.pages):
    for key in re.findall(r'\b(T\d{3})\s*原理问答',page.extract_text()):page_map[key]=index
expected=json.loads((HERE/'build-report.json').read_text())
assert len(page_map)==expected['theory_questions'],(len(page_map),sorted(page_map))
report={'theory_page_map':{k:v+1 for k,v in sorted(page_map.items())},'files':{}}
for filename in ['第一部分_纯八股.pdf','第二部分_项目面经.pdf','网关与节点总面经.pdf']:
    path=HERE/filename;reader=pypdf.PdfReader(path);writer=pypdf.PdfWriter();writer.clone_document_from_reader(reader)
    converted=portable=0
    for page in writer.pages:
        for ref in page.get('/Annots',[]):
            annotation=ref.get_object();action=annotation.get('/A')
            if not action:continue
            action=action.get_object();uri=action.get('/URI')
            if not uri:continue
            parsed=urlsplit(str(uri));local=unquote(parsed.path)
            if parsed.scheme=='file':
                if local.endswith('/theory.html') and parsed.fragment in page_map:
                    spec=DictionaryObject({NameObject('/Type'):NameObject('/Filespec'),NameObject('/F'):TextStringObject(first.name),NameObject('/UF'):TextStringObject(first.name)})
                    annotation[NameObject('/A')]=DictionaryObject({NameObject('/S'):NameObject('/GoToR'),NameObject('/F'):spec,NameObject('/D'):ArrayObject([NumberObject(page_map[parsed.fragment]),NameObject('/Fit')]),NameObject('/NewWindow'):BooleanObject(True)})
                    converted+=1
                elif local.startswith(str(HERE)+'/'):
                    relative=local[len(str(HERE))+1:]+(('#'+parsed.fragment) if parsed.fragment else '')
                    action[NameObject('/URI')]=TextStringObject(relative);portable+=1
    tmp=path.with_suffix('.tmp.pdf')
    with tmp.open('wb') as stream:writer.write(stream)
    tmp.replace(path)
    report['files'][filename]=dict(theory_cross_file_links=converted,relative_local_links=portable,pages=len(reader.pages))
assert report['files']['第二部分_项目面经.pdf']['theory_cross_file_links']>=expected['cross_references'],report
(HERE/'pdf-link-map.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(report,ensure_ascii=False))
