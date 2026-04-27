#!/usr/bin/env python3
# -*- coding: utf-8 -*-

with open('FontExp.map', 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

print("=== .text段原始内容（前200行）===\n")

in_text = False
count = 0
for i, line in enumerate(lines):
    if '.text           0x00201000' in line:
        in_text = True
    if in_text:
        print(line.rstrip())
        count += 1
        if count >= 200:
            break