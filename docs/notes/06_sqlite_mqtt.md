# 卡 6:存储与消息 — SQLite(M12) + MQTT 客户端(M10)

> 模块:SQLite 本地落库 + libmosquitto MQTT 客户端封装
> 覆盖:`src/db/`(Statement/Database)、`src/mqtt/`(MqttClient),namespace gateway

---

## 结构说明
**第一部分「纯面试」最重要**——SQLite 用法陷阱、prepared statement、C 回调转 C++、MQTT 基础是通用知识。
**第二部分「项目挂钩」**——我的 RAII 封装、回调 trampoline、数据链路接法、UAF 析构顺序。

---
---

# 第一部分:纯面试(通用知识,最重要)

## 主题 A:SQLite 与 prepared statement

### A1. 为什么用 prepared statement(预编译语句)而不是直接拼 SQL 字符串?

**答题要点**:两个理由。
1. **防 SQL 注入**:参数用占位符 `?` 绑定,数据和 SQL 结构分离,用户输入永远当数据不当代码。拼字符串则有注入风险。
2. **性能**:预编译一次、多次复用(`bind` 新参数 + `reset` 重置),省掉每次解析 SQL 的开销。高频插入时收益明显。

---

### A2. SQLite 的 C API 用起来有哪些经典陷阱?

**答题要点**(几个高频坑):
1. **bind 参数索引从 1 开始,column 取值索引从 0 开始**——两边不一致,记混了就错位。
2. **绑定字符串要用 `SQLITE_TRANSIENT`**:告诉 SQLite "拷贝一份这个字符串",否则 SQLite 只存指针,你的字符串先析构了它就读到野指针。
3. **column 返回的指针生命周期只到下次 `step` 或 `reset`**:`sqlite3_column_text` 返回的指针,下次 step 后就失效,要用必须立刻拷走。
4. **statement 复用要 `reset`**:执行完一次要 `sqlite3_reset` 才能 bind 新参数再执行,否则状态残留。

**怎么答出深度**:这些坑的共性是**生命周期/所有权不清晰**——C API 不帮你管内存,你得清楚每个指针活多久。用 RAII 包一层(Statement 类)能把这些坑封在内部。

---

### A3. SQLite 的 WAL 模式是什么?为什么用它?

**答题要点**:WAL(Write-Ahead Logging,预写日志)是 SQLite 的一种日志模式。默认的 rollback journal 模式下读写互斥;WAL 模式下**读和写可以并发**(写追加到 WAL 文件,读还能读主库),写入也更快(顺序追加)。配合 `synchronous=NORMAL` 在性能和安全间取平衡。适合"写多 + 要并发读"的场景。

---

## 主题 B:C 回调转 C++ 成员函数(通用技巧)

### B1. C 库(如 libmosquitto)要求传一个 C 函数指针做回调,但你想调用 C++ 对象的成员函数,怎么办?

**答题要点**:**静态 trampoline(蹦床)函数**。C 库的回调注册一般允许你传一个 `void* userdata`。技巧:
1. 注册回调时把 `this`(对象指针)作为 userdata 传进去。
2. 回调函数本身写成 **static 成员函数或自由函数**(签名匹配 C 要求,没有隐式 this)。
3. 回调被触发时,把 userdata 转回 `MyClass*`,再调用真正的成员函数 / `std::function`。

```cpp
static void on_message_trampoline(void* obj, const Message* msg) {
    static_cast<MqttClient*>(obj)->handleMessage(msg);  // 转回对象,调成员函数
}
```

**怎么答出深度**:这是 C 和 C++ 互操作的通用模式——C 函数指针装不下"对象 + 方法"(成员函数有隐式 this),所以用 static 函数做跳板、靠 userdata 把 this 带进来。很多 C 库(pthread、libevent、信号处理)都用这套。

---

## 主题 C:MQTT 基础

### C1. MQTT 是什么?核心概念有哪些?

