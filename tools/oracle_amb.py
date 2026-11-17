#!/usr/bin/env python3
"""oracle_amb.py FILE - decode an amblist LISTOBJ output (raw EBCDIC cp037)
into one logical line per ESD/TEXT/RLD/... item, for diffing two objects."""
import re, sys
d = open(sys.argv[1], 'rb').read().decode('cp037', 'replace')
t = re.sub(r'\s+', ' ', d)
t = re.sub(r'PAGE\s+\d+|1\s+\*+ G E N.*?\*+', '', t)
t = re.sub(r'0RECORD TYPE', '\nRECORD TYPE', t)
t = re.sub(r'>(\d{6}) ', r'\n>\1 ', t)
print(t)
