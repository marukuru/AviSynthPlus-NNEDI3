#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Translate this project's leaf x64 MASM kernels to position-independent ELF.

Keep the hand-tuned instruction sequences and Win64 register/stack convention.
GCC/Clang ms_abi calls in generated C++ wrappers adapt the System V entry points.
This is deliberately a translator for the MASM subset in these files, not a
MASM assembler. NASM checks the resulting instruction operands at build time.
The original files remain the single source for both Windows and Linux builds.
"""
import argparse
import re
from pathlib import Path

SOURCES = ("nnedi3_asm_x64.asm", "nnedi3_asm_FMA_x64.asm", "nnedi3_asm_AVX512_x64.asm")
DATA = re.compile(r"(\w+)\s+(qword|real4|sword|word|sdword|dword|byte)\s+(.+)", re.I)
PROC = re.compile(r"(\w+)\s+proc\s+public\s+frame", re.I)


def syntax(code):
    code = re.sub(r"\b([0-9][0-9a-fA-F]*)h\b", r"0x\1", code)
    for masm, nasm in (("xmmword", "oword"), ("ymmword", "yword"), ("zmmword", "zword")):
        code = re.sub(r"\b" + masm + r"\s+ptr\s*", nasm + " ", code, flags=re.I)
    code = re.sub(r"\b(byte|word|dword|qword)\s+ptr\s*", r"\1 ", code, flags=re.I)
    # Branch lengths can change with instruction encodings; let NASM choose.
    return re.sub(r"\bshort\s+", "", code, flags=re.I)


def translate(source):
    lines = source.read_text().splitlines()
    constants = set()
    for line in lines:
        if line.strip().lower() == ".code":
            break
        match = DATA.fullmatch(line.partition(";")[0].strip())
        if match:
            constants.add(match[1])
    output = ["; Generated from " + source.name + "; do not edit.", "bits 64", "default rel"]
    scope, aliases, labels, functions = None, [], [], []
    for index, line in enumerate(lines):
        code, separator, comment = line.partition(";")
        code = syntax(code.strip())
        if not code:
            output.append(";" + comment if separator else "")
            continue
        if code.lower() == ".data":
            output.append("section .rodata align=64")
            continue
        if code.lower() == ".code":
            output.append("section .text align=16")
            continue
        match = re.fullmatch(r"data\s+segment\s+align\((\d+)\)", code, re.I)
        if match:
            output.append("align " + match[1])
            continue
        if code.lower() in ("data ends", "end"):
            continue
        # Windows unwind metadata, not instructions. These leaf kernels never
        # call C++ or unwind. The ABI wrappers have normal compiler unwind info.
        if re.match(r"\.(endprolog|allocstack|pushreg|savexmm128)\b", code, re.I):
            continue
        match = PROC.fullmatch(code)
        if match:
            if scope is not None:
                raise ValueError("nested procedure in " + str(source))
            scope, aliases, labels = match[1], [], []
            functions.append(scope)
            for following in lines[index + 1:]:
                if re.match(r"\s*" + re.escape(scope) + r"\s+endp", following, re.I):
                    break
                label = re.match(r"\s*(\w+):", following)
                if label:
                    labels.append(label[1])
            output.extend(("align 16", "global nnedi3_asm_" + scope + ":function hidden", "nnedi3_asm_" + scope + ":"))
            continue
        match = re.fullmatch(r"(\w+)\s+endp", code, re.I)
        if match:
            if match[1] != scope:
                raise ValueError("mismatched procedure end in " + str(source))
            output.extend("%undef " + alias for alias in aliases)
            scope = None
            continue
        match = re.fullmatch(r"(\w+)\s+equ\s+(.+)", code, re.I)
        if match:
            output.append("%define " + match[1] + " " + match[2])
            if scope is not None:
                aliases.append(match[1])
            continue
        match = DATA.fullmatch(code)
        if match and scope is None:
            name, kind, value = match.groups()
            kind = {"qword": "dq", "real4": "dd", "sword": "dw", "word": "dw", "sdword": "dd", "dword": "dd", "byte": "db"}[kind.lower()]
            repeat = re.fullmatch(r"(\d+)\s+dup\((.*)\)", value, re.I)
            output.append(name + ": " + ("times " + repeat[1] + " " + kind + " " + repeat[2] if repeat else kind + " " + value))
            continue
        # A bare typed MASM data symbol denotes memory. default rel keeps all
        # such references RIP-relative; there are no writable global buffers.
        for constant in sorted(constants):
            code = re.sub(r"(?<![\w\[])\b" + re.escape(constant) + r"\b(?![\w\]])", "[" + constant + "]", code)
        for label in labels:
            code = re.sub(r"\b" + re.escape(label) + r"\b", "." + label, code)
        output.append(code + (" ;" + comment if separator else ""))
    if scope is not None:
        raise ValueError("unterminated procedure in " + str(source))
    output.append("section .note.GNU-stack noalloc noexec nowrite progbits")
    return "\n".join(output) + "\n", functions


def wrappers(functions, declarations):
    prototypes = {name: (result, arguments) for result, name, arguments in
                  re.findall(r'extern "C" (void|int) (\w+)\(([^;]+)\);', declarations)}
    output = ["// Generated ABI adapters; do not edit.", "#include <cstdint>"]
    for name in functions:
        result, arguments = prototypes[name]  # Fail if a new kernel has no prototype.
        names = [re.search(r"(\w+)\s*$", argument)[1] for argument in arguments.split(",")]
        output.extend((
            'extern "C" __attribute__((ms_abi)) ' + result + ' nnedi3_asm_' + name + '(' + arguments + ');',
            'extern "C" ' + result + ' ' + name + '(' + arguments + ')',
            '{ return nnedi3_asm_' + name + '(' + ', '.join(names) + '); }',
        ))
    return "\n".join(output) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    functions = []
    for filename in SOURCES:
        code, names = translate(args.source / filename)
        (args.output / filename).write_text(code)
        functions.extend(names)
    if len(set(functions)) != len(functions):
        raise ValueError("duplicate assembly entry points")
    (args.output / "KernelWrappers.cpp").write_text(wrappers(functions, (args.source / "nnedi3.cpp").read_text()))


if __name__ == "__main__":
    main()
