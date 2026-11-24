#!/usr/bin/env python3
"""pop2json - build isa/isa.json from the z/Architecture Principles of Operation.

    tools/pop2json.py SA22-7832-14.pdf > isa/isa.json

The instruction table is derived ONLY from IBM's Principles of Operation
(SA22-7832): nothing is read from any assembler or disassembler source.
The following parts of the book are used:

  * Appendix B, "Instructions Arranged by Mnemonic": mnemonic, name, format,
    facility code and operation code of every instruction.
  * The "Format" line of every instruction description in chapters 7-26
    (e.g. "ADD HALFWORD   AH  R1,D2(X2,B2)  [RX-a]"): the operand syntax,
    including optional trailing operands written in brackets.
  * Figure 5-1, "Basic Instruction Formats": bit positions of the fields (kept
    in FORMATS below).
  * Appendix J, "List of Extended Mnemonics": extended mnemonics.

POP does not say in its syntax lines whether an "R1" is a general, floating
point, access or control register; REG_RULES below records that knowledge
(floating-point instructions are those of chapters 18-20 plus a few of
chapter 9; the others are listed by hand).

This is a development-time tool (needs python3 and pdftotext from poppler);
the generated isa/isa.json is committed so building zosasm needs neither.
"""
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# --------------------------------------------------------------------------
# Figure 5-1: field -> (bit position, width); the operation code and unused
# fields are not listed.
# --------------------------------------------------------------------------
def _f(**kw):
    return kw

