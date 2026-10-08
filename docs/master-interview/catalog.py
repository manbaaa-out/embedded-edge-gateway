"""Primary reading links and source locations for the interview book.

Online references are background verification; project-specific claims are grounded
in the checked-out source snapshots, not in the newest upstream release.
"""
import os
from pathlib import Path

ROOT = Path(os.environ.get('GATEWAY_SOURCE_ROOT', Path(__file__).resolve().parents[2])).resolve()
NODE = Path(os.environ.get('STM32_SOURCE_ROOT', ROOT.parent / 'STM32Project')).resolve()
KERNEL = 'Middlewares/Third_Party/FreeRTOS-Kernel/'

# alias: repository, relative path, line-selection token
SOURCES = {
    'app': ('gateway','src/gateway/app/GatewayApp.cpp','GatewayApp::run()'),
    'apph': ('gateway','src/gateway/app/GatewayApp.h','class GatewayApp'),
    'core': ('gateway','src/gateway/app/GatewayCore.cpp','void GatewayCore::handleMqtt('),
    'coreh': ('gateway','src/gateway/app/GatewayCore.h','class GatewayCore'),
    'executor': ('gateway','src/gateway/core/concurrent/TaskExecutor.h','class TaskExecutor'),
    'admin': ('gateway','src/gateway/app/ManagementReactor.cpp','void ManagementReactor::onSignals('),
    'adminh': ('gateway','src/gateway/app/ManagementReactor.h','class ManagementReactor'),
    'main': ('gateway','src/gateway/app/main.cpp','int main('),
    'loop': ('gateway','src/gateway/io/event/EventLoop.cpp','void EventLoop::loop()'),
    'link': ('gateway','src/gateway/link/NodeLink.cpp','void NodeLink::flushOutput()'),
    'serial': ('gateway','src/gateway/io/serial/SerialPort.cpp','void SerialPort::configure'),
    'tracker': ('gateway','src/gateway/link/CommandTracker.cpp','bool CommandTracker::canSubmit'),
    'proto': ('gateway','protocol/include/edge_proto/edge_proto.h','#define EDGE_ACK_TIMEOUT_MS'),
    'sr': ('gateway','protocol/src/edge_sr.c','static void deliver('),
    'frame': ('gateway','protocol/src/edge_frame.c','edge_parser_feed'),
    'crc': ('gateway','protocol/src/edge_crc16.c','uint16_t'),
    'queue': ('gateway','src/gateway/core/concurrent/ThreadSafeQueue.h','std::optional<T> pop()'),
    'pipe': ('gateway','src/gateway/pipeline/TelemetryPipeline.cpp','void TelemetryPipeline::writerLoop'),
    'pipeh': ('gateway','src/gateway/pipeline/TelemetryPipeline.h','kQueueCapacity ='),
    'db': ('gateway','src/gateway/storage/Database.h','void insertBatch('),
    'mqtt': ('gateway','src/gateway/cloud/MqttClient.cpp','mosquitto_new('),
    'translator': ('gateway','src/gateway/cloud/CommandTranslator.cpp','translateCommand('),
    'http': ('gateway','src/gateway/io/http/HttpServer.cpp','class Session :'),
    'httph': ('gateway','src/gateway/io/http/HttpServer.h','struct HttpRequest'),
    'config': ('gateway','src/gateway/core/config/Config.cpp','ConfigManager::reload('),
    'log': ('gateway','src/gateway/core/log/AsyncLogger.cpp','void AsyncLogger::append('),
    'cmake': ('gateway','CMakeLists.txt','cmake_minimum_required'),
    'deploy': ('gateway','deploy/gateway.service.in','[Service]'),
    'nodeapp': ('node','App/app.c','void app_start('),
    'uart': ('node','App/uart_link.c','void HAL_UARTEx_RxEventCallback('),
    'tx': ('node','App/tx_service.c','void vTxTask('),
    'txh': ('node','App/tx_service.h','#include'),
    'cmd': ('node','App/cmd_service.c','void vCmdTask('),
    'sensor': ('node','App/sensor_hub.c','static void read_selected('),
    'report': ('node','App/report_task.c','void vSampleReportTask('),
    'watch': ('node','App/watchdog.c','void watchdog_kick_if_all_checked_in('),
    'dht': ('node','BSP/dht11.c','uint8_t DHT11_Read('),
    'bh': ('node','BSP/bh1750.c','static HAL_StatusTypeDef bh1750_read_once('),
    'bhh': ('node','BSP/bh1750.h','#define BH1750_ADDR'),
    'dwt': ('node','BSP/dwt_delay.c','void DWT_Init('),
    'nmain': ('node','Core/Src/main.c','void SystemClock_Config(void)\n{'),
    'nioc': ('node','N6_freertos.ioc','NVIC.PriorityGroup=NVIC_PRIORITYGROUP_4'),
    'halcfg': ('node','Core/Inc/stm32f1xx_hal_conf.h','#define  USE_RTOS'),
    'nirq': ('node','Core/Src/stm32f1xx_it.c','void HardFault_Handler('),
    'dma': ('node','Core/Src/dma.c','void MX_DMA_Init('),
    'usart': ('node','Core/Src/usart.c','void MX_USART1_UART_Init('),
    'gpio': ('node','Core/Src/gpio.c','void MX_GPIO_Init('),
    'i2c': ('node','Core/Src/i2c.c','void MX_I2C1_Init('),
    'iwdg': ('node','Core/Src/iwdg.c','hiwdg.Init.Prescaler'),
    'haltick': ('node','Core/Src/stm32f1xx_hal_timebase_tim.c','HAL_StatusTypeDef HAL_InitTick('),
    'haluart': ('node','Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c','void HAL_UART_IRQHandler('),
    'rtcfg': ('node','Core/Inc/FreeRTOSConfig.h','#define configTICK_RATE_HZ'),
    'rtport': ('node',KERNEL+'portable/GCC/ARM_CM3/port.c','void xPortPendSVHandler( void )\n{'),
    'rttasks': ('node',KERNEL+'tasks.c','BaseType_t xTaskIncrementTick('),
    'rtqueue': ('node',KERNEL+'queue.c','BaseType_t xQueueGenericSend('),
    'rtstream': ('node',KERNEL+'stream_buffer.c','size_t xStreamBufferSendFromISR('),
    'rtheap': ('node',KERNEL+'portable/MemMang/heap_4.c','void * pvPortMalloc('),
    'rttimers': ('node',KERNEL+'timers.c','static portTASK_FUNCTION( prvTimerTask,'),
    'rtevents': ('node',KERNEL+'event_groups.c','EventBits_t xEventGroupSetBits('),
    'rtcmake': ('node','cmake/freertos.cmake','target_sources('),
    'startup': ('node','startup_stm32f103xb.s','Reset_Handler:\n'),
    'ld': ('node','STM32F103XX_FLASH.ld','MEMORY'),
}

