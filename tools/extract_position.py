#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
extract_position.py - 从长文本中抽取指定百分比位置附近的片段

用途: 为 FontExp 阅读器做鲁棒性测试 (深位置阅读 / 书签恢复 / 生僻字 L1 覆盖)。
百分比按【字符数】计算 (中文文本按字节算会被 UTF-8 三字节汉字扭曲)。

用法:
    python extract_position.py <文件> [百分比=85] [窗口字符数=2000]

示例:
    python extract_position.py 白鹿原.txt 85 2000

输出:
    - <原名>_p85.txt : 片段内容 (UTF-8, 无 BOM)
    - 控制台: 总长度、目标位置、片段的字符/字节偏移、预览
"""
import os
import re
import sys

# "第X章/节/回/卷" 标题行: 行首 + 汉字/数字序号 + 章节字 (+ 可选标题)
CHAPTER_RE = re.compile(
    r'(?m)^[ \t　]*第[0-9零一二三四五六七八九十百千两]+[章节回卷][^\n]{0,30}$'
)


def read_text(path):
    """按 utf-8 / gb18030 依次尝试解码, 返回 (文本, 编码名)。"""
    with open(path, 'rb') as f:
        raw = f.read()
    for enc in ('utf-8-sig', 'utf-8', 'gb18030'):
        try:
            return raw.decode(enc), enc
        except UnicodeDecodeError:
            continue
    raise SystemExit('无法识别文件编码 (尝试过 utf-8 / gb18030)')


def align_to_block(text, start, end, chapter_lookback=600):
    """片段起点若紧邻章节标题行(<=600字符)则对齐到标题, 否则对齐到行首;
    终点对齐行尾。不为了找章节把窗口拉长 —— 深位置才是测试目标。"""
    head_start = max(0, start - chapter_lookback)
    best = None
    for m in CHAPTER_RE.finditer(text, head_start, start):
        best = m                      # 取距离 start 最近的一个
    if best is not None:
        return best.start(), end
    nl = text.rfind('\n', 0, start)   # 对齐最近的行首
    if nl > 0 and start - nl <= 200:
        start = nl + 1
    nl2 = text.find('\n', end)        # 终点对齐行尾
    if nl2 != -1:
        end = nl2
    return start, end


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    path = sys.argv[1]
    pct = float(sys.argv[2]) if len(sys.argv) > 2 else 85.0
    window = int(sys.argv[3]) if len(sys.argv) > 3 else 2000

    text, enc = read_text(path)
    total = len(text)
    if total == 0:
        raise SystemExit('空文件')

    target = int(total * pct / 100)
    start = max(0, target - window // 2)
    end = min(total, start + window)
    start = max(0, end - window)          # 窗口贴文件尾时往前挪
    start, end = align_to_block(text, start, end)
    excerpt = text[start:end]

    out_path = os.path.splitext(path)[0] + '_p%d.txt' % round(pct)
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(excerpt)

    byte_total = len(text.encode('utf-8'))
    byte_start = len(text[:start].encode('utf-8'))
    byte_end = len(text[:end].encode('utf-8'))
    print('源文件      : %s (编码 %s)' % (path, enc))
    print('总长度      : %d 字符 / %d 字节 (UTF-8)' % (total, byte_total))
    print('%.1f%% 位置 : 第 %d 个字符' % (pct, target))
    print('片段范围    : 字符 [%d, %d) 共 %d 字符' % (start, end, end - start))
    print('片段字节偏移: [%d, %d) (若设备书签按 UTF-8 字节计)' % (byte_start, byte_end))
    print('输出文件    : %s' % out_path)
    preview = excerpt[:100].replace('\n', ' ')
    print('预览        : %s%s' % (preview, '...' if len(excerpt) > 100 else ''))


if __name__ == '__main__':
    main()