FORMATS = {
    'E': {},
    'I': {'I': (8, 8)},
    'IE': {'I1': (24, 4), 'I2': (28, 4)},
    'MII': {'M1': (8, 4), 'RI2': (12, 12), 'RI3': (24, 24)},
    'RI-a': {'R1': (8, 4), 'I2': (16, 16)},
    'RI-b': {'R1': (8, 4), 'RI2': (16, 16)},
    'RI-c': {'M1': (8, 4), 'RI2': (16, 16)},
    'RIE-a': {'R1': (8, 4), 'I2': (16, 16), 'M3': (32, 4)},
    'RIE-b': {'R1': (8, 4), 'R2': (12, 4), 'RI4': (16, 16), 'M3': (32, 4)},
    'RIE-c': {'R1': (8, 4), 'M3': (12, 4), 'RI4': (16, 16), 'I2': (32, 8)},
    'RIE-d': {'R1': (8, 4), 'R3': (12, 4), 'I2': (16, 16)},
    'RIE-e': {'R1': (8, 4), 'R3': (12, 4), 'RI2': (16, 16)},
    'RIE-f': {'R1': (8, 4), 'R2': (12, 4), 'I3': (16, 8), 'I4': (24, 8), 'I5': (32, 8)},
    'RIE-g': {'R1': (8, 4), 'M3': (12, 4), 'I2': (16, 16)},
    'RIL-a': {'R1': (8, 4), 'I2': (16, 32)},
    'RIL-b': {'R1': (8, 4), 'RI2': (16, 32)},
    'RIL-c': {'M1': (8, 4), 'RI2': (16, 32)},
    'RIS': {'R1': (8, 4), 'M3': (12, 4), 'B4': (16, 4), 'D4': (20, 12), 'I2': (32, 8)},
    'RR': {'R1': (8, 4), 'M1': (8, 4), 'R2': (12, 4)},
    'RRD': {'R1': (16, 4), 'R3': (24, 4), 'R2': (28, 4)},
    'RRE': {'R1': (24, 4), 'R2': (28, 4)},
    'RRF-a': {'R3': (16, 4), 'M4': (20, 4), 'R1': (24, 4), 'R2': (28, 4)},
    'RRF-b': {'R3': (16, 4), 'M4': (20, 4), 'R1': (24, 4), 'R2': (28, 4)},
    'RRF-c': {'M3': (16, 4), 'M4': (20, 4), 'R1': (24, 4), 'R2': (28, 4)},
    'RRF-d': {'M3': (16, 4), 'M4': (20, 4), 'R1': (24, 4), 'R2': (28, 4)},
    'RRF-e': {'M3': (16, 4), 'M4': (20, 4), 'R1': (24, 4), 'R2': (28, 4)},
    'RRS': {'R1': (8, 4), 'R2': (12, 4), 'B4': (16, 4), 'D4': (20, 12), 'M3': (32, 4)},
    'RS-a': {'R1': (8, 4), 'R3': (12, 4), 'B2': (16, 4), 'D2': (20, 12)},
    'RS-b': {'R1': (8, 4), 'M3': (12, 4), 'B2': (16, 4), 'D2': (20, 12)},
    'RSI': {'R1': (8, 4), 'R3': (12, 4), 'RI2': (16, 16)},
    'RSL-a': {'L1': (8, 4), 'B1': (16, 4), 'D1': (20, 12)},
    'RSL-b': {'L2': (8, 8), 'B2': (16, 4), 'D2': (20, 12), 'R1': (32, 4), 'M3': (36, 4)},
    'RSY-a': {'R1': (8, 4), 'R3': (12, 4), 'B2': (16, 4), 'DL2': (20, 12), 'DH2': (32, 8)},
    'RSY-b': {'R1': (8, 4), 'M3': (12, 4), 'B2': (16, 4), 'DL2': (20, 12), 'DH2': (32, 8)},
    'RX-a': {'R1': (8, 4), 'X2': (12, 4), 'B2': (16, 4), 'D2': (20, 12)},
    'RX-b': {'M1': (8, 4), 'X2': (12, 4), 'B2': (16, 4), 'D2': (20, 12)},
    'RXE': {'R1': (8, 4), 'X2': (12, 4), 'B2': (16, 4), 'D2': (20, 12), 'M3': (32, 4)},
    'RXF': {'R3': (8, 4), 'X2': (12, 4), 'B2': (16, 4), 'D2': (20, 12), 'R1': (32, 4)},
    'RXY-a': {'R1': (8, 4), 'X2': (12, 4), 'B2': (16, 4), 'DL2': (20, 12), 'DH2': (32, 8)},
    'RXY-b': {'M1': (8, 4), 'X2': (12, 4), 'B2': (16, 4), 'DL2': (20, 12), 'DH2': (32, 8)},
    'RXY-c': {'R1': (8, 4), 'X2': (12, 4), 'B2': (16, 4), 'DXL2': (20, 12), 'DXH2': (32, 8)},
    'S': {'B2': (16, 4), 'D2': (20, 12)},
    # LPSW, SSM and TS write the storage operand as D2(B2)
    'SI': {'I2': (8, 8), 'B1': (16, 4), 'D1': (20, 12), 'B2': (16, 4), 'D2': (20, 12)},
    'SIL': {'B1': (16, 4), 'D1': (20, 12), 'I2': (32, 16)},
    'SIY': {'I2': (8, 8), 'B1': (16, 4), 'DL1': (20, 12), 'DH1': (32, 8)},
    'SMI': {'M1': (8, 4), 'B3': (16, 4), 'D3': (20, 12), 'RI2': (32, 16)},
    'SS-a': {'L': (8, 8), 'L1': (8, 8), 'B1': (16, 4), 'D1': (20, 12), 'B2': (32, 4), 'D2': (36, 12)},
    'SS-b': {'L1': (8, 4), 'L2': (12, 4), 'B1': (16, 4), 'D1': (20, 12), 'B2': (32, 4), 'D2': (36, 12)},
    'SS-c': {'L1': (8, 4), 'I3': (12, 4), 'B1': (16, 4), 'D1': (20, 12), 'B2': (32, 4), 'D2': (36, 12)},
    'SS-d': {'R1': (8, 4), 'R3': (12, 4), 'B1': (16, 4), 'D1': (20, 12), 'B2': (32, 4), 'D2': (36, 12)},
    'SS-e': {'R1': (8, 4), 'R3': (12, 4), 'B2': (16, 4), 'D2': (20, 12), 'B4': (32, 4), 'D4': (36, 12)},
    'SS-f': {'L2': (8, 8), 'B1': (16, 4), 'D1': (20, 12), 'B2': (32, 4), 'D2': (36, 12)},
    'SSE': {'B1': (16, 4), 'D1': (20, 12), 'B2': (32, 4), 'D2': (36, 12)},
    'SSF': {'R3': (8, 4), 'B1': (16, 4), 'D1': (20, 12), 'B2': (32, 4), 'D2': (36, 12)},
    'VRI-a': {'V1': (8, 4), 'I2': (16, 16), 'M3': (32, 4)},
    'VRI-b': {'V1': (8, 4), 'I2': (16, 8), 'I3': (24, 8), 'M4': (32, 4)},
    'VRI-c': {'V1': (8, 4), 'V3': (12, 4), 'I2': (16, 16), 'M4': (32, 4)},
    'VRI-d': {'V1': (8, 4), 'V2': (12, 4), 'V3': (16, 4), 'I4': (24, 8), 'M5': (32, 4)},
    'VRI-e': {'V1': (8, 4), 'V2': (12, 4), 'I3': (16, 12), 'M5': (28, 4), 'M4': (32, 4)},
    'VRI-f': {'V1': (8, 4), 'V2': (12, 4), 'V3': (16, 4), 'M5': (24, 4), 'I4': (28, 8)},
    'VRI-g': {'V1': (8, 4), 'V2': (12, 4), 'I4': (16, 8), 'M5': (24, 4), 'I3': (28, 8)},
    'VRI-h': {'V1': (8, 4), 'I2': (16, 16), 'I3': (32, 4)},
    'VRI-i': {'V1': (8, 4), 'R2': (12, 4), 'M4': (24, 4), 'I3': (28, 8)},
    'VRI-j': {'V1': (8, 4), 'V2': (12, 4), 'M4': (24, 4), 'I3': (28, 8)},
    'VRI-k': {'V1': (8, 4), 'V2': (12, 4), 'V3': (16, 4), 'I5': (24, 8), 'V4': (32, 4)},
    'VRI-l': {'V1': (12, 4), 'V2': (16, 4), 'I3': (20, 16)},
    'VRR-a': {'V1': (8, 4), 'V2': (12, 4), 'M5': (24, 4), 'M4': (28, 4), 'M3': (32, 4)},
    'VRR-b': {'V1': (8, 4), 'V2': (12, 4), 'V3': (16, 4), 'M5': (24, 4), 'M4': (32, 4)},
    'VRR-c': {'V1': (8, 4), 'V2': (12, 4), 'V3': (16, 4), 'M6': (24, 4), 'M5': (28, 4), 'M4': (32, 4)},
    'VRR-d': {'V1': (8, 4), 'V2': (12, 4), 'V3': (16, 4), 'M5': (20, 4), 'M6': (24, 4), 'V4': (32, 4)},
    'VRR-e': {'V1': (8, 4), 'V2': (12, 4), 'V3': (16, 4), 'M6': (20, 4), 'M5': (28, 4), 'V4': (32, 4)},
    'VRR-f': {'V1': (8, 4), 'R2': (12, 4), 'R3': (16, 4)},
    'VRR-g': {'V1': (12, 4), 'I2': (20, 16)},
    'VRR-h': {'V1': (12, 4), 'V2': (16, 4), 'M3': (24, 4)},
    'VRR-i': {'R1': (8, 4), 'V2': (12, 4), 'M3': (24, 4), 'M4': (28, 4)},
    'VRR-j': {'V1': (8, 4), 'V2': (12, 4), 'V3': (16, 4), 'M4': (24, 4)},
    'VRR-k': {'V1': (8, 4), 'V2': (12, 4), 'M3': (24, 4)},
    'VRS-a': {'V1': (8, 4), 'V3': (12, 4), 'B2': (16, 4), 'D2': (20, 12), 'M4': (32, 4)},
    'VRS-b': {'V1': (8, 4), 'R3': (12, 4), 'B2': (16, 4), 'D2': (20, 12), 'M4': (32, 4)},
    'VRS-c': {'R1': (8, 4), 'V3': (12, 4), 'B2': (16, 4), 'D2': (20, 12), 'M4': (32, 4)},
    'VRS-d': {'R3': (12, 4), 'B2': (16, 4), 'D2': (20, 12), 'V1': (32, 4)},
    'VRV': {'V1': (8, 4), 'V2': (12, 4), 'B2': (16, 4), 'D2': (20, 12), 'M3': (32, 4)},
    'VRX': {'V1': (8, 4), 'X2': (12, 4), 'B2': (16, 4), 'D2': (20, 12), 'M3': (32, 4)},
    'VSI': {'I3': (8, 8), 'B2': (16, 4), 'D2': (20, 12), 'V1': (32, 4)},
}

