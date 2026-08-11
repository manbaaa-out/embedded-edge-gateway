# link 层测试:在途命令表的全部时序路径,通过注入假时钟避免真实等待
gateway_add_test(command_tracker_test
    SOURCES CommandTrackerTest.cpp
    LIBS    gw_link
    LABELS  "unit;link"
)

# reopen 的强异常保证 —— 热加载"失败而不致命"的地基。用 pty 当真串口,
# 因为 SerialPort 要对 fd 做 tcgetattr/tcsetattr,普通文件过不了。
gateway_add_test(node_link_reopen_test
    SOURCES NodeLinkReopenTest.cpp
    LIBS    gw_link
    LABELS  "unit;link"
)