REFS = {}
def ref(key, title, url, scope, kind='官方资料', status='已访问'):
    REFS[key] = dict(title=title, url=url, scope=scope, kind=kind, status=status)

ref('interview','牛客：大疆嵌入式工程师面经（作者复盘）','https://ac.nowcoder.com/discuss/794763','题目覆盖与项目追问方式；经验帖不作为技术标准或公司必问题库。','面经选题')
ref('xiaolin','小林 coding：操作系统面试题','https://www.xiaolincoding.com/interview/os.html','章节组织和高频问题检索；技术结论另以官方手册核对。','面经选题')
man = 'https://man7.org/linux/man-pages/'
for key, path, title, scope in [
 ('epoll','man7/epoll.7.html','epoll(7)','就绪集合、ET/LT、EAGAIN 与事件生命周期。'),
 ('eventfd','man2/eventfd.2.html','eventfd(2)','计数器、非阻塞读写与合并通知。'),
 ('timerfd','man2/timerfd_create.2.html','timerfd_create(2)','时钟选择、到期计数与 fd 通知。'),
 ('signalfd','man2/signalfd.2.html','signalfd(2)','信号掩码、线程继承与 fd 读取。'),
 ('pthreads','man7/pthreads.7.html','pthreads(7)','进程内共享与线程独立状态。'),
 ('process','man2/fork.2.html','fork(2)：进程资源','进程隔离与继承。'),
 ('syscalls','man2/syscalls.2.html','syscalls(2)','系统调用接口与内核边界。'),
 ('read','man2/read.2.html','read(2)','读取返回值、EINTR、EAGAIN。'),
 ('write','man2/write.2.html','write(2)','短写、错误与完成边界。'),
 ('mmap','man2/mmap.2.html','mmap(2)','虚拟内存与文件映射。'),
 ('proc','man5/proc.5.html','proc(5)','进程、内存与运行期诊断入口。'),
 ('condvar','man3/pthread_cond_wait.3p.html','pthread_cond_wait(3p)','谓词、原子解锁等待与虚假唤醒。'),
 ('mutex','man3/pthread_mutex_lock.3p.html','pthread_mutex_lock(3p)','锁所有者、阻塞和错误语义。'),
 ('sched','man7/sched.7.html','sched(7)','普通与实时调度策略。'),
 ('select','man2/select.2.html','select(2)','集合、描述符表示限制。'),
 ('poll','man2/poll.2.html','poll(2)','就绪查询与接口差别。'),
 ('termios','man3/termios.3.html','termios(3)','串口原始模式、VMIN/VTIME 和 tcdrain。'),
 ('signal','man7/signal.7.html','signal(7)','信号上下文与屏蔽。'),
 ('tcpman','man7/tcp.7.html','tcp(7)','Linux TCP 状态、缓冲与选项。'),
 ('fork','man2/fork.2.html','fork(2)','写时复制和多线程 fork 限制。'),
 ('exec','man3/exec.3.html','exec(3)','进程映像替换。'),
 ('ipc','man7/sysvipc.7.html','sysvipc(7)','IPC 家族；与管道/socket 选型结合。'),
 ('open','man2/open.2.html','open(2)','fd 与 open file description。'),
 ('dup','man2/dup.2.html','dup(2)','共享打开状态与 fd 标志。'),
 ('close','man2/close.2.html','close(2)','关闭、fd 复用和错误处理。'),
]: ref(key,title,man+path,scope)
ref('eevdf','Linux Kernel：EEVDF Scheduler','https://docs.kernel.org/scheduler/sched-eevdf.html','调度实现版本边界，不预设目标机内核。')
ref('irq','Linux Kernel：Generic IRQ handling','https://docs.kernel.org/core-api/genericirq.html','硬中断与延后处理的上下文。')
ref('kernelmemory','Linux Kernel：Memory barriers','https://docs.kernel.org/core-api/wrappers/memory-barriers.html','缓存、访问排序与设备交互的层次。')
ref('cppguide','C++ Core Guidelines','https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines','R.1/资源管理、CP/并发与接口生命周期。')
for key, path, title, scope in [
 ('cppatomics','atomics.order','C++ 工作草案：原子内存序','release/acquire/relaxed 与可见性。'),
 ('cppmemory','util.smartptr.shared','C++ 工作草案：shared_ptr','共享所有权及线程安全边界。'),
 ('cppmove','forward','C++ 工作草案：move / forward','值类别转换与移动语义。'),
 ('cppvector','vector.modifiers','C++ 工作草案：vector modifiers','扩容、擦除和失效规则。'),
 ('cpplambda','expr.prim.lambda.capture','C++ 工作草案：lambda capture','捕获形式与生命周期。'),
 ('cppnew','expr.new','C++ 工作草案：new','对象构造和存储分配。'),
 ('cppinline','dcl.inline','C++ 工作草案：inline','语言定义与优化内联的区别。'),
 ('cppvirtual','class.virtual','C++ 工作草案：virtual functions','运行期多态与覆盖。'),
]: ref(key,title,'https://eel.is/c++draft/'+path,scope+' 项目按 C++17，未引入新标准专属特性。','标准草案')
ref('cstandard','WG14 N1570：C11 委员会草案','https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf','6.2/对象表示和链接、6.5/表达式、7.24/字符串内存接口。','标准草案')
ref('volatile','GCC：Volatiles','https://gcc.gnu.org/onlinedocs/gcc/Volatiles.html','volatile 的访问约束，不提供普通内存同步。')
ref('gccstages','GCC：Overall Options','https://gcc.gnu.org/onlinedocs/gcc/Overall-Options.html','预处理、编译、汇编与链接阶段。')
ref('gnuld','GNU ld：Linker Scripts','https://sourceware.org/binutils/docs/ld/Scripts.html','段布局、装载地址与运行地址。')
ref('sanitizers','GCC：Instrumentation Options','https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html','ASan/UBSan/TSan 的用途与限制。')
for key, num, title, scope in [
 ('tcp','9293','TCP','握手、序号、字节流和关闭；§3.4–3.8。'),
 ('tcpcc','5681','TCP Congestion Control','拥塞窗口、慢启动与拥塞避免。'),
 ('http11','9112','HTTP/1.1','消息定界、正文长度和连接管理；§6–§9。'),
 ('tls','8446','TLS 1.3','认证、完整性与传输保护的边界。'),
 ('rfc1982','1982','Serial Number Arithmetic','通用模序号比较背景；项目 SR 具体策略以源码为准。'),
]: ref(key,'RFC '+num+'：'+title,'https://datatracker.ietf.org/doc/html/rfc'+num,scope,'协议标准')
ref('mqttspec','OASIS MQTT 3.1.1','https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html','§3.1.2 会话/遗嘱/Keep Alive、§3.3 Retain、§4.3 QoS。','协议标准')
ref('mosquitto','Eclipse Mosquitto：C API','https://mosquitto.org/api/files/mosquitto-h.html','publish 返回值、网络线程、回调与重连。')
ref('modbus','Modbus Organization：Serial Line V1.02','https://www.modbus.org/file/secure/modbusoverserial.pdf','串行 CRC 附录；本项目只使用 CRC 变体，不是 Modbus 协议。','协议标准')
for key,path,title,scope in [
 ('sqlitewal','wal.html','Write-Ahead Logging','读写并发、checkpoint 与 WAL 保留。'),
 ('sqlitethread','threadsafe.html','Using SQLite in multi-threaded applications','连接线程模式与应用事务归属。'),
 ('sqlitetrans','lang_transaction.html','Transaction','事务边界与提交失败。'),
 ('sqlitepragma','pragma.html#pragma_synchronous','PRAGMA synchronous','WAL/NORMAL 与断电持久性。'),
 ('sqlitequery','queryplanner.html','Query Planning','组合索引、过滤与排序。'),
 ('sqlitebind','c3ref/bind_blob.html','Binding values to prepared statements','绑定数据与语句执行。'),
]: ref(key,'SQLite：'+title,'https://www.sqlite.org/'+path,scope)
ref('rm0008','ST RM0008：STM32F1 参考手册','https://www.st.com/resource/en/reference_manual/rm0008-stm32f101xx-stm32f102xx-stm32f103xx-stm32f105xx-and-stm32f107xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf','RCC、GPIO、DMA、TIM、USART、I2C、IWDG/WWDG 等章节；配置同时核对本地 HAL。','芯片手册','官方检索已核对；全文超过浏览抓取大小限制')
ref('pm0056','ST PM0056：Cortex-M3 编程手册','https://www.st.com/resource/en/programming_manual/pm0056-stm32f10xxx20xxx21xxxl1xxxx-cortexm3-programming-manual-stmicroelectronics.pdf','异常、寄存器、NVIC、SysTick 和故障状态；Rev 7。','芯片手册')
ref('stm32ds','ST STM32F103x8/xB 数据手册','https://www.st.com/resource/en/datasheet/stm32f103c8.pdf','存储规格、时钟与电气参数范围。','芯片手册')
ref('stboot','ST AN2606：System memory boot mode','https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf','启动模式背景；自定义 OTA 回滚是本文设计扩展，并非引用本手册宣称已实现。','应用笔记')
ref('sthal','ST 官方 stm32f1xx HAL UART 源码','https://github.com/STMicroelectronics/stm32f1xx-hal-driver/blob/master/Src/stm32f1xx_hal_uart.c','HAL_UART_IRQHandler / ReceiveToIdle 与驱动状态；项目结论以离线本地版本为准。','原厂源码')
ref('i2cspec','NXP UM10204：I2C-bus specification','https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/nxp-designs/931/1/UM10204.pdf','§3.1 信号、ACK/时钟拉伸/总线清理，§7 上拉计算。','总线规范')
ref('dht11','奥松 DHT11 V1.3 产品手册','https://www.aosong.com/userfiles/files/media/DHT11-V1_3%E8%AF%B4%E6%98%8E%E4%B9%A6%EF%BC%88%E8%AF%A6%E7%BB%86%E7%89%88%EF%BC%89.pdf','启动、位时序、校验与采样间隔；实物版本应另核对。','原厂手册')
ref('bh1750','ROHM BH1750FVI 原厂数据手册（分销商镜像）','https://nettigo.eu/attachments/628','原厂 PDF 的转换时间、默认测量系数与命令；项目换算和等待值另核对本地驱动。','原厂手册镜像','已核对检索内容；PDF 抓取暂返回 502')
fr='https://www.freertos.org/Documentation/02-Kernel/'
for key,path,title,scope in [
 ('rtisr','03-Supported-devices/04-Demos/ARM-Cortex/RTOS-Cortex-M3-M4','Cortex-M interrupt priorities','优先级编码、BASEPRI 与可调用 ISR 范围。'),
 ('rtstreamdoc','02-Kernel-features/04-Stream-and-message-buffers/02-Stream-buffer-example','Stream / message buffers','单写者单读者与字节流语义。'),
 ('rtnotify','02-Kernel-features/03-Direct-to-task-notifications/01-Task-notifications','Task notifications','通知槽、计数/事件语义和接收者。'),
 ('rtheapdoc','02-Kernel-features/09-Memory-management/01-Memory-management','Memory management','heap_1..5 与静态分配。'),
 ('rtmutex','02-Kernel-features/02-Queues-mutexes-and-semaphores/04-Mutexes','Mutexes','优先级继承及任务上下文。'),
 ('rtdelay','04-API-references/02-Task-control/02-vTaskDelayUntil','vTaskDelayUntil','相对与绝对周期延时。'),
 ('rtstack','04-API-references/03-Task-utilities/04-uxTaskGetStackHighWaterMark','Stack high-water mark','水位单位与观测范围。'),
 ('rttimerdoc','02-Kernel-features/05-Software-timers/01-Software-timers','Software timers','定时器服务任务与回调约束。'),
 ('rteventdoc','02-Kernel-features/06-Event-groups','Event groups','事件位与等待任意/全部。'),
 ('rtkernel','02-Kernel-features/01-Tasks-and-co-routines/01-Tasks-overview','Tasks','任务状态与调度。'),
 ('rtlowpower','02-Kernel-features/07-Lower-power-support','Low power support','Tickless 进入、唤醒与 Tick 补偿。'),
]: ref(key,'FreeRTOS：'+title,fr+path,scope,'内核官方文档','官方页面已访问；页面动态渲染，机制另核对本地内核源码')
ref('rtqueueapi','FreeRTOS 官方 queue.h 接口契约','https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/include/queue.h','队列元素复制、发送等待与 ISR 接口。','内核官方源码')
ref('rtcritical','FreeRTOS 官方 task.h 接口契约','https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/include/task.h','临界区、调度器挂起与阻塞限制。','内核官方源码')
ref('systemd','systemd 官方 systemd.service 手册源码','https://github.com/systemd/systemd/blob/main/man/systemd.service.xml','重启、停止超时与服务状态；官网手册访问受限时采用官方源码。')
ref('systemdexec','systemd 官方 systemd.exec 手册源码','https://github.com/systemd/systemd/blob/main/man/systemd.exec.xml','运行身份、目录权限和沙箱配置。')
ref('perf','Linux Kernel：Perf events','https://www.kernel.org/doc/html/latest/admin-guide/perf-security.html','性能事件采样与权限；性能诊断方案由本文组织。')
ref('strace','strace 官方文档入口','https://strace.io/','系统调用、信号与进程跟踪。')
ref('gdb','GNU GDB 官方手册','https://sourceware.org/gdb/current/onlinedocs/gdb.html/','断点、栈、寄存器、远程调试与目标现场。')
ref('cmakedoc','CMake：cmake-buildsystem','https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html','构建目标、依赖与使用要求。')

