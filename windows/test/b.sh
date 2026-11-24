#!/bin/sh
# windows/test/b.sh — build, with the compiler's output cut to what is worth reading (errors first)
cd "$(dirname "$0")/../.." && powershell -NoProfile -ExecutionPolicy Bypass -File windows/build.ps1 2>&1 | grep -v "misleading-indentation\|^      |\|  362 |\|In member function\|multiple non-default manifests\|^\s*$" | cut -c1-400 | grep -n "error\|Error\|built\|failed\|warning" | head -${1:-40}