# instruction length in bytes by format family
def fmt_length(fmt):
    fam = fmt.split('-')[0]
    if fam in ('E', 'I', 'RR'):
        return 2
    if fam in ('IE', 'RI', 'RRD', 'RRE', 'RRF', 'RS', 'RSI', 'RX', 'S', 'SI'):
        return 4
    return 6

# 4-digit operation codes that are the first two bytes of the instruction
OPC16 = {'E', 'RRE', 'RRF', 'RRD', 'S', 'SSE', 'SIL', 'IE'}

# --------------------------------------------------------------------------
# Facility codes of the "Characteristics" column (Appendix B legend)
# --------------------------------------------------------------------------
FACILITIES = {
    'BE': 'BEAR-enhancement facility', 'CF': 'Concurrent-functions facility',
    'CS': 'Compare-and-swap-and-store facility', 'CT': 'Configuration-topology facility',
    'CX': 'Constrained transactional-execution facility', 'D2': 'DAT-enhancement facility 2',
    'DE': 'DAT-enhancement facility 1', 'DO': 'Distinct-operands facility',
    'DP': 'Reset-DAP-protection facility', 'E2': 'Extended-translation facility 2',
    'E3': 'Extended-translation facility 3', 'ED1': 'Enhanced-DAT facility 1',
    'ED2': 'Enhanced-DAT facility 2', 'EH': 'Execution-hint facility',
    'EI': 'Extended-immediate facility', 'ES': 'Expanded-storage facility',
    'ET': 'Extract-CPU-time facility', 'F': 'Floating-point-extension facility',
    'FG': 'FPR-GR-transfer facility', 'FL': 'Store-facility-list-extended facility',
    'FS': 'Floating-point-support-sign-handling facility',
    'GE': 'General-instructions-extension facility', 'GF': 'Guarded-storage facility',
    'GZ': 'DEFLATE-conversion facility', 'HM': 'HFP-multiply-add/subtract facility',
    'HW': 'High-word facility', 'IA': 'Interlocked-access facility',
    'IM': 'Insert-reference-bits-multiple facility', 'L1': 'Load/store-on-condition facility 1',
    'L2': 'Load/store-on-condition facility 2', 'LD': 'Long-displacement facility',
    'LT': 'Load-and-trap facility', 'LZ': 'Load-and-zero-rightmost-byte facility',
    'M3': 'Message-security assist extension 3', 'M4': 'Message-security assist extension 4',
    'M5': 'Message-security assist extension 5', 'M8': 'Message-security assist extension 8',
    'M9': 'Message-security assist extension 9',
    'MI1': 'Miscellaneous-instruction-extensions facility 1',
    'MI2': 'Miscellaneous-instruction-extensions facility 2',
    'MI3': 'Miscellaneous-instruction-extensions facility 3',
    'MI4': 'Miscellaneous-instruction-extensions facility 4',
    'MO': 'Move-with-optional-specifications facility', 'MS': 'Message-security assist',
    'NN': 'Neural-network-processing-assist facility', 'PA': 'Processor-assist facility',
    'PC': 'DFP packed-conversion facility', 'PE': 'Parsing-enhancement facility',
    'PF': 'PFPO facility', 'PI': 'Processor-activity-instrumentation facility',
    'PK': 'Population-count facility', 'RA': 'Reusable-ASN-and-LX facility',
    'RB': 'Reset-reference-bits-multiple facility', 'SC': 'Store-clock-fast facility',
    'SL': 'Enhanced-sort facility', 'TE': 'Test-pending-external-interruption facility',
    'TF': 'Decimal-floating-point facility', 'TR': 'Decimal-floating-point-rounding facility',
    'TS': 'TOD-clock-steering facility', 'TX': 'Transactional-execution facility',
    'UE': 'HFP unnormalized-extensions facility', 'V1': 'Vector-enhancements facility 1',
    'V2': 'Vector-enhancements facility 2', 'V3': 'Vector-enhancements facility 3',
    'VD': 'Vector packed-decimal facility',
    'VD2': 'Vector-packed-decimal-enhancement facility 2',
    'VD3': 'Vector-packed-decimal-enhancement facility 3',
    'VF': 'Vector facility for z/Architecture', 'XF': 'IEEE-exception-simulation facility',
    'XX': 'Execute-extension facility', 'ZF': 'Decimal-floating-point-zoned-conversion facility',
}