# Revision 2: primary reading locations for the standalone theory sections.
for i,title in [(3,'Heap Memory Management'),(4,'Task Management'),(5,'Queue Management'),(6,'Software Timer Management'),(7,'Interrupt Management'),(8,'Resource Management'),(9,'Event Groups'),(10,'Task Notifications')]:
    ref('rtbook'+str(i),'FreeRTOS 官方内核书：'+title,
        f'https://github.com/FreeRTOS/FreeRTOS-Kernel-Book/blob/main/ch{i:02d}.md',
        f'第 {i} 章；用于通用机制。具体节点实现另对照随包内核源码，不推定与上游 main 完全相同。','内核官方教材','2026-09-18 已检索/读取对应章节')
ref('cmsisnvic','Arm CMSIS：NVIC 接口与优先级',
    'https://arm-software.github.io/CMSIS_6/main/Core/group__NVIC__gr.html',
    'Priority Grouping、NVIC_SetPriority、__NVIC_PRIO_BITS；CMSIS 逻辑值与寄存器编码。')
ref('cmsisreg','Arm CMSIS：内核寄存器',
    'https://arm-software.github.io/CMSIS_6/main/Core/group__Core__Register__gr.html',
    '__get_MSP / __get_PSP / __set_BASEPRI / __set_PRIMASK；具体 M3 异常帧对照 PM0056。')
