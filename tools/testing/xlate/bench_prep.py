# One program in every N of shader-programs.bin: a smaller shader-programs.bin (for lower_test) and the same programs as
# translator batch jobs (jobs.json, debug off as the runtime asks), with the default answers. Both translators take it:
#   python -I bench_prep.py <build>/shader-programs.bin <dir> 20
#   sr-xlate.exe <dir>/jobs.json <dir>/ours         (DOTNET_ROOT set to the workspace's .toolchains/dotnet)
#   eden-xlate.exe <dir>/jobs.json <dir>/eden
#   python -I cmp_res.py <dir>/ours <dir>/eden ; outputs.py / vsio.py the same ; validate.py <dir>/eden
import json, os, struct, sys
src, outdir, every = sys.argv[1], sys.argv[2], int(sys.argv[3])
b = open(src, 'rb').read()
head = 12 + 32 + 0x880
os.makedirs(os.path.join(outdir, 'code'), exist_ok=True)
sub, jobs, o, i = [b[:16]], [], 16, 0
while len(b) - o >= head:
    stage, ln, c1 = struct.unpack_from('<III', b, o)
    size = head + ln + c1
    if i % every == 0:
        rec = b[o:o + size]
        sub.append(rec)
        control = rec[44:44 + 0x880]
        code = rec[head:head + ln]
        key = rec[12:20].hex()
        path = os.path.join(outdir, 'code', key + '.bin')
        open(path, 'wb').write(code)
        cs = stage == 5
        job = {'key': key, 'stage': 'cs' if cs else 'ps' if stage == 4 else 'vs', 'code': path,
               'c1': rec[head + ln:head + ln + c1].hex(), 'arch': 'sm86', 'debug': False, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}}
        if cs:
            c0 = struct.unpack_from('<H', control, 0xba)[0]
            job['nvn2_cs'] = [control[0xb1], control[0xb2], control[0xb3], c0, control[0xa9] * 4]
            job['local'] = list(struct.unpack_from('<III', control, 0x748))
            job['shared'] = struct.unpack_from('<I', control, 0x754)[0]
        jobs.append(job)
    o += size
    i += 1
open(os.path.join(outdir, 'sample-programs.bin'), 'wb').write(b''.join(sub))
json.dump(jobs, open(os.path.join(outdir, 'jobs.json'), 'w'))
print(len(jobs), 'programs of', i)
