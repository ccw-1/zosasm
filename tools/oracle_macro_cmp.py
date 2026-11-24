#!/usr/bin/env python3
"""Compare the statements HLASM generated for a macro call (listing NAME.lst from tools/oracle.sh style
runs on igor: the lines marked N+) with the -E output of zosasm (NAME.zexp) on name, operation and first
operand. usage: oracle_macro_cmp.py NAME   (files NAME.lst and NAME.zexp in the current directory)"""
import sys,re,difflib
m=sys.argv[1]
def norm(x):
    x=re.sub(r'\s*X?\s*\d\d-\d{8}$','',x)
    return ' '.join(x.split()[:3])
h=[]
for l in open(m+'.lst',encoding='latin1'):
    mm=re.match(r'^[ 0-9A-F]{0,60}?\s(\d+)\+(.*)$',l.rstrip('\n'))
    if mm and mm.group(2).strip() and not mm.group(2).strip().startswith('*'): h.append(norm(mm.group(2)))
z=[]
for l in open(m+'.zexp',encoding='latin1'):
    l=l.rstrip('\n')
    if not l.strip() or l.startswith('*'): continue
    n=norm(l)
    if n in('T CSECT','END'): continue
    z.append(n)
print(m,'hlasm',len(h),'zosasm',len(z))
d=list(difflib.unified_diff(h,z,lineterm='',n=1))
print(len(d),'diff lines'); print('\n'.join(d[:40]))
sys.exit(1 if d or len(h)!=len(z) else 0)