# --------------------------------------------------------------------------
# Register classes (not stated by the POP syntax lines)
# --------------------------------------------------------------------------
# floating-point instructions outside chapters 18-20
FP_EXTRA = set('LD LDR LDY LE LER LEY LXR STD STDY STE STEY CPSDR LCDFR LNDFR LPDFR LDGR LGDR '
               'LZDR LZER LZXR THDER THDR TBDR TBEDR'.split())
# overrides: mnemonic -> {field: class}; classes G F A C
REG_OVERRIDE = {
    'LDGR': {'R2': 'G'}, 'LGDR': {'R1': 'G'},
    'IEDTR': {'R2': 'G'}, 'IEXTR': {'R2': 'G'}, 'RRDTR': {'R2': 'G'}, 'RRXTR': {'R2': 'G'},
    'CPYA': {'R1': 'A', 'R2': 'A'}, 'EAR': {'R2': 'A'}, 'SAR': {'R1': 'A'}, 'TAR': {'R1': 'A'},
    'LAM': {'R1': 'A', 'R3': 'A'}, 'LAMY': {'R1': 'A', 'R3': 'A'},
    'STAM': {'R1': 'A', 'R3': 'A'}, 'STAMY': {'R1': 'A', 'R3': 'A'},
    'LCTL': {'R1': 'C', 'R3': 'C'}, 'LCTLG': {'R1': 'C', 'R3': 'C'},
    'STCTL': {'R1': 'C', 'R3': 'C'}, 'STCTG': {'R1': 'C', 'R3': 'C'},
}
# (name prefix, {field: class}) applied to floating-point instructions
REG_NAME_RULES = [
    (('CONVERT FROM FIXED', 'CONVERT FROM LOGICAL', 'CONVERT FROM SIGNED PACKED',
      'CONVERT FROM UNSIGNED PACKED'), {'R2': 'G'}),
    (('CONVERT TO FIXED', 'CONVERT TO LOGICAL', 'CONVERT TO SIGNED PACKED',
      'CONVERT TO UNSIGNED PACKED'), {'R1': 'G'}),
    (('EXTRACT BIASED EXPONENT', 'EXTRACT SIGNIFICANCE'), {'R1': 'G'}),
]


def reg_class(mn, name, chapter, field):
    if mn in REG_OVERRIDE and field in REG_OVERRIDE[mn]:
        return REG_OVERRIDE[mn][field]
    fp = chapter in ('18', '19', '20') or mn in FP_EXTRA
    if not fp:
        return 'G'
    up = name.upper()
    for prefixes, ov in REG_NAME_RULES:
        if up.startswith(prefixes) and field in ov:
            return ov[field]
    return 'F'


# --------------------------------------------------------------------------
# text extraction
# --------------------------------------------------------------------------
def extract(pdf):
    tmp = tempfile.mkdtemp(prefix='pop2json')
    out = {}
    try:
        for key, args in (('full', ['-layout']),
                          ('left', ['-x', '0', '-y', '0', '-W', '306', '-H', '792', '-layout']),
                          ('right', ['-x', '306', '-y', '0', '-W', '306', '-H', '792', '-layout'])):
            f = os.path.join(tmp, key + '.txt')
            subprocess.check_call(['pdftotext'] + args + [pdf, f])
            out[key] = open(f, encoding='utf-8', errors='replace').read().split('\f')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return out