ref('dmaapp','ST AN2548：DMA 控制器原理',
    'https://www.st.com/resource/en/application_note/cd00160362.pdf',
    'DMA 请求、总线争用与传输模式；具体通道和完成语义对照 RM0008 及本地 HAL。','应用笔记','2026-09-18 官方索引可检索；PDF 直接抓取超时')
ref('adcapp','ST AN2834：ADC 精度',
    'https://www.st.com/resource/en/application_note/an2834-how-to-get-the-best-adc-accuracy-in-stm32-microcontrollers-stmicroelectronics.pdf',
    'Rev 10，采样电容、源阻抗、参考电压与噪声；适用参数按具体芯片数据手册。','原厂应用笔记','2026-09-18 已读取')
ref('canintro','TI SLOA101B：CAN 总线原理',
    'https://www.ti.com/lit/an/sloa101b/sloa101b.pdf',
    '§4.1 仲裁、§4.4 故障限制、§5 物理总线；用于经典 CAN，不套用 CAN FD 的数据长度。','原厂应用笔记','2026-09-18 已读取')
ref('canarb','CiA：CAN 数据链路层',
    'https://www.can-cia.org/can-knowledge/can-cc',
    '经典 CAN 的消息、仲裁和错误处理；CAN 控制器与应用层完成边界分开。','行业组织资料','阅读入口，须结合具体控制器手册')
