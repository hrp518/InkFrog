#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re

with open('FontExp.map', 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

print("=== .text段完整分析（296KB）===\n")

# 找到.text段范围
text_match = None
for i, line in enumerate(lines):
    if '.text           0x00201000' in line:
        text_match = re.match(r'^\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)', line)
        break

if text_match:
    text_size = int(text_match.group(2), 16)
    
    print(f".text段总大小: {text_size} bytes = {text_size/1024:.2f} KB")
    print()

    # 提取所有.text段内容
    in_text = False
    current_func = None
    sections = []
    
    for i, line in enumerate(lines):
        if '.text           0x00201000' in line:
            in_text = True
            continue
        if in_text and line.startswith('.data'):
            break
        
        if in_text:
            # 匹配函数名行（如 .text.lv_obj_center）
            func_match = re.match(r'\s+\.text\.(\w+)\s*$', line)
            if func_match:
                current_func = func_match.group(1)
                continue
            
            # 匹配地址和大小行
            addr_match = re.match(r'\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)', line)
            if addr_match and current_func:
                addr = int(addr_match.group(1), 16)
                size = int(addr_match.group(2), 16)
                source = addr_match.group(3).strip()
                sections.append((current_func, addr, size, source))
                current_func = None
            
            # 匹配普通段（如 .text  0x0020115c  0x5c  crtbegin.o）
            normal_match = re.match(r'\s+\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)', line)
            if normal_match:
                addr = int(normal_match.group(1), 16)
                size = int(normal_match.group(2), 16)
                source = normal_match.group(3).strip()
                sections.append(('.text', addr, size, source))

    # 统计LVGL相关代码
    lvgl_total = 0
    other_total = 0
    lvgl_funcs = []
    other_funcs = []
    
    for func_name, addr, size, source in sections:
        if size > 0:
            # 判断是否是LVGL
            is_lvgl = False
            if 'lvgl' in source.lower():
                is_lvgl = True
            elif func_name.startswith('lv_'):
                is_lvgl = True
            
            if is_lvgl:
                lvgl_total += size
                lvgl_funcs.append((func_name, size, source))
            else:
                other_total += size
                other_funcs.append((func_name, size, source))
    
    print("=== LVGL相关代码 ===")
    print(f"LVGL总计: {lvgl_total} bytes = {lvgl_total/1024:.2f} KB")
    print(f"占.text段的百分比: {lvgl_total/text_size*100:.2f}%")
    print()
    
    print("LVGL函数（按大小排序，前30个）:")
    lvgl_funcs.sort(key=lambda x: x[1], reverse=True)
    for i, (func_name, size, source) in enumerate(lvgl_funcs[:30], 1):
        filename = source.split('/')[-1] if '/' in source else source
        print(f"{i:2d}. {func_name:40s} {size:6d} bytes ({size/1024:6.2f} KB) - {filename}")
    
    print("\n=== 其他代码 ===")
    print(f"其他代码总计: {other_total} bytes = {other_total/1024:.2f} KB")
    print(f"占.text段的百分比: {other_total/text_size*100:.2f}%")
    print()
    
    print("其他函数（按大小排序，前30个）:")
    other_funcs.sort(key=lambda x: x[1], reverse=True)
    for i, (func_name, size, source) in enumerate(other_funcs[:30], 1):
        filename = source.split('/')[-1] if '/' in source else source
        print(f"{i:2d}. {func_name:40s} {size:6d} bytes ({size/1024:6.2f} KB) - {filename}")
    
    print("\n=== 总结 ===")
    print(f"LVGL代码: {lvgl_total/1024:.2f} KB ({lvgl_total/text_size*100:.2f}%)")
    print(f"其他代码: {other_total/1024:.2f} KB ({other_total/text_size*100:.2f}%)")
    print(f"总计: {(lvgl_total+other_total)/1024:.2f} KB ({(lvgl_total+other_total)/text_size*100:.2f}%)")
    print(f".text段总大小: {text_size/1024:.2f} KB")
    
    # 未识别的代码
    unaccounted = text_size - (lvgl_total + other_total)
    print(f"未被识别的代码: {unaccounted/1024:.2f} KB ({unaccounted/text_size*100:.2f}%)")