#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
./truss_linux ../总控信息/必做题_总控信息.txt ../计算结果/必做题_机算结果.txt
./truss_linux ../总控信息/选做题_总控信息.txt ../计算结果/选做题_机算结果.txt
printf '两题计算完成，结果已保存到计算结果目录。\n'
