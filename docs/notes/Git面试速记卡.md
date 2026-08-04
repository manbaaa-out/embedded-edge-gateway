# Git 工业级面试速记卡

> 用法:面试前 10 分钟扫一遍。每条「一句话答案」可直接说出口,「原理」是被追问时的底牌。
> 核心心法:面试官最爱追问「为什么」。答完 what 主动补 why,就赢了 80% 的人。

---

## 一、底层模型(最能体现深度,几乎必被追问)

**Q1. Git 底层到底存什么?**
一句话:Git 是一个内容寻址的 key-value 对象数据库,key 是内容的 SHA-1 哈希,value 是三种对象——blob(文件内容)、tree(目录清单)、commit(快照 + 元信息 + 父指针)。
原理:每次提交存的是**完整快照**而非差异;内容相同的文件哈希相同,只存一份,所以快照不浪费空间。

**Q2. 为什么 Git 的历史是防篡改的?**
一句话:commit 哈希由「内容 + tree + 父提交哈希」共同算出,改动任一历史提交,它及之后所有提交的哈希全变,篡改立刻暴露。
延伸:这也是 rebase 后「commit ID 全变了」的根因——父提交变了,哈希必然变。

**Q3. 工作区、暂存区、对象库三者关系?**
一句话:`git add` 把文件存成 blob 并登记进暂存区(index);`git commit` 把暂存区固化成 tree+commit,并移动当前分支指针。
关键坑:`commit` 只认暂存区。`add` 后又改文件却没再 `add`,提交的是 `add` 那一刻的版本。
实用:`git add -p` 可对同一文件部分提交(分块选 y/n),适合「驱动改动提交、调试 printf 留下」。

---

## 二、分支与合并

**Q4. 为什么 Git 建分支那么快?(对比 SVN)**
一句话:分支只是一个存了 40 位哈希的文本文件(`.git/refs/heads/xxx`),建分支不复制任何项目文件,所有对象由各分支共享。空间增量 ≈ 一个文件名 + 40 字符。

**Q5. merge 的两种本质?**
一句话:① fast-forward——目标分支在前方且当前分支没分叉,直接移动指针,不产生新 commit;② 三方合并——两边都有对方没有的提交,需找共同祖先(merge base),生成一个有**两个父**的 merge commit。
追问「怎么强制产生 merge commit」:`git merge --no-ff`,保留功能边界,GitFlow 常要求。

**Q6. 冲突是怎么产生的?怎么解决?**
一句话:两边改了**同一文件的同一块**,Git 不知听谁的就停下报冲突。解决 = 手动编辑删掉 `<<< === >>>` 标记、留下最终内容、`git add` + `git commit`。
关键:不强制二选一,可两边都留或改写第三种;`git merge --abort` 可中止合并回到合并前。

---

## 三、历史治理(高频,且最能拉开差距)

**Q7. merge 和 rebase 的区别?**
一句话:merge 保留分叉、产生 merge commit、历史如实记录;rebase 把提交「搬家」重接成直线、不产生 merge commit、历史干净但 commit 哈希全变。
**黄金法则(必背):绝不 rebase 已推送、别人可能基于其工作的提交**——因为改写哈希会让别人本地的旧哈希对不上,造成协作灾难。rebase 只用于整理本地未分享的提交。

**Q8. reset 三种模式的区别?**
| 模式 | 分支指针 | 暂存区 | 工作区 | 用途 |
|---|---|---|---|---|
| `--soft` | 移动 | 不动 | 不动 | 揉碎提交(改动留暂存区,重新 commit) |
| `--mixed`(默认) | 移动 | 重置 | 不动 | 取消 add(改动退回工作区) |
| `--hard` | 移动 | 重置 | 重置 | **危险!丢工作区未提交改动** |
记忆:soft/mixed 永远安全(改动还在),只有 hard 可能真丢东西。

**Q9. 不小心 `reset --hard` 删了提交怎么救?**
一句话:`git reflog` 找回那个提交的哈希,`git reset --hard <哈希>`(或 `HEAD@{n}`)让分支重新指回去。
原理:reset 不删对象、只移指针,旧 commit 变「悬空对象」,垃圾回收(默认约 30 天)前都能救。reflog 记录 HEAD 每次移动的历史,所以能找回哈希。
真正救不回的:① reflog 也过期被清;② 丢的是从未提交、只在工作区的改动(所以频繁 commit 本身就是安全网)。

---

## 四、远程协作

**Q10. origin/main 是什么?和本地 main 什么关系?**
一句话:存在三种分支——本地 `main`(你 commit 的地方)、远程仓库的 `main`(服务器上)、`origin/main`(**远程跟踪指针**,记录「上次同步时远程 main 在哪」,只读、非实时)。
`git status` 的 ahead/behind = 本地 main 与 origin/main 跟踪指针的比较结果。

