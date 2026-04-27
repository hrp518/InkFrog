#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys
import os

def convert_txt_to_c_array(input_file, output_header, output_source, array_name):
    with open(input_file, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    codes = []
    for line in lines:
        line = line.strip()
        if line:
            code = ord(line[0])
            if 0x4e00 <= code <= 0x9fff:
                if code not in codes:
                    codes.append(code)
    
    with open(output_header, 'w', encoding='utf-8') as f:
        f.write(f'#ifndef {array_name.upper()}_DATA_H\n')
        f.write(f'#define {array_name.upper()}_DATA_H\n\n')
        f.write('#include <stdint.h>\n\n')
        f.write(f'extern const uint32_t {array_name}[];\n')
        f.write(f'extern const int {array_name}_count;\n\n')
        f.write('#endif\n')
    
    with open(output_source, 'w', encoding='utf-8') as f:
        f.write(f'#include "{os.path.basename(output_header)}"\n\n')
        f.write(f'const uint32_t {array_name}[] = {{\n')
        
        for i, code in enumerate(codes):
            if i % 10 == 0:
                f.write('    ')
            f.write(f'0x{code:04x}, ')
            if (i + 1) % 10 == 0:
                f.write('\n')
        
        f.write(f'\n}};\n\n')
        f.write(f'const int {array_name}_count = {len(codes)};\n')
    
    print(f"Generated {array_name}: {len(codes)} characters")
    return len(codes)

if __name__ == "__main__":
    if len(sys.argv) != 5:
        print("Usage: convert_font_levels.py <input.txt> <output.h> <output.c> <array_name>")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_header = sys.argv[2]
    output_source = sys.argv[3]
    array_name = sys.argv[4]
    
    count = convert_txt_to_c_array(input_file, output_header, output_source, array_name)
    print(f"Successfully converted {input_file} to C arrays with {count} characters")
