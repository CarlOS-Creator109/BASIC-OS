import numpy as np, struct, sys
COLS,ROWS=120,45; W,H=960,720; FW,FH=W,H
font=np.frombuffer(open('font.bin','rb').read(),dtype=np.uint8).reshape(256,16)
codes=[c for c in list(range(0x20,0x7F))+list(range(0xB0,0xE0))]
bits=np.unpackbits(font[codes],axis=1).astype(np.float32)   # (n,128) row-major 16 rows x 8
G=bits; g2=(G*G).sum(1)
MARGIN=float(sys.argv[1]) if len(sys.argv)>1 else 2.0
THR=float(sys.argv[2]) if len(sys.argv)>2 else 6.0
raw=np.memmap('gray.raw',dtype=np.uint8,mode='r').reshape(-1,H,W)
nf=raw.shape[0]
prev_src=None; prev_g=np.full(COLS*ROWS,0,dtype=np.int32)  # glyph index of ' ' = 0
out=bytearray(); nch=[]
def varint(v):
    b=bytearray()
    while v>=128: b.append((v&127)|128); v>>=7
    b.append(v); return b
first=True
for f in range(nf):
    fr=raw[f].astype(np.float32)
    d=np.clip((255-fr-70)/100.0,0,1)
    P=d.reshape(ROWS,16,COLS,8).transpose(0,2,1,3).reshape(ROWS*COLS,128)
    if prev_src is None:
        chg=np.ones(len(P),bool)
    else:
        chg=np.abs(P-prev_src).sum(1)>THR
    idx=np.nonzero(chg)[0]
    newg=prev_g.copy()
    if len(idx):
        Pi=P[idx]
        err=(Pi*Pi).sum(1)[:,None]-2*Pi@G.T+g2[None,:]
        best=err.argmin(1); be=err[np.arange(len(idx)),best]
        pe=err[np.arange(len(idx)),prev_g[idx]]
        keep=pe<=be+MARGIN
        newg[idx]=np.where(keep,prev_g[idx],best)
        prev_src=P.copy() if prev_src is None else prev_src
        prev_src[idx]=Pi
    ch=np.nonzero(newg!=prev_g)[0]
    out+=varint(len(ch)); last=-1
    for i in ch:
        out+=varint(int(i-last-1)); out.append(codes[newg[i]]); last=i
    nch.append(len(ch)); prev_g=newg
open('video.bin','wb').write(out)
print("frames",nf,"bytes",len(out),"avg changes",np.mean(nch),"max",max(nch))
