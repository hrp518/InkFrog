#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import re

with open('FontExp.map', 'r', encoding='utf-8', errors='ignore') as f:
    content = f.read()

print("=== .text段库文件占用统计 ===\n")

# 找到.text段范围
text_match = re.search(r'^\.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)', content, re.MULTILINE)
if text_match:
    text_start = int(text_match.group(1), 16)
    text_size = int(text_match.group(2), 16)
    
    print(f".text段大小: {text_size} bytes = {text_size/1024:.2f} KB")
    print()
    
    # 统计各个库的代码大小
    libraries = {}
    
    for line in content.split('\n'):
        # 匹配所有.text段（包括.text和.text.xxx）
        match = re.match(r'\s+\.text\S*\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)', line)
        if match:
            addr = int(match.group(1), 16)
            size = int(match.group(2), 16)
            source = match.group(3).strip()
            
            # 检查是否在.text段范围内
            if text_start <= addr < text_start + text_size and size > 0:
                # 提取库名
                if '.a(' in source:
                    # 格式: ../../lib/libxxx.a(yyy.o)
                    lib_match = re.search(r'lib([^/\\]+)\.a\(', source)
                    if lib_match:
                        lib_name = lib_match.group(1)
                    else:
                        lib_name = 'unknown_lib'
                elif '/' in source:
                    # 普通目标文件
                    lib_name = 'app_code'
                else:
                    lib_name = 'other'
                
                if lib_name not in libraries:
                    libraries[lib_name] = 0
                libraries[lib_name] += size

    print("=== 各库代码大小（按大小排序）===\n")
    
    # 按大小排序
    sorted_libs = sorted(libraries.items(), key=lambda x: x[1], reverse=True)
    
    total = 0
    for lib_name, size in sorted_libs:
        total += size
        pct = (size / text_size) * 100
        print(f"{lib_name:30s}: {size:8d} bytes = {size/1024:8.2f} KB ({pct:5.1f}%)")
    
    print(f"\n{'总计':30s}: {total:8d} bytes = {total/1024:8.2f} KB")
    print(f"{'.text段总大小':30s}: {text_size:8d} bytes = {text_size/1024:8.2f} KB")
    
    # 未识别的代码
    unaccounted = text_size - total
    print(f"{'未被识别的代码':30s}: {unaccounted:8d} bytes = {unaccounted/1024:8.2f} KB")