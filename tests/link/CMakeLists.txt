# command_tracker_test 链接 gw_link，并以可控时钟验证序号、重试与超时状态转换。
gateway_add_test(command_tracker_test
    SOURCES CommandTrackerTest.cpp
    LIBS    gw_link
    LABELS  "unit;link"
)

# node_link_reopen_test 依赖 Linux PTY，以真实 tty 验证串口重开和失败回滚。
gateway_add_test(node_link_reopen_test
    SOURCES NodeLinkReopenTest.cpp
    LIBS    gw_link
    LABELS  "unit;link"
)
