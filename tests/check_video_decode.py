"""Read AVI packets and exercise the production VP3-to-BGR player override."""
import pathlib
import struct
import subprocess
import sys
ROOT=pathlib.Path(__file__).resolve().parents[1]

def chunks(raw,start,end):
    while start+8<=end:
        kind,size=struct.unpack_from('<4sI',raw,start);payload=start+8
        if payload+size>end:raise ValueError('Truncated AVI chunk')
        yield kind,payload,size
        start=payload+size+(size&1)

def fixture(path):
    raw=path.read_bytes();assert raw[:4]==b'RIFF' and raw[8:12]==b'AVI '
    dimensions=None;packets=[]
    for kind,offset,size in chunks(raw,12,min(len(raw),struct.unpack_from('<I',raw,4)[0]+8)):
        if kind!=b'LIST':continue
        if raw[offset:offset+4]==b'hdrl':
            for sub,at,n in chunks(raw,offset+4,offset+size):
                if sub!=b'LIST' or raw[at:at+4]!=b'strl':continue
                items=list(chunks(raw,at+4,at+n))
                if not any(k==b'strh' and raw[p:p+4]==b'vids' for k,p,_ in items):continue
                p=next(p for k,p,_ in items if k==b'strf')
                w,h=struct.unpack_from('<ii',raw,p+4);tag=struct.unpack_from('<I',raw,p+16)[0]
                dimensions=(abs(w),abs(h),tag)
        if raw[offset:offset+4]==b'movi':
            for sub,at,n in chunks(raw,offset+4,offset+size):
                if sub[-2:] in (b'dc',b'db') and n:
                    packets.append(raw[at:at+n])
                    if len(packets)==120:break
    assert dimensions and packets,'No video stream'
    return struct.pack('<4I',*dimensions,len(packets))+b''.join(struct.pack('<I',len(p))+p for p in packets)

if __name__=='__main__':
    out=ROOT/'work/macos';source=pathlib.Path(sys.argv[1])
    data=out/'video-check.fixture';data.write_bytes(fixture(source))
    binary=out/'video-check'
    subprocess.run(['clang','-O2','-I'+str(ROOT/'work/macos/include'),
        str(ROOT/'tests/video_decode_check.c'),str(ROOT/'runtime/native/fo2_video.c'),
        '-o',str(binary)],check=True)
    subprocess.run([str(binary),str(data)],check=True,timeout=30)