# --------------------------------------------------------------------------
# Appendix B
# --------------------------------------------------------------------------
FMT_NAMES = sorted(FORMATS, key=len, reverse=True)
fmt_alt = '|'.join(re.escape(f) for f in FMT_NAMES)
rowrx = re.compile(r'^\s*([A-Z][A-Z0-9]*|—)\s+(\S.*?)\s+(' + fmt_alt + r')(?=\s|$)(.*)$')
tailrx = re.compile(r'^(.*?)(?:\s|^)([0-9A-F]{2,4})\s+(\d+-\d+)\s*$')


def parse_appendix_b(P):
    pages = [p for p in P if re.search(r'Figure B-2\. Instructions Arranged by Mnemonic \(Part \d+ of 25\)', p)]
    rows = []
    for p in pages:
        lines = p.split('\n')
        i = 0
        while i < len(lines):
            l = lines[i]
            i += 1
            m = rowrx.match(l)
            if not m:
                continue
            mn, name, fmt, rest = m.groups()
            if mn == '—' or not re.fullmatch(r'[A-Z][A-Z0-9]*', mn):
                continue
            t = tailrx.match(rest)
            j = i
            while not t and j < len(lines) and j < i + 3:
                nxt = lines[j]
                j += 1
                if rowrx.match(nxt):
                    break
                t = tailrx.match(nxt)
            if not t:
                sys.stderr.write('pop2json: no opcode for %s\n' % mn)
                continue
            chars, op, page = t.groups()
            rows.append(dict(mnemonic=mn, name=name.strip(), format=fmt,
                             chars=chars.split(), opcode=op, page=page))
    return rows


# --------------------------------------------------------------------------
# syntax lines of the instruction descriptions
# --------------------------------------------------------------------------
KNOWN = set(FORMATS)


def parse_syntax(L, R):
    synrx = re.compile(r'(?<![A-Za-z0-9])([A-Z][A-Za-z0-9]*)\s+([A-Z0-9][A-Za-z0-9,()\[\]]*)\s+\[([A-Z]{1,4}(?:-[a-z])?)\]')
    synrx0 = re.compile(r'(?<![A-Za-z0-9])([A-Z][A-Za-z0-9]*)\s+\[([A-Z]{1,4}(?:-[a-z])?)\]')
    hdr = re.compile(r'(?<![A-Za-z0-9])(Mnemonic\d*)\s+Op\s*Code')
    row = re.compile(r'(?<![A-Za-z0-9])([A-Z][A-Z0-9]*)\s+[\'‘’]([0-9A-F]{2,6})[\'’]')
    syn = collections.defaultdict(list)
    groups = {}
    tkey = None
    for i, (l, r) in enumerate(zip(L, R)):
        for col in (l, r):
            for ln in col.split('\n'):
                m = synrx.search(ln)
                if not m:
                    m0 = synrx0.search(ln)
                    if m0 and 'Op Code' not in ln and m0.group(2) in KNOWN:
                        syn[m0.group(1)].append(('', m0.group(2)))
                        continue
                if m and 'Op Code' not in ln:
                    name, ops, fmt = m.groups()
                    if name.startswith('Mnemonic'):
                        if name in ('Mnemonic', 'Mnemonic1'):
                            groups.clear()          # a new cluster of syntax forms
                        groups[name] = {'ops': ops, 'fmt': fmt, 'm': [], 'tab': False}
                        tkey = None
                    else:
                        syn[name].append((ops, fmt))
                    continue
                m = hdr.search(ln)
                if m:
                    tkey = m.group(1)
                    if tkey not in groups or groups[tkey]['tab']:
                        # the table header is mis-numbered in places (MULTIPLY DFP):
                        # use the latest syntax form that has no table yet
                        free = [k for k, g in groups.items() if not g['tab']]
                        tkey = free[-1] if free else None
                    if tkey:
                        groups[tkey]['tab'] = True
                    continue
                for m in row.finditer(ln):
                    if tkey and tkey in groups:
                        g = groups[tkey]
                        g['m'].append(m.group(1))
                        syn[m.group(1)].append((g['ops'], g['fmt']))
    return syn


def syntax_slots(s):
    """'R1,D2(X2,B2)[,M3]' -> (['R1','D2(X2,B2)','M3'], number of optional)"""
    toks = re.findall(r'\[|\]|[A-Za-z0-9]+(?:\([^)]*\))?', s)
    slots, nopt, inopt = [], 0, False
    for t in toks:
        if t == '[':
            inopt = True
        elif t == ']':
            pass
        else:
            parts = [t]
            if re.fullmatch(r'(?:[VRM]\d)+', t) and len(t) > 2:   # V1V2 lost its comma
                parts = re.findall(r'[VRM]\d', t)
            for p in parts:
                slots.append(p)
                if inopt:
                    nopt += 1
    return slots, nopt