ref('falsesharing','Linux Kernel：False Sharing',
    'https://docs.kernel.org/kernel-hacking/false-sharing.html',
    'What is false sharing、How to detect、Possible Mitigations；缓存行争用与测量。','内核官方文档','2026-09-18 已读取')
ref('httpsem','RFC 9110：HTTP 幂等语义',
    'https://www.rfc-editor.org/rfc/rfc9110.html#section-9.2.2',
    '§9.2.2 Idempotent Methods，约束预期效果，并非要求所有响应内容相同。','协议标准','2026-09-18 已读取')
ref('tcprto','RFC 6298：TCP RTO 计算',
    'https://www.rfc-editor.org/rfc/rfc6298.html',
    '§2 RTT/波动估计、§3 重传样本、§5 定时器管理；作为重试基础，自定义 SR 参数以项目为准。','协议标准','阅读入口')
ref('arq','MIT 6.02：可靠传输与 ARQ 课程原稿',
    'https://web.mit.edu/6.02/www/f2011/handouts/20.pdf',
    '第 20 章 Stop-and-Wait、Sliding Window 与 ACK；项目窗口与缓存寿命由本地实现给出。','高校课程原稿','阅读入口')
ref('mcuboot','MCUboot 官方设计文档',
    'https://docs.mcuboot.com/design.html',
    'Image validation、Image trailers、Test images / image confirmation；用于解释掉电恢复和试运行，节点未实现 OTA。','项目官方设计文档','阅读入口')
