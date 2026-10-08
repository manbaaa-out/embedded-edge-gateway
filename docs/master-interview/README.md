# 网关与节点面经

本版按两个独立部分重新编排，面向兼顾 Linux/C++ 网关和 STM32/FreeRTOS 的嵌入式软件岗位。

- **第一部分：120 道纯八股、13 章。** 每题解释原理、成立条件、推演或反例、后续追问，紧随资料依据。包含 8 个代码/时序/定量示例。
- **第二部分：54 道项目题、7 章。** 每题先给两段可口述的回答，再回指所用八股题，补充实现证据、追问和当前边界。共 249 处明确回指，并附上下行路径和确认阶段图。

## 直接阅读

| 内容 | 网页 | PDF |
| --- | --- | --- |
| 第一部分 · 纯八股 | [阅读](theory.html) | [下载](第一部分_纯八股.pdf) |
| 第二部分 · 项目面经 | [阅读](project.html) | [下载](第二部分_项目面经.pdf) |
| 合订本 | [全文检索](full.html) | [下载](网关与节点总面经.pdf) |

从 [总目录](index.html) 进入，也可按 `theory-01.html` 至 `theory-13.html`、`project-01.html` 至 `project-07.html` 分章阅读。网页无需服务、CDN 或联网依赖；外部参考资料需要联网。

项目网页的 T 编号直接跳到第一部分的具体问题。合订 PDF 使用文档内跳转；独立项目 PDF 使用 PDF 标准的跨文件目标，指向同目录八股 PDF 的相应页，是否自动打开取决于阅读器支持。若阅读器不支持，使用合订 PDF 或离线 HTML。请保留两份 PDF 的原文件名和相对位置。

支持问题编号精确搜索、正文关键词搜索、章节筛选和隐藏答案自测。完整包为上级目录的 `master-interview.zip`，解压后打开 `master-interview/index.html`。

## 内容范围与证据

第一部分依次覆盖 C/C++、操作系统、Linux I/O、帧协议与可靠性、TCP/MQTT/HTTP、存储与生命周期、Cortex-M3/STM32、外设与传感器、FreeRTOS、时间与低功耗、排障工程、跨模块设计及 FreeRTOS 移植专题。CAN、SPI、ADC、OTA 等扩展知识保留为原理题，没有虚构项目已实现的应用。

第二部分沿项目全景、网关并发、协议/MQTT、存储/热加载、节点采集/串口、时序/排障、FreeRTOS 移植与启动验收展开。本轮按独立管理 Reactor 与管理工作线程、多个通信循环、共享 GatewayCore 和独立 SQLite 读写执行器更新网关回答，并以 Boost.Beast/Asio 取代旧自写 HTTP 解析器的当前实现说明。回答依据截至 2026-10-08 核对的双端工作区源码；已有实现、源码推导、后续建议及未测量结果分开说明。个人贡献不由代码存在自动推断。

- [知识覆盖表](coverage.html)：项目题与对应基础知识的映射。
- [资料校对](references.html)：38 组重点机制的核对结论、资料定位与访问边界。
- [源码索引](source-index.html)：62 份保留原行号的只读快照与许可。
- [研究记录](research-notes.json)：2026-09-20/21、2026-09-24、2026-10-04 与 2026-10-08 的重点校对；其余参考保留原读取日期。
- [源码基线](source-baseline.json)：被引用文件哈希及仓库版本。

网络校对优先使用 Linux man-pages、C/C++ 标准材料、Arm/ST 与传感器原厂手册、FreeRTOS 官方源码/教材、RFC、OASIS MQTT、SQLite 和高校原始资料。只链接引用，不打包转载第三方手册。项目的 FreeRTOS 行为以工程内嵌源码为准，不推定与上游 main 或某个发行版完全相同。

## 维护与重建

正文源位于 `v3/`：

- `theory.json` 保存基础机制，`deepening.txt` 保存重新撰写的结论、推演与追问，`extra-theory.txt` 保存新增专题。
- `freertos-port-theory.txt` 保存新增的 FreeRTOS 移植八股题；`project.txt` 独立保存项目题；`A` 为口述回答，`D` 为实现证据，`F` 为追问，`B` 为边界。题头使用稳定的 T 编号回指。
- `examples.json` 保存原创教学示例。`compiled-content.json` 是生成的完整结构化正文。

`book_data.py` 合并并检查正文；`catalog.py` 管理参考与源码位置；`build.py` 构建页面，`book.css`/`book.js` 提供样式和交互。

在仓库根目录运行：

```sh
python3 docs/master-interview/build.py
node docs/master-interview/render.mjs
python3 docs/master-interview/validate.py
```

生成器使用 Python 标准库；导出需要本地 Chromium、Node.js、Poppler `pdftotext` 和 Python `pypdf`。可通过 `INTERVIEW_BROWSER_PATH` 指定 Chromium。`pdf_links.py` 用 pypdf 把项目 PDF 的回指改为相对文件名和准确页码，避免携带当前机器的绝对路径。

源码哈希变更时，先审阅受影响回答，再用 `build.py --refresh-baseline` 更新快照。导出后实际查看网页、手机排版及 PDF 抽样页，记录 `visual-review.json`，最后运行 `python3 docs/master-interview/package.py`。

在隔离发布目录中核对指定源码时，可设置 `GATEWAY_SOURCE_ROOT` 与 `STM32_SOURCE_ROOT`；默认分别为本仓库根目录与旁边的 `STM32Project`。覆盖目录只改变源码取证位置，生成物仍写入本面经目录，刷新前仍需审阅源码差异。

验证包含两部分隔离、问题数量、249 个回指、页面本地链接、源码哈希、PDF 逐段文本、题目顺序、桌面/手机布局和检索交互。验证不等同于应用程序测试、固件上板或性能测量。
