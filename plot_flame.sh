#!/bin/bash
if [ -z "$1" ]; then
    echo "Usage: $0 <output_filename>"
    exit 1
fi

FG_DIR="/home/pzy/ann/FlameGraph"
OUT_FILE="$1"
PERF_DATA="./perf.data"

# 自动补全后缀
if [[ "$OUT_FILE" != *.svg ]]; then
    OUT_FILE="${OUT_FILE}.svg"
fi

# perf script -> 折叠栈 -> 绘制 svg
perf script -i $PERF_DATA | "$FG_DIR/stackcollapse-perf.pl" | "$FG_DIR/flamegraph.pl" > "$OUT_FILE"
