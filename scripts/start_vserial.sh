#!/bin/bash
# 在前台运行 socat，创建互联的两个 PTY，并以固定链接 /tmp/ttyV0（网关端）和
# /tmp/ttyV1（节点端）暴露给其他脚本。raw/echo=0 保证字节不受终端行规整和回显修改；
# exec 让调用 shell 直接由 socat 替代，Ctrl+C 的信号和退出状态无需额外转发。
exec socat -d -d \
    pty,raw,echo=0,link=/tmp/ttyV0 \
    pty,raw,echo=0,link=/tmp/ttyV1
