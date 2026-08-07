# link 层测试:在途命令表的全部时序路径,通过注入假时钟避免真实等待
gateway_add_test(command_tracker_test
    SOURCES CommandTrackerTest.cpp
    LIBS    gw_link
    LABELS  "unit;link"
)
