import os,sys,re
t=open(os.environ.get('RE_DIR','.')+'/resolved.c').read()
parts=re.split(r'(?=//==== FUNC )',t)
want=sys.argv[1:]
for p in parts:
    m=re.match(r'//==== FUNC (\w+)',p)
    if m and m.group(1) in want: print(p)
