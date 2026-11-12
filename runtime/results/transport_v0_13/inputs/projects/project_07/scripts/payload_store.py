"""Bounded writer for Suffice 0.10 independently compressed payload rows.

The checksum binds accidental file mixups, not adversarial authenticity. Input
features are signed little-endian int32, and manifest order is global row order.
Zstandard is required only by this application writer, not the Suffice core.
"""
from pathlib import Path
import argparse, hashlib, json, os, struct
import zstandard

MASK=(1<<64)-1
def fnv64(data, state=14695981039346656037):
    for byte in data: state=((state^byte)*1099511628211)&MASK
    return state

def write_store(destination, raw_path, channels, payloads, reference_path=None,
                max_row_bytes=1<<20, max_total_decoded_bytes=4<<30,
                max_store_bytes=512<<20):
    destination=Path(destination);raw_path=Path(raw_path)
    if type(channels) is not int or not 1<=channels<=1024: raise ValueError("channels must be in [1,1024]")
    for value in (max_row_bytes,max_total_decoded_bytes,max_store_bytes):
        if type(value) is not int or value<0: raise ValueError("budgets must be nonnegative integers")
    nbytes=raw_path.stat().st_size
    if nbytes>max_store_bytes or nbytes%(4*channels): raise ValueError("invalid/budgeted feature length")
    with raw_path.open("rb") as stream: raw=stream.read(nbytes+1)
    if len(raw)!=nbytes: raise ValueError("feature file changed during read")
    rows=len(raw)//(4*channels)
    if rows>1<<20: raise ValueError("row limit exceeded")
    index_bytes=rows*24;start=64+index_bytes
    if start>max_store_bytes: raise ValueError("index exceeds store budget")
    if reference_path is not None and Path(reference_path).resolve()==destination.resolve():
        raise ValueError("reference and compressed paths must differ")
    compressor=zstandard.ZstdCompressor(level=3,write_checksum=True,write_content_size=True)
    index=bytearray(index_bytes);cursor=start;total=0;created=[];reference=None
    try:
        stream=open(destination,"xb+");created.append(destination)
        with stream:
            stream.write(bytes(start))
            if reference_path is not None:
                reference=open(reference_path,"xb");created.append(Path(reference_path))
                reference.write(b"SUFREF01"+struct.pack("<Q",rows))
            iterator=iter(payloads)
            try:
                for row in range(rows):
                    try: payload=next(iterator)
                    except StopIteration as exc: raise ValueError("too few payload rows") from exc
                    if not isinstance(payload,(bytes,bytearray)): raise ValueError("payload must be bytes")
                    payload=bytes(payload)
                    if len(payload)>max_row_bytes: raise ValueError("row exceeds decoded limit")
                    total+=len(payload)
                    if total>max_total_decoded_bytes: raise ValueError("decoded total exceeds limit")
                    if reference is not None: reference.write(struct.pack("<I",len(payload))+payload)
                    frame=compressor.compress(payload)
                    if len(frame)>0xffffffff or cursor+len(frame)>max_store_bytes:raise ValueError("compressed store exceeds limit")
                    struct.pack_into("<QIIQ",index,row*24,cursor,len(frame),len(payload),fnv64(payload))
                    stream.write(frame);cursor+=len(frame)
                try: next(iterator)
                except StopIteration: pass
                else: raise ValueError("too many payload rows")
            finally:
                if reference is not None: reference.close()
            header=bytearray(struct.pack("<8sIIQIIQQQQ",b"SUFPAY01",1,1,rows,channels,24,fnv64(raw),index_bytes,cursor,0))
            struct.pack_into("<Q",header,56,fnv64(index,fnv64(header)))
            stream.seek(0);stream.write(header);stream.write(index)
            stream.flush()
    except BaseException:
        if reference is not None and not reference.closed:reference.close()
        for p in reversed(created):p.unlink(missing_ok=True)
        raise
    return {"rows":rows,"channels":channels,"feature_bytes":len(raw),
            "store_bytes":cursor,"decoded_payload_bytes":total,"index_bytes":index_bytes,
            "raw_sha256":hashlib.sha256(raw).hexdigest(),"store_sha256":hashlib.sha256(destination.read_bytes()).hexdigest(),
            "zstandard_python_version":zstandard.__version__,"codec_version":list(zstandard.ZSTD_VERSION)}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest",type=Path,required=True,help="JSON {raw,channels,payload_paths}; paths relative to manifest")
    ap.add_argument("--output",type=Path,required=True)
    ap.add_argument("--reference",type=Path)
    args=ap.parse_args()
    if args.manifest.stat().st_size>8<<20:raise ValueError("manifest exceeds 8 MiB")
    with args.manifest.open("rb") as stream: manifest_bytes=stream.read((8<<20)+1)
    if len(manifest_bytes)>8<<20: raise ValueError("manifest grew beyond 8 MiB")
    request=json.loads(manifest_bytes);parent=args.manifest.resolve().parent
    if set(request)!={"raw","channels","payload_paths"}:raise ValueError("manifest fields differ")
    paths=request["payload_paths"]
    if not isinstance(paths,list) or not all(isinstance(p,str) for p in paths):raise ValueError("payload_paths must be a list of paths")
    def rows():
        for p in paths:
            file=parent/p
            if file.stat().st_size>1<<20:raise ValueError("payload exceeds row limit")
            with file.open("rb") as stream: data=stream.read((1<<20)+1)
            if len(data)>1<<20:raise ValueError("payload grew beyond row limit")
            yield data
    result=write_store(args.output,parent/request["raw"],request["channels"],rows(),args.reference)
    print(json.dumps(result,indent=2))
if __name__=="__main__":main()
