# link 层测试:在途命令表的全部时序路径(注入假时钟,毫秒内跑完)
gateway_add_test(command_tracker_test
    SOURCES CommandTrackerTest.cpp
    LIBS    gw_link
    LABELS  "unit;link"
)
