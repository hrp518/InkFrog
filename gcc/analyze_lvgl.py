#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re

with open('FontExp.map', 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

print("=== LVGL内存占用分析 ===\n")

# 统计LVGL各段的大小
lvgl_stats = {
    'text': 0,
    'data': 0,
    'bss': 0,
    'rodata': 0,
    'total': 0
}

# 详细列表
lvgl_details = []

for i, line in enumerate(lines):
    # 匹配格式: .text.xxx  0x地址  0x大小  文件名
    match = re.match(r'\s+\.(text|data|bss|rodata)\S*\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)', line)
    if match:
        seg_type = match.group(1)
        addr = int(match.group(2), 16)
        size = int(match.group(3), 16)
        source = match.group(4).strip()
        
        # 检查是否是LVGL相关
        if 'lvgl' in source.lower() or 'lv_' in source.lower():
            lvgl_stats[seg_type] += size
            lvgl_stats['total'] += size
            if size > 100:  # 只记录大于100字节的
                lvgl_details.append((seg_type, size, source))

print("LVGL各段占用:")
print(f"  .text (代码): {lvgl_stats['text']:8d} bytes = {lvgl_stats['text']/1024:8.2f} KB")
print(f"  .data (数据): {lvgl_stats['data']:8d} bytes = {lvgl_stats['data']/1024:8.2f} KB")
print(f"  .bss  (BSS) : {lvgl_stats['bss']:8d} bytes = {lvgl_stats['bss']/1024:8.2f} KB")
print(f"  .rodata     : {lvgl_stats['rodata']:8d} bytes = {lvgl_stats['rodata']/1024:8.2f} KB")
print(f"  ----------------------------------------")
print(f"  总计         : {lvgl_stats['total']:8d} bytes = {lvgl_stats['total']/1024:8.2f} KB")

print("\n=== LVGL占用最大的模块（前20个）===\n")
# 按大小排序
lvgl_details.sort(key=lambda x: x[1], reverse=True)
for i, (seg_type, size, source) in enumerate(lvgl_details[:20], 1):
    # 提取文件名
    filename = source.split('/')[-1] if '/' in source else source
    print(f"{i:2d}. [{seg_type:6s}] {size:8d} bytes ({size/1024:6.2f} KB) - {filename}")

# 统计所有SRAM占用
print("\n=== SRAM总体占用分析 ===\n")

sram_total = 0
sram_segments = {}

for line in lines:
    # 匹配SRAM中的段（地址在0x00201000-0x00268000范围内）
    match = re.match(r'\s+\.(\w+)\S*\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)', line)
    if match:
        seg_name = match.group(1)
        addr = int(match.group(2), 16)
        size = int(match.group(3), 16)
        
        # 检查是否在SRAM范围内
        if 0x00201000 <= addr < 0x00268000:
            if seg_name not in sram_segments:
                sram_segments[seg_name] = 0
            sram_segments[seg_name] += size
            sram_total += size

print(f"SRAM总占用: {sram_total} bytes = {sram_total/1024:.2f} KB")
print(f"SRAM总大小: 412 KB")
print(f"SRAM剩余:   {(412*1024 - sram_total)} bytes = {(412*1024 - sram_total)/1024:.2f} KB")

print("\nSRAM各段占用:")
for seg_name, size in sorted(sram_segments.items(), key=lambda x: x[1], reverse=True):
    if size > 0:
        print(f"  {seg_name:15s}: {size:8d} bytes = {size/1024:8.2f} KB")
