# 卡 4:Reactor 核心 — Channel/EventLoop(M7) + timerfd 定时器(M9)

> 模块:epoll 驱动的 Reactor 事件循环 + Channel 抽象 + timerfd 定时器
> 覆盖:`src/net/Channel`、`EventLoop`、timerfd 封装(namespace gateway)
> 备注:这是网关最核心、面试深挖最狠的一份。重点在"为什么这么设计",不是"用了什么 API"。

---

## 结构说明
**第一部分「纯面试」最重要**——epoll、Reactor、ET/LT、事件驱动是后端核心八股。
**第二部分「项目挂钩」**——我的 Channel/EventLoop 实现、ownership 设计、UAF 延迟销毁(这几点是我 mock 被点过"没主动讲深"的,必须练到能主动抛出来)。

---
---

# 第一部分:纯面试(通用知识,最重要)

## 主题 A:I/O 多路复用与 epoll

### A1. select / poll / epoll 的区别?epoll 为什么高效?

**答题要点**:
- **select**:fd 集合有上限(FD_SETSIZE,通常 1024);每次调用要把整个 fd 集合从用户态拷到内核态;返回后要**遍历所有 fd** 才知道哪个就绪。O(n)。
- **poll**:去掉了 fd 数量上限(用数组),但仍是每次全量拷贝 + 全量遍历。O(n)。
- **epoll**:内核维护一个事件表(`epoll_ctl` 增删改),只在 fd 状态变化时**回调**把就绪 fd 放进就绪列表;`epoll_wait` 直接返回**就绪的** fd,不用遍历全部。O(就绪数)。

**怎么答出深度**:epoll 高效的根本是**"内核帮你记住关心哪些 fd(不用每次传)" + "只返回就绪的(不用遍历全部)"**。在大量连接但同时活跃少的场景(C10K)优势巨大;但如果几乎所有 fd 都活跃,epoll 优势就不明显了——按场景。

---

### A2. epoll 的 ET(边沿触发)和 LT(水平触发)区别?各怎么用?

**答题要点**:
- **LT(Level Triggered,水平触发)**:只要 fd 还有数据可读,`epoll_wait` 就**反复**通知你。没读完下次还提醒。容错性好。
- **ET(Edge Triggered,边沿触发)**:只在 fd 状态**从无到有变化**的那一刻通知**一次**。这次不读完,不会再提醒。

**ET 的正确用法(关键考点)**:ET 模式下必须**一次把数据读干净**——循环 read **直到返回 EAGAIN**(表示内核缓冲区空了)。否则没读完的数据会"卡住",因为没有新的边沿来触发。所以 ET 必须配**非阻塞 fd**(阻塞 fd 读到没数据会卡死在 read 上)。

**措辞陷阱**:别说"ET 比 LT 快所以更好"。ET 减少了 epoll_wait 唤醒次数,但**编程更难、易漏读**。muduo 默认用 LT 就是因为 LT 更简单更安全。选 ET 是为减少唤醒,要承担"必须读到 EAGAIN"的复杂度。

---

## 主题 B:Reactor 模式

### B1. 什么是 Reactor 模式?核心组件有哪些?

**答题要点**:Reactor 是**事件驱动**的并发模式。核心:一个 **EventLoop**(事件循环)跑在一个线程里,不停 `epoll_wait` 等事件;每个被监听的 fd 配一个**事件处理器**(回调);事件就绪时,EventLoop 分发(dispatch)到对应回调处理。一句话:**"epoll_wait 等事件 → 拿到就绪 fd → 调它的回调"** 循环往复。

**怎么答出深度**:Reactor 的精髓是**单线程串行处理所有 I/O 事件**——所有回调都在 EventLoop 线程跑,所以**回调之间天然无需加锁**(同一线程串行执行)。这是它比"一连接一线程"省锁、省线程切换的根本。重活(如写库)再丢给线程池,I/O 线程本身保持轻快。

---

### B2. "one loop per thread" 是什么意思?

**答题要点**:每个线程最多跑一个 EventLoop。多核扩展时,开多个线程、每个线程一个 EventLoop(一个 epoll 实例),用某种方式(如主 Reactor accept 后分发给子 Reactor)把连接分散到各 loop。每个 loop 内部仍是单线程串行,所以**单 loop 内无锁**,跨 loop 才需要考虑同步。

**追问:别的线程想让某个 EventLoop 干件事,怎么安全地塞进去?**
不能直接在别的线程调 loop 的方法(破坏"单线程"假设)。标准做法:别的线程把任务**放进一个队列**,然后**唤醒**目标 loop(用一个专门的 fd,如 eventfd),loop 醒来后在自己线程里取队列执行。这样所有操作仍只在 loop 线程发生——**"单一写者 / 把跨线程调用转成 loop 内任务"**。

---

## 主题 C:定时器

### C1. 怎么给事件循环加"定时"能力?Linux 上用什么?

**答题要点**:用 **timerfd**——`timerfd_create` 创建一个定时器 fd,到期时这个 fd 变得"可读",于是它能像普通 fd 一样被 **epoll 监听**。这是 Linux "一切皆 fd" 的漂亮体现:定时器被统一进 I/O 多路复用,EventLoop 不用为定时单独搞一套机制。

**怎么答出深度**:点出"为什么用 timerfd 而不是 `sleep`/信号"——`sleep` 会阻塞整个 loop;信号(SIGALRM)和事件循环混用很别扭、可重入问题多。timerfd 把"时间到了"变成"fd 可读"事件,无缝融入 epoll,这是事件驱动架构里处理定时的最优解。