**答题要点**:MQTT(Message Queuing Telemetry Transport,消息队列遥测传输)是轻量级的**发布-订阅(pub/sub)** 消息协议,为低带宽、不稳定网络的物联网设计。核心:
- **Broker**(消息代理):中心服务器,转发消息。
- **Topic**(主题):消息的层级地址(如 `gateway/cmd/query_light`),发布者发到 topic,订阅者订 topic。
- **QoS**(服务质量):0 最多一次(可能丢)、1 至少一次(可能重)、2 恰好一次。
- 发布者和订阅者**解耦**,互相不知道对方,只通过 broker 和 topic 交互。

**追问:payload 处理有什么坑?**
MQTT 的 payload **不保证以 `\0` 结尾**(它是带长度的二进制 blob)。当字符串处理时不能直接 `printf("%s")`,要用 `%.*s` + payloadlen,或者按 payloadlen 拷贝。而且回调里的 payload 指针**回调返回后就失效**,要用必须立刻拷走。

---
---

# 第二部分:和项目挂钩(我具体怎么做的)

### P1. 你的 Statement 和 Database 类怎么设计的?

**答**:`Statement` 是 RAII 包 `sqlite3_stmt`——构造时 prepare、析构时 finalize,提供 bind(从 1)、step、column(从 0),复用时 reset。把第一部分 A2 那四个坑(索引差异、SQLITE_TRANSIENT、column 指针生命周期、reset 复用)全封在类内部。`Database` 是 RAII 连接 + 一把 mutex 串行化访问 + 缓存一个 prepared insert 语句复用 + 开 WAL + synchronous=NORMAL pragma。

---

### P2. 你的 MqttClient 怎么封装 libmosquitto 的?

**答**:RAII 包 libmosquitto 句柄。回调用第一部分 B1 的 static trampoline:注册时把 `this` 当 obj 传进去,trampoline 里转回来调 `std::function` 回调。提供三种 loop 模式。payload 处理严格按 `%.*s` + payloadlen(不当 null 结尾),且在回调里**立刻拷贝** payload(因为回调返回后指针失效)。

---

### P3. 这两个怎么串成数据链路?

**答**:"MQTT → 线程池 → SQLite 落库"。MqttClient 收到消息(I/O,在 mosquitto 的 loop 线程)→ 立刻拷出 payload、`submit` 给线程池 → worker 线程调 Database 写库。I/O 线程不碰慢的写盘,worker 才落库。两个模块都做成 INTERFACE 库整理进 `src/db/`、`src/mqtt/`,在 main.cpp 里接起来。

---

### P4.(高频)这条链路你踩过什么 UAF 坑?

**答**:**析构顺序导致的 task UAF**。worker 任务里持有对 db 的引用去写库;`main` 里对象析构是声明的逆序,如果 db 先于 pool 析构,而 pool 析构时 worker 可能还在跑没干完的任务、还在用 db → UAF。解法:**用声明顺序控制析构顺序**——db 声明在 pool 前面(后声明的 pool 先析构、先 join 完 worker),client 放 try 块内。保证析构顺序 client → pool → db。我还把 db、client 改成 shared_ptr 进一步防 task 持有的对象提前释放。

**这能讲出的点**:RAII 下"谁后声明谁先析构"必须和"谁被依赖谁后析构"对齐。依赖链是 worker→db,所以 db 必须活得比 pool 久,就要比 pool 先声明。这是 C++ 对象生命周期管理的实战考点。

---

### P5. 端到端验证?

**答**:跑通了"MQTT 发消息 → 线程池 → SQLite 落库"完整链路。还特意测了容错:发一个坏 payload(数字字段是非法字符串),被 `std::stod` 的异常处理跳过、不崩库、不污染数据。

---

*作者:Bi(2026 秋招准备项目)*
*配套实测:M12/M10 各产出 12 卡;端到端 MQTT→线程池→SQLite 落库通过;坏 payload 被异常处理跳过;WAL+NORMAL pragma 生效*
