#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re

with open('FontExp.map', 'r', encoding='utf-8', errors='ignore') as f:
    content = f.read()

print("=== SRAM .text段详细分析（296KB）===\n")

# 找到.text段范围
text_match = re.search(r'^\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)', content, re.MULTILINE)
if text_match:
    text_start = int(text_match.group(1), 16)
    text_size = int(text_match.group(2), 16)
    
    print(f".text段起始地址: 0x{text_start:X}")
    print(f".text段大小: {text_size} bytes = {text_size/1024:.2f} KB")
    print()

    # 统计各个模块的代码大小
    modules = {}
    
    for line in content.split('\n'):
        # 匹配所有.text.xxx段
        match = re.match(r'\s+\.text\S*\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)', line)
        if match:
            addr = int(match.group(1), 16)
            size = int(match.group(2), 16)
            source = match.group(3).strip()
            
            # 检查是否在.text段范围内
            if text_start <= addr < text_start + text_size and size > 0:
                # 提取模块名
                if '/' in source:
                    module = source.split('/')[-1].replace('.o', '')
                else:
                    module = source.replace('.o', '')
                
                if module not in modules:
                    modules[module] = {'lvgl': 0, 'other': 0}
                
                # 判断是否是LVGL
                if 'lvgl' in source.lower() or 'lv_' in source.lower() or module.startswith('lv_'):
                    modules[module]['lvgl'] += size
                else:
                    modules[module]['other'] += size

    print("=== 各模块代码大小（LVGL vs 其他）===\n")
    
    # 计算
    lvgl_total = 0
    other_total = 0
    
    # 按总大小排序
    sorted_modules = sorted(modules.items(), key=lambda x: x[1]['lvgl'] + x[1]['other'], reverse=True)
    
    for module, sizes in sorted_modules[:40]:
        total = sizes['lvgl'] + sizes['other']
        if total > 0:
            lvgl_total += sizes['lvgl']
            other_total += sizes['other']
            if sizes['lvgl'] > 0 or sizes['other'] > 500:  # 只显示LVGL或大于500字节的
                print(f"{module:30s}: LVGL={sizes['lvgl']:6d}B  Other={sizes['other']:6d}B  Total={total:6d}B")
    
    print(f"\n=== 总结 ===")
    print(f"LVGL代码总计: {lvgl_total} bytes = {lvgl_total/1024:.2f} KB")
    print(f"其他代码总计: {other_total} bytes = {other_total/1024:.2f} KB")
    print(f"总计: {lvgl_total + other_total} bytes = {(lvgl_total + other_total)/1024:.2f} KB")
    print(f".text段总大小: {text_size} bytes = {text_size/1024:.2f} KB")
    
    # 未识别的代码
    unaccounted = text_size - (lvgl_total + other_total)
    print(f"未被识别的代码: {unaccounted} bytes = {unaccounted/1024:.2f} KB")