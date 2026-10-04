#!/usr/bin/env python3
"""Build two independent interview books with explicit theory cross references."""
import argparse,html,json,re
from pathlib import Path
from catalog import REFS
from book_data import load
from source_snapshot import snapshot_sources
HERE=Path(__file__).resolve().parent
E=html.escape
THEORY,PROJECT=load()
TQ={q['id']:q for c in THEORY for q in c['questions']}
PQ={q['id']:q for c in PROJECT for q in c['questions']}

def link(href,label):return f'<a href="{E(href,quote=True)}">{E(label)}</a>'
def para(text,cls='bodytext'):return f'<p class="{cls}">{E(text)}</p>'
def refs(q):
    return '<div class="references"><h4>依据与核对范围</h4>'+''.join('<p>'+link(REFS[k]['url'],REFS[k]['title'])+'<span> — '+E(REFS[k]['scope'])+'</span></p>' for k in q['refs'])+'</div>'
def source_links(q,manifest):
    return ' · '.join(link('source/'+k+'.html#L'+str(manifest['files'][k]['line']),('网关 ' if manifest['files'][k]['repository']=='gateway' else '节点 ')+Path(manifest['files'][k]['path']).name+':'+str(manifest['files'][k]['line'])) for k in q['sources'])

def project_diagram(key):
    if key=='P001':
        rows=[('遥测上行',['节点采样 / TX 队列','串口 Reactor 拆帧','GatewayCore 解码','分别交 control 发布 / writer 写库']),('控制下行',['MQTT 原始消息','GatewayCore 解释','队列 / eventfd','串口链路 / 节点执行','GatewayCore 应答'])]
        return '<figure class="flow"><figcaption>先记住这两条数据路径</figcaption>'+''.join('<h5>'+title+'</h5><ol>'+''.join('<li>'+E(v)+'</li>' for v in values)+'</ol>' for title,values in rows)+'</figure>'
    if key=='P004':
        rows=[('串口 epoll','fd、缓冲、SR 状态','完整帧 / 命令完成'),('MQTT 网络循环','连接收发、MQTT 协议','原始 topic / payload'),('HTTP Asio 循环','连接、HTTP 解析和收发','请求 / 响应完成'),('GatewayCore','应用转换与路由','命令 / 发布 / SQL 任务'),('gateway-control','MQTT 发布/替换、资源准备','串口候选 / 有序切库任务'),('SQLite 读写执行器','各自连接与语句','查询结果 / 写入进度')]
        return '<figure class="flow"><figcaption>同一进程内的线程与消息交接</figcaption><table><thead><tr><th>线程</th><th>独占职责</th><th>跨线程消息</th></tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+E(v)+'</td>' for v in row)+'</tr>' for row in rows)+'</tbody></table></figure>'
    if key=='P014':
        rows=[('请求与结果',['Beast 完成 HTTP 读取','GatewayCore 校验路由','读执行器执行 SQL','GatewayCore 生成响应','post 回 HTTP / async_write'])]
        return '<figure class="flow"><figcaption>网络循环与业务、数据库执行分离</figcaption>'+''.join('<ol>'+''.join('<li>'+E(v)+'</li>' for v in values)+'</ol>' for _,values in rows)+'</figure>'
    if key=='P053':
        rows=[('构建接线',['STM32 HAL 工程','内核公共源码 + ARM_CM3 port + heap_4','FreeRTOSConfig.h 与异常入口']),('上电运行',['复位与 main','建共享对象及任务','SVC 首任务 → SysTick 计时 → PendSV 切换'])]
        return '<figure class="flow"><figcaption>移植分成构建接线和上电运行两条核查路径</figcaption>'+''.join('<h5>'+title+'</h5><ol>'+''.join('<li>'+E(v)+'</li>' for v in values)+'</ol>' for title,values in rows)+'</figure>'
    if key=='P017':
        rows=[('本地整帧提交','网关 → 本地内核','启动本轮接收等待；尚未证明对端收到'),('SR_RECEIVED','节点 → 网关','节点已接纳请求；尚未证明执行成功'),('业务结果','节点 → 网关','链路接收终态；投递 Core 生成应答'),('SR_RESULT_ACK','网关 → 节点','网关链路已接收；不证明 MQTT 已交付')]
        return '<figure class="flow"><figcaption>四个里程碑分别证明什么</figcaption><table><thead><tr><th>事件</th><th>方向</th><th>含义</th></tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+E(v)+'</td>' for v in row)+'</tr>' for row in rows)+'</tbody></table></figure>'
    return ''

