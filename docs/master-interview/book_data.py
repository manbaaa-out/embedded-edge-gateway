"""Load and check the two independent, authored parts of the book."""
import json,re
from pathlib import Path
from catalog import REFS, SOURCES
HERE=Path(__file__).resolve().parent

def load():
    chapters=json.loads((HERE/'v3/theory.json').read_text())
    questions={q['old_id']:q for c in chapters for q in c['questions']}
    for line in (HERE/'v3/deepening.txt').read_text().splitlines():
        if not line:continue
        if line.startswith('## '):
            q=questions[int(line[3:])];q['examples']=[]
        else:
            kind,text=line.split(': ',1)
            if kind=='A':q['answer']=text
            elif kind=='E':
                title,text=text.split('|',1);q['examples'].append(dict(title=title,text=text))
            elif kind=='F':
                title,text=text.split('|',1);q['followups'].append(dict(question=title,answer=text))
            else:raise ValueError(line)
    # Remove repeated mechanisms where the expanded derivation already explains them.
    omit={29:[1,2],30:[0,1,2],31:[0,1,2],32:[0,1],33:[0,1],34:[0,1],35:[0],
          13:[0],14:[0,1],15:[0],20:[0],21:[0],22:[0],40:[0],41:[0],
          69:[0],70:[0],76:[0],77:[0],78:[0],85:[0],86:[0],87:[0],91:[0],97:[0]}
    omit.update({15:[2],36:[1],38:[1,3],116:[1,2],41:[0,4],59:[2],67:[2],80:[1,3],83:[4],90:[1],91:[0,3],92:[1],94:[0],96:[1]})
    for old,q in questions.items():
        q['sections']=[s for i,s in enumerate(q['sections']) if i not in omit.get(old,[])]
        q['part']='theory'
    # Keep the first part free of claims about this repository or authorship.
    questions[17]['refs'].append('rta')
    questions[115]['title']='Cache、伪共享和内存序为什么会影响多核并发？'
    for sec in questions[99]['sections']:
        sec['text']=sec['text'].replace('命令任务为协议 TTL 或错误维护每 100 ms 醒一次','例如，一个任务为协议 TTL 或错误维护每 100 ms 醒一次')
    for sec in questions[48]['sections']:
        sec['text']=sec['text'].replace('测试资产存在也不代表候选人亲自设计过它们。','')
    extra=dict(id='theory-12',title='跨模块设计与定量分析',questions=[])
    chapters.append(extra)
    for line in (HERE/'v3/extra-theory.txt').read_text().splitlines():
        if not line:continue
        if line.startswith('## '):
            key,title,refs=line[3:].split('|')
            q=dict(id=key,title=title,refs=refs.split(','),priority='P0',part='theory',sections=[],examples=[],followups=[])
            extra['questions'].append(q)
        else:
            kind,text=line.split(': ',1)
            if kind=='A':q['answer']=text
            else:
                title,text=text.split('|',1)
                key={'M':'sections','E':'examples','F':'followups'}[kind]
                q[key].append(dict(question=title,answer=text) if kind=='F' else dict(title=title,text=text))
    extra['questions'][-1]['sections'][0]['text']=extra['questions'][-1]['sections'][0]['text'].replace('周期或最小间隔已知且任务模型符合假设时','独立周期/间歇任务、截止期不大于周期、无释放抖动且阻塞已被正确界定时')
    porting=dict(id='theory-13',title='FreeRTOS 移植：源码、异常与验收',questions=[])
    chapters.append(porting)
    for line in (HERE/'v3/freertos-port-theory.txt').read_text().splitlines():
        if not line:continue
        if line.startswith('## '):
            key,title,refs=line[3:].split('|')
            q=dict(id=key,title=title,refs=refs.split(','),priority='P0',part='theory',sections=[],examples=[],followups=[])
            porting['questions'].append(q)
        else:
            kind,text=line.split(': ',1)
            if kind=='A':q['answer']=text
            else:
                title,text=text.split('|',1)
                key={'M':'sections','E':'examples','F':'followups'}[kind]
                q[key].append(dict(question=title,answer=text) if kind=='F' else dict(title=title,text=text))
    project=[]
    for line in (HERE/'v3/project.txt').read_text().splitlines():
        if not line:continue
        if line.startswith('# '):
            cid,title=line[2:].split('|');c=dict(id=cid,title=title,questions=[]);project.append(c)
        elif line.startswith('## '):
            key,title,refs,sources=line[3:].split('|')
            q=dict(id=key,title=title,theory=refs.split(','),sources=sources.split(','),part='project',A=[],D=[],F=[],B=[])
            c['questions'].append(q)
        else:
            kind,text=line.split(': ',1);assert kind in ('A','D','F','B');q[kind].append(text)
    theory=[q for c in chapters for q in c['questions']]
    projects=[q for c in project for q in c['questions']]
    assert [q['id'] for q in theory]==[f'T{i:03d}' for i in range(1,121)]
    assert [q['id'] for q in projects]==[f'P{i:03d}' for i in range(1,55)]
    valid={q['id'] for q in theory}
    for c in chapters+project:
        for q in c['questions']:q['chapter']=c['id']
    for q in theory:
        assert not re.search(r'Q[0-9]{3}',json.dumps(q,ensure_ascii=False)),q['id']
        assert q['answer'] and q['sections'] and q['examples'] and q['followups'],q['id']
        assert all(k in REFS for k in q['refs']), (q['id'],q['refs'])
        assert len(json.dumps(q,ensure_ascii=False))>650,q['id']
        assert not any(w in json.dumps(q,ensure_ascii=False) for w in ('本项目','当前项目','当前网关','当前节点','CommandTracker','SR_RECEIVED')),q['id']
    for q in projects:
        assert len(q['A'])==2 and q['D'] and q['F'] and q['B'],q['id']
        assert all(k in valid for k in q['theory']),q['id']
        assert all(k in SOURCES for k in q['sources']),q['id']
    return chapters,project
