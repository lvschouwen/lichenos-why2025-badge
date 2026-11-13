import sys,re
p=sys.argv[1]; s=open(p).read(); n=0
def merge(m):
    global n; n+=1
    ours=m.group(1); theirs=m.group(2)
    ol=ours.split('\n'); tl=theirs.split('\n')
    if len(ol)!=len(tl): raise SystemExit("hunk %d: line counts differ (%d vs %d); resolve by hand"%(n,len(ol),len(tl)))
    out=[]
    for a,b in zip(ol,tl):
        if a==b: out.append(a); continue
        at=a.split(' '); bt=b.split(' ')
        res=list(at); have=set(at)
        for i,t in enumerate(bt):
            if t and t not in have:
                prev=bt[i-1] if i>0 else None
                j=res.index(prev)+1 if prev in res else len(res)
                res.insert(j,t); have.add(t)
        out.append(' '.join(res))
    return '\n'.join(out)
s2=re.sub(r'<<<<<<< [^\n]*\n(.*?)\n=======\n(.*?)\n>>>>>>> [^\n]*',merge,s,flags=re.S)
assert '<<<<<<<' not in s2 and '>>>>>>>' not in s2
open(p,'w').write(s2); print("merged %d hunk(s) in %s"%(n,p.split('/')[-1]))
