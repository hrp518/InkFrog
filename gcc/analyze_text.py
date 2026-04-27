#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re

with open('FontExp.map', 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

print("=== .text段详细分析（SRAM代码段）===\n")

# .text段范围
text_start = 0x00201000
text_end = 0x002052c8  # 0x00201000 + 0x4a2c8

print(f".text段范围: 0x{text_start:X} - 0x{text_end:X}")
print(f".text段大小: {text_end - text_start} bytes = {(text_end - text_start)/1024:.2f} KB")
print()

# 统计LVGL相关代码
lvgl_code = []
other_code = []

for line in lines:
    match = re.match(r'\s+\.text\.(\w+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)', line)
    if match:
        func_name = match.group(1)
        addr = int(match.group(2), 16)
        size = int(match.group(3), 16)
        source = match.group(4).strip()

        # 检查是否在.text段范围内
        if text_start <= addr < text_end:
            if 'lvgl' in source.lower() or 'lv_' in source.lower():
                lvgl_code.append((func_name, size, source))
            else:
                other_code.append((func_name, size, source))

print(f"LVGL相关函数: {len(lvgl_code)} 个")
print(f"其他代码: {len(other_code)} 个")
print()

print("=== LVGL相关函数（前30个）===")
lvgl_code.sort(key=lambda x: x[1], reverse=True)
for i, (func_name, size, source) in enumerate(lvgl_code[:30], 1):
    filename = source.split('/')[-1] if '/' in source else source
    print(f"{i:2d}. {func_name:30s} {size:6d} bytes ({size/1024:6.2f} KB) - {filename}")

lvgl_total = sum(size for _, size, _ in lvgl_code)
print(f"\nLVGL代码总计: {lvgl_total} bytes = {lvgl_total/1024:.2f} KB")

print("\n=== 其他代码（前30个）===")
other_code.sort(key=lambda x: x[1], reverse=True)
for i, (func_name, size, source) in enumerate(other_code[:30], 1):
    filename = source.split('/')[-1] if '/' in source else source
    print(f"{i:2d}. {func_name:30s} {size:6d} bytes ({size/1024:6.2f} KB) - {filename}")

other_total = sum(size for _, size, _ in other_code)
print(f"\n其他代码总计: {other_total} bytes = {other_total/1024:.2f} KB")

print(f"\n=== 总计 ===")
print(f"LVGL + 其他: {lvgl_total + other_total} bytes = {(lvgl_total + other_total)/1024:.2f} KB")
print(f"%.text段总大小: {text_end - text_start} bytes = {(text_end - text_start)/1024:.2f} KB")
