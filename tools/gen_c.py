"""Run totk-pc-recomp's C generator (recomp/gen.py) with the srrecomp lifter extension.

usage: python tools/gen_c.py --vendor <dir> -- <gen.py arguments: --exefs DIR --out DIR [--modules ...] ...>

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
    os.environ['SRRECOMP_VENDOR'] = vendor  # inherited by spawned workers
    _install(vendor)
    import gen
    sys.argv = ['gen.py', *gen_args]
    return gen.main()


if __name__ == '__main__':
    sys.exit(main())