def question(q,manifest,full=False):
    part=q['part'];out=f'<article class="qa {part}" id="{q["id"]}" data-chapter="{q["chapter"]}" data-priority="{q.get("priority", "P0")}">'
    out+=f'<div class="question-label">{q["id"]} <span>{"原理问答" if part=="theory" else "项目问答"}</span></div><h3>{E(q["title"])}</h3>'
    out+='<button class="reveal" type="button">展开 / 收起答案</button><div class="answer-content">'
    if part=='theory':
        out+=para(q['answer'],'bodytext thesis')
        out+='<section class="mechanism"><h4>原理与成立条件</h4>'
        for s in q['sections']:out+='<h5>'+E(s['title'])+'</h5>'+para(s['text'])
        out+='</section><section class="derivation"><h4>推演与反例</h4>'
        for s in q['examples']:out+='<h5>'+E(s['title'])+'</h5>'+para(s['text'])
        out+='</section>'
        if q['id'] in EXAMPLES:out+=example_html(EXAMPLES[q['id']])
        out+='<section class="follow"><h4>继续追问</h4>'
        for s in q['followups']:out+='<h5>'+E(s['question'])+'</h5>'+para(s['answer'])
        out+='</section>'+refs(q)
    else:
        out+='<section class="oral"><h4>面试时这样回答</h4>'+''.join(para(v) for v in q['A'])+'</section>'
        out+=project_diagram(q['id'])
        out+='<nav class="theory-backlinks" aria-label="所用八股知识"><h4>回到第一部分</h4>'
        out+=''.join(link(('#' if full else 'theory.html#')+key,key+' · '+TQ[key]['title']) for key in q['theory'])+'</nav>'
        out+='<section class="details"><h4>实现依据与展开</h4>'
        for value in q['D']:
            title,value=value.split('|',1);out+='<h5>'+E(title)+'</h5>'+para(value)
        out+='<p class="source-links">'+source_links(q,manifest)+'</p></section>'
        out+='<section class="follow"><h4>继续追问</h4>'
        for value in q['F']:
            title,value=value.split('|',1);out+='<h5>'+E(title)+'</h5>'+para(value)
        out+='</section><aside class="boundary"><h4>当前实现边界</h4>'+''.join(para(v) for v in q['B'])+'</aside>'
    return out+'</div></article>'

def example_html(ex):
    return '<figure class="code-example"><figcaption>'+E(ex['title'])+'</figcaption><pre><code>'+E(ex['code'])+'</code></pre>'+para(ex['note'])+'</figure>'
EXAMPLES=json.loads((HERE/'v3/examples.json').read_text())

def cover(title,subtitle,part=''):
    return '<header class="cover"><p class="eyebrow">嵌入式软件面试 · 2026.10.04</p><h1>'+E(title)+'</h1><p class="lead">'+E(subtitle)+'</p>'+('<p class="cover-note">'+E(part)+'</p>' if part else '')+'</header>'
def controls(chapters):
    return '<div class="tools"><label>检索问题与正文 <input id="search" type="search" placeholder="如 T075、P037、优先级继承"></label><label>章节 <select id="chapter-filter"><option value="">全部章节</option>'+''.join('<option value="'+c['id']+'">'+E(c['title'])+'</option>' for c in chapters)+'</select></label><button id="quiz">自测：隐藏答案</button><button id="reset">重置</button><output id="count"></output></div><p id="empty" hidden>没有匹配的问题，请更换关键词。</p>'
def toc(chapters,full=False):
    return '<nav class="toc" aria-label="章节目录">'+''.join('<a href="#'+c['id']+'">'+E(c['title'])+'</a>' for c in chapters)+'</nav>'
def chapters_html(chapters,manifest,full=False):
    return ''.join('<section class="chapter" id="'+c['id']+'"><header class="chapter-head"><p class="eyebrow">'+('第一部分 · 纯八股' if c['id'].startswith('theory') else '第二部分 · 项目面经')+'</p><h2>'+E(c['title'])+'</h2></header>'+''.join(question(q,manifest,full) for q in c['questions'])+'</section>' for c in chapters)

