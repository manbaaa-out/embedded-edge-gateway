# 面经文档配图

`fig01.png … fig19.png` 是 [`../嵌入式软件开发面经总纲.md`](../嵌入式软件开发面经总纲.md) 里引用的配图，由对应的 `figNN.mmd`（Mermaid 源码）渲染而来。

- **改图**：编辑对应的 `figNN.mmd`，然后跑 `bash render.sh` 重新生成 PNG。
- **渲染参数**：白底、2 倍清晰度（`-b white -s 2`），CJK 字体走系统 fallback。
- 另有 `../architecture.png`（网关架构）、`../n6_node_architecture.png`（STM32 节点架构）是手绘图，非本目录生成。

| 图 | 主题 | 图 | 主题 |
|---|---|---|---|
| fig01 | 进程内存布局 | fig11 | signalfd 信号转 fd 事件 |
| fig02 | 虚函数表 vtable | fig12 | 事件组全员喂狗 |
| fig03 | 进程 vs 线程 | fig13 | UART DMA+空闲中断接收 |
| fig04 | 虚拟内存分页/TLB | fig14 | FreeRTOS 任务状态机 |
| fig05 | TCP 三次握手 | fig15 | 优先级翻转/继承 |
| fig06 | TCP 四次挥手 | fig16 | UAF 与延迟销毁 |
| fig07 | 拥塞控制四阶段 | fig17 | 串口协议 8 状态机 |
| fig08 | 粘包半包重组 | fig18 | 单一写者 + eventfd |
| fig09 | 五种 I/O 模型 | fig19 | 采集节点数据流 |
| fig10 | epoll 内核结构 | | |
