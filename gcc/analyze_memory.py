#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re

with open('FontExp.map', 'r', encoding='utf-8', errors='ignore') as f:
    content = f.read()

# 提取内存配置
print("=== 内存配置 ===")
for line in content.split('\n'):
    if 'Name' in line and 'Origin' in line:
        print(line)
        next_line = content.split('\n')[content.split('\n').index(line)+1]
        print(next_line)
        break

print("\n=== RAM段统计 ===")

# 统计各段的总大小
segments = {}
for line in content.split('\n'):
    match = re.match(r'^\s+\.(\w+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)', line)
    if match:
        segment = match.group(1)
        size_hex = match.group(3)
        size = int(size_hex, 16)
        if segment not in segments:
            segments[segment] = 0
        segments[segment] += size

# 按大小排序
sorted_segments = sorted(segments.items(), key=lambda x: x[1], reverse=True)

total_ram = 0x67000
print(f"RAM总大小: {total_ram} bytes ({total_ram/1024} KB)")
print(f"RAM已使用: {sum(segments.values())} bytes ({sum(segments.values())/1024} KB)")
print(f"RAM剩余: {total_ram - sum(segments.values())} bytes ({(total_ram - sum(segments.values()))/1024} KB)")
print()

print("=== 关键SRAM段（按大小排序）===")
for segment, size in sorted_segments:
    if size > 0:
        print(f"{segment:15s} {size:10d} bytes ({size/1024:8.2f} KB)")