def write_page(name,title,body):
    css=(HERE/'book.css').read_text();js=(HERE/'book.js').read_text()
    nav=''.join(link(p,t) for p,t in [('index.html','总目录'),('theory.html','第一部分 · 纯八股'),('project.html','第二部分 · 项目'),('coverage.html','知识覆盖表'),('references.html','资料校对'),('source-index.html','源码')])
    page='<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>'+E(title)+' · 网关与节点面经</title><style>'+css+'</style></head><body><nav class="site-nav">'+nav+'</nav><main>'+body+'</main><footer>源码核对：2026-10-04 · 原理适用条件见各题及资料说明 · 本文不代替硬件测量</footer><script>'+js+'</script></body></html>'
    (HERE/name).write_text(page)

def directory():
    out=cover('网关与节点面经','先把原理学透，再把项目讲清楚。','两部分独立阅读：第一部分解释原理、推演和易错边界；第二部分沿真实实现回答，并回指所用原理题。')
    nt=sum(len(c['questions']) for c in THEORY);np=sum(len(c['questions']) for c in PROJECT)
    out+=f'<div class="part-cards"><section><p class="eyebrow">PART 01 / {nt} 问</p><h2>纯八股</h2><p>C/C++、操作系统、Linux I/O、通信、存储、STM32、FreeRTOS，以及跨模块定量分析。</p>'+link('theory.html','打开第一部分 →')+' · '+link('第一部分_纯八股.pdf','PDF')+'</section><section><p class="eyebrow">PART 02 / 54 问</p><h2>项目面经</h2><p>先给可直接口述的回答，再看调用链、资源归属、源码证据与实现边界。</p>'+link('project.html','打开第二部分 →')+' · '+link('第二部分_项目面经.pdf','PDF')+'</section></div>'
    out+='<section class="intro"><h2>怎样使用这一版</h2><p>不熟悉基础时按第一部分各章学习；准备项目面试时从 P001 的两分钟介绍进入，再沿 P002/P003 的上下行链路展开。项目题里的 T 编号可直接跳转到对应原理，不需要在项目回答中重复大段定义。</p><p>原理题给出结论、机制、成立条件、具体推演与追问；项目题中的“当前实现边界”区分已有实现和建议。源码快照有行号与哈希，网络参考标明规范、版本及校对范围。</p><p>'+link('coverage.html','查看项目模块 → 项目题 → 八股题覆盖表')+' · '+link('full.html','全文阅读与检索')+' · '+link('网关与节点总面经.pdf','合订 PDF')+'</p></section>'
    for title,chs in [('第一部分 · 纯八股',THEORY),('第二部分 · 项目面经',PROJECT)]:
        out+='<h2>'+title+'</h2><div class="directory">'
        for c in chs:
            out+='<section class="chapter-card"><h3>'+link(c['id']+'.html',c['title'])+'</h3><ul>'+''.join('<li>'+link(c['id']+'.html#'+q['id'],q['id']+' '+q['title'])+'</li>' for q in c['questions'])+'</ul></section>'
        out+='</div>'
    return out

def coverage():
    out=cover('项目知识覆盖表','每条项目回答都能追溯到原理，也能追溯到实现。')
    out+='<p>本表按实际功能组织，覆盖正文中涉及的核心实现与扩展追问。第一部分另含进程管理、CAN、SPI、ADC、OTA 等通用知识；没有对应应用实现的知识不编造项目案例。</p>'
    for c in PROJECT:
        out+='<h2>'+E(c['title'])+'</h2><div class="table-wrap"><table><thead><tr><th>项目问题</th><th>所用原理</th></tr></thead><tbody>'
        for q in c['questions']:
            out+='<tr><td>'+link('project.html#'+q['id'],q['id']+' '+q['title'])+'</td><td>'+''.join(link('theory.html#'+k,k+' '+TQ[k]['title'])+'<br>' for k in q['theory'])+'</td></tr>'
        out+='</tbody></table></div>'
    return out

