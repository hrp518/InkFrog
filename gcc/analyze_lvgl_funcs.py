#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re

with open('FontExp.map', 'r', encoding='utf-8', errors='ignore') as f:
    content = f.read()

print("=== .text段中LVGL函数统计 ===\n")

# 找到.text段范围
text_match = re.search(r'^\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)', content, re.MULTILINE)
if text_match:
    text_start = int(text_match.group(1), 16)
    text_size = int(text_match.group(2), 16)
    
    print(f".text段范围: 0x{text_start:X} - 0x{text_start+text_size:X}")
    print(f".text段大小: {text_size} bytes = {text_size/1024:.2f} KB")
    print()
    
    # 统计LVGL函数
    lvgl_funcs = []
    other_funcs = []
    
    for line in content.split('\n'):
        # 匹配所有.text.xxx段
        match = re.match(r'\s+\.text\.(\w+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)', line)
        if match:
            func_name = match.group(1)
            addr = int(match.group(2), 16)
            size = int(match.group(3), 16)
            source = match.group(4).strip()
            
            # 检查是否在.text段范围内
            if text_start <= addr < text_start + text_size:
                # 判断是否是LVGL
                if 'lvgl' in source.lower() or 'lv_' in source.lower() or func_name.startswith('lv_'):
                    lvgl_funcs.append((func_name, size, source))
                else:
                    other_funcs.append((func_name, size, source))
    
    print(f"LVGL函数: {len(lvgl_funcs)} 个")
    print(f"其他函数: {len(other_funcs)} 个")
    print()
    
    print("=== LVGL函数（按大小排序，前40个）===")
    lvgl_funcs.sort(key=lambda x: x[1], reverse=True)
    lvgl_total = 0
    for i, (func_name, size, source) in enumerate(lvgl_funcs[:40], 1):
        filename = source.split('/')[-1] if '/' in source else source
        lvgl_total += size
        print(f"{i:2d}. {func_name:40s} {size:6d} bytes ({size/1024:6.2f} KB) - {filename}")
    
    print(f"\nLVGL函数总计: {lvgl_total} bytes = {lvgl_total/1024:.2f} KB")
    print()
    
    print("=== 其他函数（按大小排序，前40个）===")
    other_funcs.sort(key=lambda x: x[1], reverse=True)
    other_total = 0
    for i, (func_name, size, source) in enumerate(other_funcs[:40], 1):
        filename = source.split('/')[-1] if '/' in source else source
        other_total += size
        print(f"{i:2d}. {func_name:40s} {size:6d} bytes ({size/1024:6.2f} KB) - {filename}")
    
    print(f"\n其他函数总计: {other_total} bytes = {other_total/1024:.2f} KB")
    print()
    
    print(f"=== 总计 ===")
    print(f"LVGL + 其他: {lvgl_total + other_total} bytes = {(lvgl_total + other_total)/1024:.2f} KB")
    print(f".text段总大小: {text_size} bytes = {text_size/1024:.2f} KB")
    
    # 未识别的代码
    unaccounted = text_size - (lvgl_total + other_total)
    print(f"未被识别的代码: {unaccounted} bytes = {unaccounted/1024:.2f} KB")
