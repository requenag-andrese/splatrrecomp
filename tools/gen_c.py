"""Run totk-pc-recomp's C generator (recomp/gen.py) with the srrecomp lifter extension.

usage: python tools/gen_c.py --vendor <dir> [--patches-dir <dir>] -- <gen.py arguments: --exefs DIR --out DIR
                                                                      [--modules ...] ...>

--patches-dir: code patch sets by game version (srrecomp/patches). The file named after the module id of --exefs's
main is given to gen.py as --patches; a game with no file there is generated without sets.

gen.py builds `module.Module` objects in its own process and in spawned workers; each one gets its Lifter wrapped in
srrecomp.lifter_ext.ExtLifter. The wrap happens at import time of this file, which spawned workers re-import as
__mp_main__, so it covers them too. The vendored code is not modified. --out must lie outside this repository: the
generated C is derived from game code.
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_VENDOR = os.environ.get('SRRECOMP_VENDOR')


def _install(vendor):
    totk = os.path.join(vendor, 'totk-pc-recomp')
    for p in (REPO, os.path.join(totk, 'tools'), os.path.join(totk, 'recomp')):
        if p not in sys.path:
            sys.path.insert(0, p)
    import lifter as vlifter
    import module as vmodule
    from srrecomp.lifter_ext import ExtLifter

    if getattr(vmodule.Module, '_srrecomp_ext', False):
        return
    orig = vmodule.Module.__init__

    def init(self, *args, **kwargs):
        orig(self, *args, **kwargs)
        self.lifter = ExtLifter(self.lifter, vlifter)

    vmodule.Module.__init__ = init
    vmodule.Module._srrecomp_ext = True


if _VENDOR:
    _install(_VENDOR)


def patches_for(directory, gen_args):
    """The patch file of the game in gen.py's --exefs: <directory>/<main's module id in hex>.txt, None without one."""
    if '--exefs' not in gen_args:
        return None
    main = os.path.join(gen_args[gen_args.index('--exefs') + 1], 'main')
    with open(main, 'rb') as f:
        head = f.read(0x60)
    if head[:4] != b'NSO0':
        return None
    path = os.path.join(directory, head[0x40:0x54].hex() + '.txt')
    return path if os.path.isfile(path) else None


def main():
    argv = sys.argv[1:]
    if '--vendor' not in argv or '--' not in argv:
        sys.exit(__doc__)
    vendor = os.path.abspath(argv[argv.index('--vendor') + 1])
    gen_args = argv[argv.index('--') + 1:]
    if '--out' in gen_args:
        out = os.path.abspath(gen_args[gen_args.index('--out') + 1])
        if os.path.commonpath([out, REPO]) == REPO:
            sys.exit('refusing to write generated C inside the repository')
    if '--patches-dir' in argv[:argv.index('--')]:
        table = patches_for(argv[argv.index('--patches-dir') + 1], gen_args)
        if table:
            gen_args += ['--patches', table]
    os.environ['SRRECOMP_VENDOR'] = vendor  # inherited by spawned workers
    _install(vendor)
    import gen
    sys.argv = ['gen.py', *gen_args]
    return gen.main()


if __name__ == '__main__':
    sys.exit(main())
