#!/bin/bash
if [ -z "$1" ]; then
    echo "Usage: $0 <executable_path> [args...]"
    exit 1
fi

EXE="$1"
shift # 将第一个参数移出，剩下的 $@ 传给程序

# -e cpu-clock: 解决 VM 中 0 events 问题
# -g: 开启调用栈
# -F 99: 采样频率
sudo perf record -F 99 -e cpu-clock -g -- "$EXE" "$@"