**Q11. git pull 和 fetch 的区别?**
一句话:`pull` = `fetch` + `merge`(或 rebase)。`fetch` 只下载并更新 `origin/*` 跟踪指针,**绝不碰本地分支和工作区**(完全安全);`pull` 会动本地分支和工作区。
实战:想看远程动静又不影响自己工作,用 `fetch` 而非 `pull`。

**Q12. 拉取远程时撞冲突怎么处理?团队怎么避免历史污染?**
一句话:实际工程推荐 `git pull --rebase`,把本地未推送的提交接到远程最新提交后,避免一堆无意义的 merge commit。
**rebase 冲突解决流程(易错点):解决冲突 → `git add` → `git rebase --continue`(不是 `git commit`!)**。三条出路:`--continue` / `--skip` / `--abort`。
**冲突标记反转坑:rebase 时 `<<<<<<< HEAD` 上半段是「远程/目标」,下半段才是你自己**(和普通 merge 相反!)。解决时认 `<<<`/`>>>` 后的标识,别靠位置记忆。
配置默认 rebase:`git config --global pull.rebase true`。

---

## 五、工程规范

**Q13. 提交信息怎么写规范?(Conventional Commits)**
格式:`<type>(<scope>): <subject>` + 空行 + body(说 why)。
type:`feat` / `fix` / `docs` / `refactor` / `test` / `chore` / `perf` / `style`。
subject 用祈使句(`add` 不是 `added`),判断法:能填进「If applied, this commit will ___」。
关键:header 和 body 之间**必须空行**,否则 Git 把全部当 header,所有工具解析错乱。
未推送的提交写错可用 `git commit --amend` 修正(改写历史,只对未推送的用)。

**Q14. .gitignore 放什么?依据是什么?**
一句话:判断标准是「是不是源头」——能从源头自动再生的一律不进库(编译产物 `.o/.elf/.bin`、`build/`、CMake 生成物、CubeMX 中间产物)。
编译产物不能提交的三个理由:① 二进制污染 diff;② 引起虚假冲突;③ 提交后永留历史使仓库膨胀(对象库不删旧对象)。
**关键坑:.gitignore 只对未跟踪文件生效**。已提交的文件要先 `git rm --cached <file>` 移出跟踪,再让 .gitignore 拦截。
进阶:个人环境垃圾(`.DS_Store`、编辑器配置)放**全局** `core.excludesfile`,不污染项目 .gitignore。

**Q15. Git Hooks 是什么?有什么坑?**
一句话:hook 是 Git 在特定事件(commit/push 等)自动执行的脚本,返回非零则中止操作。常用 `pre-commit`(检查暂存内容)、`commit-msg`(校验提交信息格式)。
**控制机制:拦截能力完全来自退出码——`exit 1` 拦截,`exit 0` 放行,与打印了什么无关**。
两大失效原因:① 忘了 `chmod +x`(无执行权限 Git 直接跳过);② 文件名带了 `.sample` 后缀。
**最大坑:`.git/hooks/` 不进版本库、不随 clone 分发**。团队共享 hook 要用 pre-commit 框架 / husky 把配置纳入版本库。

---

## 六、分支策略(回答「你平时怎么用 Git」)

**Q16. 了解哪些工作流?个人项目该用哪个?**
- **GitHub Flow(轻量)**:main 永远可发布,任何改动开短命分支,做完合并删除。适合持续交付、个人项目、多数互联网公司。
- **GitFlow(重型)**:master/develop/feature/release/hotfix 多层分支,流程严格。适合有明确版本发布周期的传统软件。
成熟判断:**个人项目和多数现代团队用 GitHub Flow**;别上来用 GitFlow,对单人是过度工程。
配套:`git push -u origin <分支>` 的 `-u` 建立本地↔远程绑定,之后直接 `git push/pull` 不用写分支名;合并后用 `git branch -d` 删已合并的短命分支(`-D` 是强制删,危险)。

**减少冲突的工程习惯**:每天开工先 `git pull --rebase`(分叉越久冲突越惨);功能分支短命;提交粒度小而聚焦。

---

## 附:命令空间速查

```
本地三层:  工作区 --add--> 暂存区 --commit--> 本地仓库
                  <--restore--      <--reset--

跨本地/远程(只有这三个):
  git push          本地 -> 远程
  git fetch         远程 -> origin/* 跟踪指针(不碰本地分支/工作区)
  git pull          = fetch + merge/rebase(动本地)

历史治理(全在本地仓库内动指针,碰不到远程):
  reset(三模式)/ rebase / merge / reflog
  → 所以「别 rebase 推送过的东西」:推到远程后它就不只属于你了
```

---

## 临场口诀

1. 答完 what,主动补一句 why(面试官 90% 会追问,你先说就是降维打击)。
2. 被问危险操作(hard/rebase),主动说出安全边界和救援方式(reflog),体现工程审慎。
3. 不确定时说「我会先 `git status` / `git log` 看清状态再操作」——这本身就是专业习惯。