# --------------------------------------------------------------------------
# Appendix J
# --------------------------------------------------------------------------
def parse_appendix_j(P, basemn):
    pages = [p for p in P if re.search(
        r'Figure J-1\. List of Extended Mnemonics Arranged by Instruction Name \(Part \d+ of 24\)', p)]
    oprx = re.compile(r'^[A-Za-z0-9()\[\],]+$')
    out, bad = [], []
    for p in pages:
        for ln in p.split('\n'):
            if 'Instruction Name' in ln or 'Figure J' in ln or 'Copyright' in ln:
                continue
            t = ln.split()
            if len(t) < 4:
                continue
            for k in range(len(t) - 3):
                if (t[k + 2] in basemn and re.fullmatch(r'[A-Z][A-Z0-9]*', t[k])
                        and oprx.match(t[k + 1]) and re.search(r'\d', t[k + 1])
                        and re.search(r'[\d\[]', t[k + 3])):
                    out.append((t[k], t[k + 1], t[k + 2], t[k + 3], ' '.join(t[k + 4:])))
                    break
            else:
                if re.search(r'^\s{30,}[A-Z][A-Z0-9]+\s{2,}\S', ln):
                    bad.append(ln.strip())
    return out, bad


# --------------------------------------------------------------------------
# table builder
# --------------------------------------------------------------------------
class Table:
    def __init__(self):
        self.ops = [{'name': 'UNUSED', 'bits': 0, 'shift': 0, 'flags': []}]
        self.index = {'UNUSED': 0}

    def operand(self, name, bits, shift, flags):
        if name not in self.index:
            self.index[name] = len(self.ops)
            self.ops.append({'name': name, 'bits': bits, 'shift': shift, 'flags': flags})
        else:
            o = self.ops[self.index[name]]
            assert (o['bits'], o['shift'], o['flags']) == (bits, shift, flags), name
        return name


def field_operand(tab, fmt, fname, mn, iname, chapter, role=None):
    """descriptor name for field fname of fmt; role: None, 'index', 'base', 'len', 'mid'"""
    shift, bits = FORMATS[fmt][fname]
    c = fname[0]
    if fname.startswith('RI'):
        return tab.operand('J%d_%d' % (bits, shift), bits, shift, ['PCREL'])
    if c == 'R':
        k = reg_class(mn, iname, chapter, fname)
        pre = {'G': 'R', 'F': 'F', 'A': 'A', 'C': 'C'}[k]
        fl = {'G': 'GPR', 'F': 'FPR', 'A': 'AR', 'C': 'CR'}[k]
        return tab.operand('%s_%d' % (pre, shift), bits, shift, [fl])
    if c == 'V':
        if role == 'index':
            return tab.operand('VX_%d' % shift, bits, shift, ['INDEX', 'VR'])
        return tab.operand('V_%d' % shift, bits, shift, ['VR'])
    if c == 'X':
        return tab.operand('X_%d' % shift, bits, shift, ['INDEX', 'GPR'])
    if c == 'B':
        return tab.operand('B_%d' % shift, bits, shift, ['BASE', 'GPR'])
    if c == 'L':
        return tab.operand('L%d_%d' % (bits, shift), bits, shift, ['LENGTH'])
    if c == 'M':
        return tab.operand('U%d_%d' % (bits, shift), bits, shift, [])
    if c == 'I':
        return tab.operand('I%d_%d' % (bits, shift), bits, shift, ['SIGNED'])
    raise KeyError(fname)


def disp_operand(tab, fmt, dname):
    f = FORMATS[fmt]
    if dname in f:
        shift, bits = f[dname]
        return tab.operand('D_%d' % shift, bits, shift, ['DISP'])
    lo, hi = dname[0] + dname[1:-1] + 'L' + dname[-1], dname[0] + dname[1:-1] + 'H' + dname[-1]
    # D2 -> DL2/DH2 ; DX2 -> DXL2/DXH2
    if dname.startswith('DX'):
        lo, hi = 'DXL' + dname[-1], 'DXH' + dname[-1]
    else:
        lo, hi = 'DL' + dname[-1], 'DH' + dname[-1]
    shift = f[lo][0]
    assert f[hi][0] == shift + 12 and f[lo][1] == 12 and f[hi][1] == 8
    return tab.operand('D20_%d' % shift, 20, shift, ['DISP', 'SIGNED'])


def expand_slot(tab, fmt, tok, mn, iname, chapter):
    """one syntax token -> list of operand descriptor names (D, X/L/R, B order)"""
    m = re.fullmatch(r'(D[X]?\d)\((\w+(?:,\w+)?)\)', tok)
    if m:
        dname, inner = m.groups()
        parts = inner.split(',')
        out = [disp_operand(tab, fmt, dname)]
        if len(parts) == 2:
            mid = parts[0]
            role = 'index' if mid.startswith(('X', 'V')) else ('len' if mid.startswith('L') else 'mid')
            out.append(field_operand(tab, fmt, mid, mn, iname, chapter, role))
        out.append(field_operand(tab, fmt, parts[-1], mn, iname, chapter, 'base'))
        return out
    if re.fullmatch(r'D[X]?\d', tok):
        raise KeyError('bare displacement ' + tok)
    return [field_operand(tab, fmt, tok, mn, iname, chapter)]


def slot_fields(fmt, tok):
    """(field name, bit position, width) list covered by a syntax token"""
    f = FORMATS[fmt]
    m = re.fullmatch(r'(D[X]?\d)\((\w+(?:,\w+)?)\)', tok)
    names = []
    if m:
        d = m.group(1)
        if d in f:
            names.append(d)
        else:
            names += (['DXL' + d[-1], 'DXH' + d[-1]] if d.startswith('DX') else ['DL' + d[-1], 'DH' + d[-1]])
        names += m.group(2).split(',')
    else:
        names.append(tok)
    return [(n, f[n][0], f[n][1]) for n in names]


