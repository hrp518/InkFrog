#!/usr/bin/env python3
# -*- coding: utf-8 -*-

def unicode_to_utf8(code_point):
    """将Unicode码点转换为UTF-8字节序列"""
    return chr(code_point).encode('utf-8')

# 检查 level-1.txt 文件
with open('level-1.txt', 'rb') as f:
    content = f.read()

# 查找 0x5570 (唰) 的UTF-8编码
target = unicode_to_utf8(0x5570)
print(f'查找 0x5570 (唰) 的UTF-8编码: {target.hex()}')
if target in content:
    idx = content.index(target)
    print(f'找到 0x5570 (唰) 在文件中的位置: {idx}')
    print(f'行号 (1-based): {content[:idx].count(b"\n") + 1}')
    lines = content.split(b'\n')
    line_num = content[:idx].count(b'\n')
    start = max(0, line_num - 2)
    end = min(len(lines), line_num + 3)
    print(f'\n前后几行内容:')
    for i in range(start, end):
        try:
            line_text = lines[i].decode('utf-8')
        except:
            line_text = str(lines[i])
        print(f'  {i+1}: {line_text}')
else:
    print('未在文件中找到 0x5570 (唰)')

print('\n' + '='*60 + '\n')

# 查找 0x77AD (箍) 的UTF-8编码
target = unicode_to_utf8(0x77AD)
print(f'查找 0x77AD (箍) 的UTF-8编码: {target.hex()}')
if target in content:
    idx = content.index(target)
    print(f'找到 0x77AD (箍) 在文件中的位置: {idx}')
    print(f'行号 (1-based): {content[:idx].count(b"\n") + 1}')
    lines = content.split(b'\n')
    line_num = content[:idx].count(b'\n')
    start = max(0, line_num - 2)
    end = min(len(lines), line_num + 3)
    print(f'\n前后几行内容:')
    for i in range(start, end):
        try:
            line_text = lines[i].decode('utf-8')
        except:
            line_text = str(lines[i])
        print(f'  {i+1}: {line_text}')
else:
    print('未在文件中找到 0x77AD (箍)')
