#!/bin/bash
# 启动一对虚拟串口,固定符号链接到 /tmp/ttyV0 和 /tmp/ttyV1
# socat 会一直挂在前台运行,Ctrl+C 即可停止
exec socat -d -d \
    pty,raw,echo=0,link=/tmp/ttyV0 \
    pty,raw,echo=0,link=/tmp/ttyV1