#!/usr/bin/env python3
"""Package only this edition and reject stale validation or visual review."""
from pathlib import Path
from zipfile import ZipFile,ZIP_DEFLATED
import hashlib,json
root=Path(__file__).resolve().parent;target=root.parent/'master-interview.zip'
required=['index.html','theory.html','project.html','full.html','theory-13.html','project-07.html','coverage.html','references.html','research-notes.json','source-index.html','source-baseline.json','validation.json','link-validation.json','visual-review.json','README.md']
assert all((root/p).is_file() for p in required)
browser=json.loads((root/'validation.json').read_text());visual=json.loads((root/'visual-review.json').read_text());links=json.loads((root/'link-validation.json').read_text())
expected=json.loads((root/'build-report.json').read_text())
assert len(browser['chapterPages'])==expected['theory_chapters']+expected['project_chapters']
for filename,sha in browser['artifacts'].items():assert hashlib.sha256((root/filename).read_bytes()).hexdigest()==sha,filename+' browser validation is stale'
for filename,result in browser['pdfs'].items():
    assert not result['missing'] and result['searchableParagraphs']==result['bodyParagraphs']
    assert visual['artifacts'][filename]==browser['artifacts'][filename],filename+' visual review is stale'
assert not links['broken_local_links'] and not links['source_drift']
assert links['cross_part_references']==expected['cross_references'] and links['parts_independent']
assert not list(root.glob('chapter-*.html')),'Old edition files must not remain in the package'
with ZipFile(target,'w',ZIP_DEFLATED,compresslevel=9) as z:
    for path in sorted(root.rglob('*')):
        if not path.is_file() or '__pycache__' in path.parts or path.suffix=='.pyc':continue
        z.write(path,Path(root.name)/path.relative_to(root))
with ZipFile(target) as z:
    assert z.testzip() is None
    assert all(n.startswith('master-interview/') for n in z.namelist())
    print(f'{target.name}: {len(z.namelist())} files, {target.stat().st_size:,} bytes; CRC check passed')