def reference_page(audit):
    out=cover('资料与校对记录','技术结论优先采用标准、原厂手册、官方源码和高校原始材料。')
    out+='<p>原有机制在 2026-09-20/21 校对，FreeRTOS 移植专题在 2026-09-24 补充，多事件循环与 Boost.Beast 专题在 2026-10-04 核对。以下记录说明实际核对范围，部分基础资料保留 2026-09-18 的读取日期；访问限制单独标明。上游文档与工作区版本可能不同，项目行为以源码快照为准。</p>'
    for item in audit['checks']:
        out+='<section class="audit"><h2>'+E(item['topic'])+'</h2>'+para(item['finding'])+'<p class="meta">定位：'+E(item['location'])+'</p><p>'+ ' · '.join(link(REFS[k]['url'],REFS[k]['title']) for k in item['refs'])+'</p></section>'
    out+='<h2>按题使用的资料索引</h2>'
    for k in sorted({k for q in TQ.values() for k in q['refs']}):
        r=REFS[k];out+='<section class="ref-entry" id="ref-'+k+'"><h3>'+link(r['url'],r['title'])+'</h3>'+para(r['scope'])+'<p class="meta">'+E(r['kind']+' · '+r['status'])+'</p></section>'
    return out

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--refresh-baseline',action='store_true');args=parser.parse_args()
    manifest=snapshot_sources(args.refresh_baseline)
    audit=json.loads((HERE/'research-notes.json').read_text())
    write_page('index.html','总目录',directory())
    nt=len(TQ);np=len(PQ)
    for name,title,chs,note in [('theory','第一部分 · 纯八股',THEORY,f'{nt} 道原理题 · 每题包括机制、推演、条件与追问'),('project','第二部分 · 项目面经',PROJECT,f'{np} 道项目题 · 两段口述回答 → 对应八股 → 实现证据与追问')]:
        write_page(name+'.html',title,cover(title,note)+toc(chs)+controls(chs)+chapters_html(chs,manifest))
        for c in chs:write_page(c['id']+'.html',c['title'],cover(c['title'],title)+controls([c])+chapters_html([c],manifest))
    write_page('full.html','合订本',cover('网关与节点面经 · 合订本',f'第一部分 {nt} 道原理题 · 第二部分 {np} 道项目题')+toc(THEORY+PROJECT)+controls(THEORY+PROJECT)+chapters_html(THEORY+PROJECT,manifest,True))
    write_page('coverage.html','知识覆盖表',coverage())
    write_page('references.html','资料校对',reference_page(audit))
    source=cover('源码依据',f"{len(manifest['files'])} 份带行号的只读快照，来自当前两端工作区。")
    source+='<p>这些快照用于核查实现；它们不表示作者个人独立贡献，也不表示完成硬件验证。FreeRTOS 为工程内嵌开发快照，不自行标注未经确认的发布版本。</p>'
    source+='<p>'+link('source/kernel-version.txt','FreeRTOS VERSION')+' · '+link('source/FreeRTOS-LICENSE.txt','FreeRTOS 许可')+' · '+link('source/ST-HAL-LICENSE.txt','ST HAL 许可')+' · '+link('source/gateway-LICENSE.txt','网关许可')+'</p>'
    for key,s in manifest['files'].items():source+='<p>'+link('source/'+key+'.html#L'+str(s['line']),('网关 / ' if s['repository']=='gateway' else '节点 / ')+s['path'])+'</p>'
    write_page('source-index.html','源码',source)
    report=dict(theory_questions=len(TQ),project_questions=len(PQ),theory_chapters=len(THEORY),project_chapters=len(PROJECT),source_files=len(manifest['files']),cross_references=sum(len(q['theory']) for q in PQ.values()),research_checks=len(audit['checks']),characters=sum(len(json.dumps(q,ensure_ascii=False)) for q in list(TQ.values())+list(PQ.values())),chapters=[dict(file=c['id']+'.html',questions=len(c['questions']),part=c['questions'][0]['part']) for c in THEORY+PROJECT])
    (HERE/'build-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    (HERE/'references.json').write_text(json.dumps(REFS,ensure_ascii=False,indent=2)+'\n')
    (HERE/'v3/compiled-content.json').write_text(json.dumps(dict(theory=THEORY,project=PROJECT),ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='chapters'},ensure_ascii=False,indent=2))
if __name__=='__main__':main()
