#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re

with open('FontExp.map', 'r', encoding='utf-8', errors='ignore') as f:
    content = f.read()

print("=== .text段完整分析（296KB）===\n")

# 找到.text段范围
text_match = re.search(r'^\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)', content, re.MULTILINE)
if text_match:
    text_end_addr = int(text_match.group(1), 16) + int(text_match.group(2), 16)
    
    print(f".text段总大小: {int(text_match.group(2), 16)} bytes = {int(text_match.group(2), 16)/1024:.2f} KB")
    print()

    # 提取所有.text段内容
    sections = []
    in_text = False
    
    for line in content.split('\n'):
        if '.text           0x00201000' in line:
            in_text = True
        if in_text and line.startswith('.data'):
            break
        if in_text:
            # 匹配所有段
            match = re.match(r'\s+\.(\w+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s*(.*)', line)
            if match:
                seg_name = match.group(1)
                addr = int(match.group(2), 16)
                size = int(match.group(3), 16)
                source = match.group(4).strip() if match.group(4) else ''
                sections.append((seg_name, addr, size, source))

    # 统计LVGL相关代码
    lvgl_total = 0
    other_total = 0
    lvgl_files = {}
    other_files = {}
    
    for seg_name, addr, size, source in sections:
        if size > 0:
            # 判断是否是LVGL
            is_lvgl = False
            if 'lvgl' in source.lower():
                is_lvgl = True
            elif seg_name.startswith('lv_'):
                is_lvgl = True
            
            if is_lvgl:
                lvgl_total += size
                if source not in lvgl_files:
                    lvgl_files[source] = 0
                lvgl_files[source] += size
            else:
                other_total += size
                if source not in other_files:
                    other_files[source] = 0
                other_files[source] += size
    
    print("=== LVGL相关代码 ===")
    print(f"LVGL总计: {lvgl_total} bytes = {lvgl_total/1024:.2f} KB")
    print(f"占.text段的百分比: {lvgl_total/int(text_match.group(2), 16)*100:.2f}%")
    print()
    
    print("LVGL各文件的代码大小:")
    sorted_lvgl = sorted(lvgl_files.items(), key=lambda x: x[1], reverse=True)
    for file, size in sorted_lvgl[:20]:
        filename = file.split('/')[-1] if '/' in file else file
        print(f"  {filename:50s}: {size:8d} bytes = {size/1024:8.2f} KB")
    
    print("\n=== 其他代码 ===")
    print(f"其他代码总计: {other_total} bytes = {other_total/1024:.2f} KB")
    print(f"占.text段的百分比: {other_total/int(text_match.group(2), 16)*100:.2f}%")
    print()
    
    print("其他各文件的代码大小（前30个）:")
    sorted_other = sorted(other_files.items(), key=lambda x: x[1], reverse=True)
    for file, size in sorted_other[:30]:
        filename = file.split('/')[-1] if '/' in file else file
        print(f"  {filename:50s}: {size:8d} bytes = {size/1024:8.2f} KB")
    
    print("\n=== 总结 ===")
    print(f"LVGL代码: {lvgl_total/1024:.2f} KB")
    print(f"其他代码: {other_total/1024:.2f} KB")
    print(f"总计: {(lvgl_total+other_total)/1024:.2f} KB")
    print(f".text段总大小: {int(text_match.group(2), 16)/1024:.2f} KB")