ref('rtportonline','FreeRTOS 官方 ARM_CM3 移植源码',
    'https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/portable/GCC/ARM_CM3/port.c',
    'SVC/PendSV、xPortSysTickHandler、vPortSuppressTicksAndSleep；上游参考，当前行为以本地快照为准。','内核官方源码','已访问并结合本地移植读取')
ref('rtstreamonline','FreeRTOS 官方 StreamBuffer 接口契约',
    'https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/include/stream_buffer.h',
    '单写者/单读者、trigger level 与 Send/Receive 返回值；以版本适用性为前提。','内核官方源码','阅读入口')
REFS['bh1750'].update(url='https://dfimg.dfrobot.com/enshop/image/data/SEN0097/BH1750FVI.pdf',
    title='ROHM BH1750FVI 原厂手册（DFRobot 镜像）',
    scope='原厂 PDF，电气特性表及 I2C Bus Access / measurement adjustment；120 ms 典型、180 ms 最大值与默认系数。镜像为较早修订，实物版本应核对。',
    status='2026-09-18 已读取 PDF 并核对对应表格')

for key in ('arq','mcuboot','canarb','tcprto','rtstreamonline'):
    REFS[key]['status']='2026-09-18 已访问并核对对应内容'
ref('arqsr','MIT 16.36：GBN 与 Selective Repeat',
    'https://ocw.mit.edu/courses/16-36-communication-systems-engineering-spring-2009/resources/mit16_36s09_lec18/',
    'Lecture 18 原始课件：GBN/SR 的窗口、缓存与序号空间；结合 Q041 的小序号反例理解。','高校课程原稿','2026-09-18 已访问课程资料入口')