def set_bits(buf, shift, bits, value):
    for i in range(bits):
        bit = (value >> (bits - 1 - i)) & 1
        pos = shift + i
        if bit:
            buf[pos // 8] |= 0x80 >> (pos % 8)
        else:
            buf[pos // 8] &= ~(0x80 >> (pos % 8)) & 0xFF


def base_opcode(row):
    fmt, op = row['format'], row['opcode']
    n = fmt_length(fmt)
    b = bytearray(n)
    fam = fmt.split('-')[0]
    if len(op) == 2:
        b[0] = int(op, 16)
    elif len(op) == 3:
        b[0] = int(op[:2], 16)
        b[1] = int(op[2], 16)           # OpCd nibble, bits 12-15
    else:
        if fam in OPC16:
            b[0], b[1] = int(op[:2], 16), int(op[2:], 16)
        else:
            b[0], b[n - 1] = int(op[:2], 16), int(op[2:], 16)
    return b


def clean(s):
    s = s.replace('\uf066', '->')
    return re.sub(r'[^\x20-\x7e]', '?', s).lower()


# Corrections found by comparing the generated table with the real HLASM
# (tools/oracle.sh, 2026-11-16); the book's text layout loses them.
#  * Appendix B shows VSTEBRG and VSTEBRH with the operation code of VSTEBRF
#    (E60B); HLASM encodes VSTEBRH as E609 and VSTEBRG as E60A.
OPCODE_FIX = {'VSTEBRH': 'E609', 'VSTEBRG': 'E60A'}
#  * the mnemonics with a trailing Z (zero remaining bits, bit 0 of I4) or T
#    (test results only, bit 0 of I3) are the base instruction with that bit
#    set: mnemonic -> (byte index in the instruction, bit mask)
ORV_FIX = {'RISBGZ': (3, 0x80), 'RISBGNZ': (3, 0x80), 'RISBHGZ': (3, 0x80),
           'RISBLGZ': (3, 0x80), 'RNSBGT': (2, 0x80), 'ROSBGT': (2, 0x80),
           'RXSBGT': (2, 0x80)}


def build(P, L, R, warn):
    rows = parse_appendix_b(P)
    for r in rows:
        if r['mnemonic'] in OPCODE_FIX:
            r['opcode'] = OPCODE_FIX[r['mnemonic']]
    syn = parse_syntax(L, R)
    manual = {'KDSA': ('R1,R2', 'RRE')}       # no syntax line is picked up for KDSA
    tab = Table()
    insts = []
    byname = {}
    for r in rows:
        mn = r['mnemonic']
        c = [s for s in syn.get(mn, []) if s[1] == r['format']]
        if c:
            ops = collections.Counter(s[0] for s in c).most_common(1)[0][0]
        elif mn in manual and manual[mn][1] == r['format']:
            ops = manual[mn][0]
        else:
            warn('no syntax for %s' % mn)
            continue
        slots, nopt = syntax_slots(ops)
        chapter = r['page'].split('-')[0]
        try:
            names = []
            for tok in slots:
                names += expand_slot(tab, r['format'], tok, mn, r['name'], chapter)
        except (KeyError, AssertionError) as e:
            warn('%s: cannot map operands %r (%s)' % (mn, ops, e))
            continue
        if len(names) > 6:
            warn('%s: more than 6 operand fields' % mn)
            continue
        fac = [t for t in r['chars'] if t in FACILITIES]
        e = {'mnemonic': mn.lower(), 'format': r['format'],
             'length': fmt_length(r['format']),
             'opcode': base_opcode(r).hex().upper(),
             'operands': names, 'opt': nopt,
             'facility': ','.join(fac), 'desc': clean(r['name'])}
        insts.append(e)
        byname[mn] = (r, slots, nopt, e)
    return rows, syn, tab, insts, byname


def same_slot(a, b):
    """POP writes D2(X2,B2) where the instruction has D2(B2) (VLVGB..VLVGG)"""
    ma = re.fullmatch(r'(D\w*\d)\((?:\w+,)?(B\d)\)', a)
    mb = re.fullmatch(r'(D\w*\d)\((?:\w+,)?(B\d)\)', b)
    return bool(ma and mb and ma.groups() == mb.groups())


def add_extended(tab, insts, byname, J, warn):
    seen = set()
    have = {e['mnemonic'] for e in insts}
    for ext, extops, base, baseops, comment in J:
        if base not in byname:
            continue
        r, bslots, bopt, be = byname[base]
        key = (ext, base, baseops)
        if key in seen:
            continue
        seen.add(key)
        if ext.lower() in have:
            warn('extended mnemonic %s duplicates an instruction' % ext)
            continue
        fmt = r['format']
        if ext in ('VSTRSB', 'VSTRSH', 'VSTRSF'):     # POP says [M5], the optional operand is M6
            extops = extops.replace('[,M5]', '[,M6]')
            baseops = baseops.replace(',M5]', ',M6]').replace('[,M5]', '[,M6]')
        # POP sometimes writes "D2,(B2)" for "D2(B2)"
        extops = re.sub(r'(D[X]?\d),\(', r'\1(', extops).upper()
        baseops = re.sub(r'(D[X]?\d),\(', r'\1(', baseops).replace('[,', ',')
        bvals = re.findall(r"(?:[^,()]|\([^)]*\))+", baseops)
        eslots, eopt = syntax_slots(extops)
        buf = base_opcode(r)
        orv = bytearray(len(buf))
        used = {}
        ok = True
        for i, bv in enumerate(bvals):
            bv2 = re.sub(r'[\[\]]', '', bv.strip())
            m = re.fullmatch(r'\((.*\|.*)\)', bv2)
            if m:
                bv2 = m.group(1)
            if i >= len(bslots):
                ok = False
                break
            slot = bslots[i]
            fields = slot_fields(fmt, slot)
            mc = re.fullmatch(r"-?\d+|X[’']([0-9A-Fa-f]+)[’']", bv2)
            mo = re.fullmatch(r"(\w+)\|(\d+|X[’']([0-9A-Fa-f]+)[’'])", bv2)
            if mc:
                if len(fields) != 1:
                    ok = False
                    break
                _, sh, bits = fields[0]
                v = int(mc.group(1), 16) if mc.group(1) else int(bv2)
                if v < 0 or v >= (1 << bits):
                    ok = False
                    break
                set_bits(buf, sh, bits, v)
            elif bv2 == slot or same_slot(bv2, slot):
                used[slot] = i
            elif mo and mo.group(1) == slot and len(fields) == 1:
                _, sh, bits = fields[0]
                v = int(mo.group(3), 16) if mo.group(3) else int(mo.group(2))
                set_bits(orv, sh, bits, v)
                used[slot] = i
            else:
                ok = False
                break
        if not ok:
            warn('extended mnemonic %s %s -> %s %s not generated' % (ext, extops, base, baseops))
            continue
        # operands in extended syntax order
        names = []
        chapter = r['page'].split('-')[0]
        try:
            for tok in eslots:
                if tok not in used:
                    # POP writes VLVGB ... D2(X2,B2) for VLVG ... D2(B2): match on D and B
                    alt = [b for b in used if same_slot(b, tok)]
                    if len(alt) != 1:
                        raise KeyError('operand %s not in base syntax' % tok)
                    tok = alt[0]
                names += expand_slot(tab, fmt, tok, base, r['name'], chapter)
        except KeyError as e:
            warn('extended mnemonic %s: %s' % (ext, e))
            continue
        e = {'mnemonic': ext.lower(), 'format': fmt, 'length': be['length'],
             'opcode': buf.hex().upper(), 'operands': names, 'opt': eopt,
             'facility': be['facility'], 'desc': clean(comment) if comment else be['desc']}
        if ext.upper() in ORV_FIX:        # "control (Z|T) treated as being one": prose in POP
            orv[ORV_FIX[ext.upper()][0]] |= ORV_FIX[ext.upper()][1]
        if any(orv):
            e['or'] = orv.hex().upper()
        insts.append(e)
        have.add(e['mnemonic'])


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    X = extract(sys.argv[1])
    warns = []
    warn = warns.append
    rows, syn, tab, insts, byname = build(X['full'], X['left'], X['right'], warn)
    J, bad = parse_appendix_j(X['full'], set(byname))
    add_extended(tab, insts, byname, J, warn)
    for b in bad:
        warn('Appendix J line not understood: ' + b)
    insts.sort(key=lambda e: (e['mnemonic'], e['opcode']))
    # operand rules found with the real HLASM (tools/oracle_rules.py)
    rf = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'isa', 'oracle_rules.json')
    if os.path.exists(rf):
        orules = json.load(open(rf))
        for e in insts:
            r = orules.get('%s %s' % (e['mnemonic'], e['format']))
            if r:
                e['rules'] = r
    doc = {
        'version': 2,
        'source': 'IBM z/Architecture Principles of Operation, SA22-7832-14 (Appendix B, '
                  'Figure 5-1, instruction format lines, Appendix J); generated by '
                  'tools/pop2json.py',
        'facilities': FACILITIES,
        'operands': tab.ops,
        'instructions': insts,
    }
    out = sys.stdout
    out.write('{\n')
    out.write('  "version": 2,\n')
    out.write('  "source": %s,\n' % json.dumps(doc['source']))
    out.write('  "facilities": {\n' + ',\n'.join('    %s: %s' % (json.dumps(k), json.dumps(v))
                                                  for k, v in sorted(FACILITIES.items())) + '\n  },\n')
    out.write('  "operands": [\n' + ',\n'.join('    ' + json.dumps(o) for o in tab.ops) + '\n  ],\n')
    out.write('  "instructions": [\n' + ',\n'.join('    ' + json.dumps(e) for e in insts) + '\n  ]\n}\n')
    for w in warns:
        sys.stderr.write('pop2json: warning: %s\n' % w)
    sys.stderr.write('pop2json: %d instructions (%d base, %d extended), %d operand kinds, %d warnings\n' % (
        len(insts), len(rows), len(insts) - len(rows), len(tab.ops), len(warns)))
    return 1 if warns else 0


if __name__ == '__main__':
    sys.exit(main())