**追问:怎么用 timerfd 做"空闲连接超时踢出"?**
给每个连接记 `last_active` 时间戳,每次有数据更新它;timerfd 周期性触发(如每秒),扫描所有连接,把"当前时间 - last_active > 超时阈值"的连接关掉。注意监听 fd、定时器 fd 本身要豁免(它们没有"活跃"概念)。

---
---

# 第二部分:和项目挂钩(我具体怎么做的)

> 这部分几个点(ownership、延迟销毁)是我 mock 被点过"没主动讲深"的,我现在练成能主动抛出。

### P1. 你的 Channel 抽象是什么?为什么要它?

**答**:Channel 把"一个 fd + 它关心的事件 + 事件回调"打包成一个对象。EventLoop 不直接和裸 fd 打交道,而是和 Channel 打交道——epoll 就绪后,我通过 `epoll_event.data.ptr` 直接拿到对应的 Channel 指针,调它的回调。**Channel 是我整个项目复用最多的抽象**:socket 连接、timerfd、signalfd、eventfd——任何能被 epoll 监听的 fd 都做成 Channel,前后复用了 5 次。这是"找到一个正确的抽象,后面所有 fd 都套它"。

---

### P2.(高频深挖)Channel 的所有权(ownership)怎么设计的?

**答**(这点我要主动讲,不等追问):**map 持有 shared_ptr,epoll 里存裸指针(borrow)**。
- EventLoop 里一个 `map<fd, shared_ptr<Channel>>` **拥有** Channel 的生命周期。
- epoll_event.data.ptr 里存的是**裸指针**,只是"借用",不参与生命周期管理。

**为什么这么分**:epoll 是内核结构,塞不进 shared_ptr;而且"谁拥有"必须单一明确(map),epoll 只是"用一下"。这就是 **"owner 持 shared_ptr、user 持 raw ptr"** 的所有权模型——拥有权集中在一处,使用方借用。

---

### P3.(最硬的一题)回调执行到一半把 Channel 删了会怎样?你怎么防的?

**答**(这是我 mock 漏讲、现在必须主动抛的核心设计):
**问题(UAF 风险)**:EventLoop 一轮里拿到一批就绪 Channel,正在遍历调它们的回调。如果某个回调内部触发了"删除某个 Channel"(比如连接关闭),而那个 Channel 恰好在这批就绪列表里还没处理——直接从 map 删掉、释放对象,后面再访问它的裸指针就是 **UAF(Use-After-Free)**。

**我的解法——延迟销毁(delayed destruction)**:回调里要删 Channel 时,**不立即 delete**,而是把它放进一个 `dying_` 待销毁 vector;等这一轮就绪事件**全部处理完**,再统一清空 `dying_`、真正销毁。这样遍历期间对象一定活着。

**措辞陷阱(我以前答错的地方)**:防 UAF 的功劳**不是 shared_ptr**。我以前会顺口说"用 shared_ptr 所以安全",但 shared_ptr 防的是"别处还引用着不会被释放";这里的问题是"遍历当前批次时对象被移除",真正解决它的是**延迟到批次结束再销毁**这个机制。这两个要分清,这是我 mock 被点的精确性问题。

---

### P4. 你的双向 I/O(读+写)怎么做的?写不完怎么办?

**答**:读是 ET 模式循环读到 EAGAIN。写要处理"内核写缓冲满、一次写不完"的情况:每个连接有个 `out_buf`,写不完的部分留在 out_buf,**注册 EPOLLOUT 事件**;等 fd 可写时 epoll 通知,接着写剩下的;**out_buf 写空后立刻取消 EPOLLOUT**(否则可写事件会一直触发、空转)。这是"EPOLLOUT 按需开关"的标准写法。

---

### P5. timerfd 在你项目里做什么?扫描时怎么防迭代器失效?

**答**:做空闲连接超时踢出——`last_active` 时间戳 + `timeout_exempt` 标志(listen fd、timer fd 自己豁免)。timerfd 用 `CLOCK_MONOTONIC`(单调时钟,不受系统时间调整影响)+ `TFD_NONBLOCK|TFD_CLOEXEC`,包成 Channel。扫描超时连接用**两阶段**:先遍历收集所有过期 fd 到一个临时 vector,遍历结束后再逐个 removeChannel。**为什么两阶段**:边遍历 map 边 erase 会使迭代器失效。这个"先收集后删除"我在 HTTP 解析、方案 B 超时重试里又复用了同一个套路。

---

### P6. M7 mock 多少分?暴露什么?

**答**(诚实):M7 mock **78 分**。暴露的持续弱点正是 P2/P3 这种——**我不主动 surface 关键设计深度**:union/裸指针分离、map-owns/epoll-borrows 的所有权、UAF 靠延迟销毁(不是 shared_ptr)。这些我答得出但要被追问才说。所以这份卡我把它们全提到"要主动讲"的位置反复练——面试是"主动展示深度"得分,不是"被问到才挤出来"。

---

*作者:Bi(2026 秋招准备项目)*
*配套实测:M7 mock 78 分;Channel 抽象在 socket/timerfd/signalfd/eventfd 复用 5 次;ET 读到 EAGAIN + EPOLLOUT 按需开关 + 空闲超时踢出均跑通;curl/压测验证*