ref('futex','Linux futex(7)：用户态快速路径与内核等待',
    'https://man7.org/linux/man-pages/man7/futex.7.html',
    '用户态原子操作、竞争时等待/唤醒；futex 是基础机制，并非完整互斥量接口。','官方接口手册','阅读入口')

REFS['arqsr'].update(url='https://ocw.mit.edu/courses/16-36-communication-systems-engineering-spring-2009/9b06e175ee2b383dc35490581a45267a_MIT16_36s09_lec18.pdf',status='2026-09-18 已读取原始课件，SRP Rules 明列 M≥2W')
REFS['futex']['status']='2026-09-18 已读取'
REFS['dht11']['scope']='V1.3_20170331：表 4/图 3–6 时序；§6 读取间隔与前次测量值；§7 温度小数及负号、湿度小数为 0。实物版本应核对。'

# Revision 3: two independent parts; web checks dated separately from source facts.
ref('little','MIT 1.041/1.200：排队模型与 Little 定律',
    'https://web.mit.edu/1.041/www/lectures/L8-queuing-models-2026sp.pdf',
    'Spring 2026，Stationary analysis and Little’s law；平均关系不能代替最坏突发容量证明。','高校课程原稿','2026-09-21 已检索并读取')
ref('rta','University of York：固定优先级响应时间分析',
    'https://www.cs.york.ac.uk/rts/documents/thesis/emberson09.pdf',
    'Paul Emberson 博士论文，2009，§2.2.2，式 2.3–2.5；本文示例采用单核固定优先级、无释放抖动的受限模型。','研究论文','2026-09-21 已读取并核对公式与适用条件')
REFS['pm0056']['url']='https://www.st.com/resource/en/programming_manual/pm0056-stm32f10xxx20xxx21xxxl1xxxx-cortexm3-programming-manual-stmicroelectronics.pdf'
REFS['pm0056']['status']='2026-09-20 已读取 Rev 7（2024-12），156 页'
REFS['arqsr']['scope']='Lecture 18，SRP Rules：M≥2W；区分一般循环序号与具体应用的会话内不复用策略。'
REFS['bh1750']['status']='2026-09-21 已读取 ROHM Rev.B 原厂 PDF 镜像并复核参数'
REFS['dht11']['status']='2026-09-21 已复核 V1.3 的读取间隔、前次测量、温度小数与符号'
for key in ('epoll','condvar','cppatomics','cppmemory','volatile','futex','eventfd','timerfd','signalfd','tcprto','arq','arqsr','mqttspec','http11','tcp','sqlitethread','sqlitetrans','sqlitepragma','sqlitewal','sqlitequery','cmsisreg','i2cspec','rtbook5','rtbook7','rtbook8','rtportonline','rtstreamonline'):
    REFS[key]['status']='2026-09-20 检索并核对机制；具体校对点见 research-notes.json'
REFS['cppvector']['status']='2026-09-21 已复核 vector.modifiers 的失效与异常条件'
REFS['systemd']['status']='2026-09-21 已复核官方手册源码 Restart 条件'

for key in ('rtbook3','rtbook4','rtbook6','rtbook9','rtbook10'):
    REFS[key]['status']='2026-09-21 已访问官方教材；调度、堆和定时器命令机制已复核，项目细节以本地内核为准'

# Multi-loop / third-party HTTP revision; verify project details against local source.
ref('asioimpl','Boost.Asio：平台实现与事件分派',
    'https://www.boost.org/latest/doc/html/boost_asio/overview/implementation.html',
    'Linux 的 epoll 后端与可选 io_uring；仅说明上游机制，具体配置以本地编译为准。','库官方文档','2026-10-04 已检索并核对')
ref('asiorun','Boost.Asio：io_context::run',
    'https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/io_context/run.html',
    '只有执行 run/poll 系列接口的线程分派处理函数；多个 run 调用者构成线程池。','库官方文档','2026-10-04 已检索并核对')
ref('beastasync','Boost.Beast：异步 HTTP 服务端示例',
    'https://github.com/boostorg/beast/blob/develop/example/http/server/async/http_server_async.cpp',
    'listener/session、异步 accept/read/write 与响应对象寿命；这是库示例，不代表应用已实现所有示例功能。','库官方源码','2026-10-04 已检索并核对')
