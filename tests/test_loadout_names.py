"""runtime/sr/loadout_names.c: the Loadout tab's lists, built from a text archive like the game's (a SARC of MSBT
files) that the test makes up, and the part file names and gadget rows it gives (tests/loadout_names_harness.c)."""
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLANG = sorted((ROOT.parent / '.toolchains').glob('llvm-mingw-*/bin/clang.exe'))


def msbt(messages, buckets=3):
    """An MSBT: LBL1 (labels hashed into buckets as the game's are, here by length) and TXT2 (UTF-16)."""
    labels = list(messages)
    groups = [[] for _ in range(buckets)]
    for i, name in enumerate(labels):
        groups[len(name) % buckets].append((name, i))
    head = struct.pack('<I', buckets)
    body, offset = b'', 4 + 8 * buckets
    for g in groups:
        head += struct.pack('<II', len(g), offset + len(body))
        for name, i in g:
            body += bytes([len(name)]) + name.encode() + struct.pack('<I', i)
    lbl = head + body
    texts = [messages[k] for k in labels]
    tbody, offs = b'', []
    for t in texts:
        offs.append(4 + 4 * len(texts) + len(tbody))
        tbody += t.encode('utf-16-le') if isinstance(t, str) else t
        tbody += b'\0\0'
    txt = struct.pack('<I', len(texts)) + b''.join(struct.pack('<I', o) for o in offs) + tbody

    def section(magic, data):
        return magic + struct.pack('<I', len(data)) + bytes(8) + data + bytes(-len(data) % 16)

    sections = section(b'LBL1', lbl) + section(b'ATR1', b'\0' * 8) + section(b'TXT2', txt)
    header = b'MsgStdBn' + b'\xff\xfe' + bytes(2) + bytes([1, 3]) + struct.pack('<H', 3) + bytes(2)
    header += struct.pack('<I', 0x20 + len(sections)) + bytes(10)
    return header + sections


def sarc(files):
    """A SARC with these members (name -> bytes)."""
    names = list(files)
    sfnt, name_offs = b'', []
    for n in names:
        name_offs.append(len(sfnt) // 4)
        sfnt += n.encode() + b'\0'
        sfnt += bytes(-len(sfnt) % 4)
    data, nodes = b'', b''
    for n, off in zip(names, name_offs):
        data += bytes(-len(data) % 8)
        start = len(data)
        data += files[n]
        nodes += struct.pack('<IIII', 0, 0x01000000 | off, start, len(data))
    sfat = b'SFAT' + struct.pack('<HHI', 12, len(names), 0x65) + nodes
    sfnt_sec = b'SFNT' + struct.pack('<HH', 8, 0) + sfnt
    data_at = 0x14 + len(sfat) + len(sfnt_sec)
    data_at += -data_at % 8
    head = b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, data_at + len(data), data_at, 0x100, 0)
    body = head + sfat + sfnt_sec
    return body + bytes(data_at - len(body)) + data


def archive(**extra):
    files = {
        'Glossary/WeaponName.msbt': msbt({'Roller_Heavy_B': 'Turbine Roller', 'Shooter_A': 'Plastic Bottle',
                                          'Roller_Heavy_A': 'Duct Roller', 'Shooter_A_Weakest': 'Wimpy one',
                                          'Shooter_Short_F': 'Primo Floatie', 'Brush_C': 'Tour Brush'}),
        'Glossary/WeaponCategory.msbt': msbt({'Shooter': 'Shooters', 'Roller': 'Rollers', 'Brush': 'Brushes'}),
        'Glossary/GadgetName.msbt': msbt({'Boomerang': 'Booyarang', 'Hook': 'Flywire', 'Slam': 'Blast Boot',
                                          'SpMorayShower': 'Moray Storm'}),
        'Glossary/RelicPower.msbt': msbt({'Wpn_ToFrozen': 'Freeze', 'Str_ShortDmgUp': 'Close Combat',
                                          'Cmn_MultiJump': 'Midair Jump', 'Cmn_InkMaster': 'Ink Pro'}),
        'Glossary/NpcName.msbt': msbt({'Fuka': 'Shiver', 'Utsuho': 'Frye', 'Mantaro': 'Big Man'}),
        'Glossary/SystemName.msbt': msbt({'PlayerStyle_Agility_00': 'Speed Tank', 'PlayerStyle_Strength_00': 'Power Tank',
                                          'PlayerStyle_Technique_00': 'Tactical Tank'}),
        'Glossary/GadgetPartsType.msbt': msbt({'Boom_Multi': 'Multirang', 'Boom_SizeUpXZ': 'Area Up',
                                               'Hook_HookRangeUp': 'Wire Length Up', 'Boom_FowardDmgUp': 'Forward Damage',
                                               'Boom_SizeUp': 'Size Up'}),
        'CommonMsg/Gadget/GadgetPartsTypeExp.msbt': msbt({'Boom_Multi': 'Throws more Booyarangs.'}),
    }
    files.update(extra)
    return sarc(files)


PARTS = ['Boom_11_Multi_02', 'Boom_01_SizeUpXZ_10', 'Boom_11_Multi_01', 'Boom_01_SizeUpXZ_02', 'Hook_04_HookRangeUp_01',
         'Slam_05_Odd', 'NotAPart']
GADGETS = ['Boomerang Boom 0 2', 'Hook Hook 0 4', 'Slam Slam 0 0', 'SpMorayShower Mory 3 0', 'Missing Miss 1 0']


@unittest.skipUnless(CLANG, 'llvm-mingw is not in .toolchains')
class LoadoutNamesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.dir.name) / 'loadout_names_harness.exe'
        sr = ROOT / 'runtime' / 'sr'
        subprocess.run([str(CLANG[-1]), '-O1', '-DLO_NAMES_NO_GAME', '-I', str(sr),
                        str(ROOT / 'tests' / 'loadout_names_harness.c'), str(sr / 'loadout_names.c'), '-o', str(cls.exe)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def names(self, data, parts=PARTS, gadgets=GADGETS, icons=None):
        d = Path(self.dir.name)
        (d / 'a.sarc').write_bytes(data)
        (d / 'parts.txt').write_text('\n'.join(parts))
        (d / 'gadgets.txt').write_text('\n'.join(gadgets))
        args = [str(self.exe), str(d / 'a.sarc'), str(d / 'parts.txt'), str(d / 'gadgets.txt')]
        if icons is not None:
            (d / 'icons.txt').write_text('\n'.join(icons))
            args.append(str(d / 'icons.txt'))
        r = subprocess.run(args, capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.splitlines()

    def test_weapons_by_category_and_kind_without_the_odd_ones(self):
        out = [line for line in self.names(archive()) if line.startswith(('weapon', 'cat'))]
        self.assertEqual(out, ['cat Shooter=Shooters', 'cat Roller=Rollers', 'cat Brush=Brushes',
                               'weapon Shooter_A=Plastic Bottle cat 0', 'weapon Shooter_Short_F=Primo Floatie cat 0',
                               'weapon Roller_Heavy_A=Duct Roller cat 1', 'weapon Roller_Heavy_B=Turbine Roller cat 1',
                               'weapon Brush_C=Tour Brush cat 2'])

    def test_relic_powers_first_then_the_weapons_own(self):
        out = [line for line in self.names(archive()) if line.startswith('power')]
        self.assertEqual(out, ['power Cmn_InkMaster=Ink Pro relic', 'power Cmn_MultiJump=Midair Jump relic',
                               'power Str_ShortDmgUp=Close Combat relic', 'power Wpn_ToFrozen=Freeze'])

    def test_tanks_and_buddies_take_the_games_names(self):
        out = [line for line in self.names(archive()) if line.startswith(('tank', 'buddy'))]
        self.assertEqual(out, ['tank Default=(none)', 'tank Speed=Speed Tank', 'tank Power=Power Tank',
                               'tank Tactical=Tactical Tank', 'buddy Shiver=Shiver', 'buddy Frye=Frye',
                               'buddy BigMan=Big Man'])

    def test_gadgets_in_the_games_order_with_their_part_types_by_level(self):
        out = [line for line in self.names(archive()) if line.startswith(('gadget', '  type'))]
        self.assertEqual(out, [
            'gadget Slam=Blast Boot',
            'gadget Boomerang=Booyarang',
            '  type Boom_SizeUpXZ=Area Up desc (none): Boom_01_SizeUpXZ_02 Boom_01_SizeUpXZ_10',
            '  type Boom_Multi=Multirang desc Throws more Booyarangs.: Boom_11_Multi_01 Boom_11_Multi_02',
            'gadget Hook=Flywire',
            '  type Hook_HookRangeUp=Wire Length Up desc (none): Hook_04_HookRangeUp_01',
        ])   # no Showstopper, no gadget the text archive does not name, no file that is not a part's

    def test_each_part_type_finds_its_icon_even_where_the_game_misspells_it(self):
        parts = ['Boom_02_ForwardDmgUp_01', 'Boom_11_Multi_01', 'Boom_01_SizeUpXZ_01', 'Boom_03_SizeUp_01',
                 'Hook_04_HookSizeRangeUp_01']
        icons = ['Boom_Multi', 'Boom_FowardDmgUp', 'Boom_SizeUp', 'Boom_SizeUpXZ', 'Hook_HookRangeUp', 'Hook_Other']
        out = [line.split(':')[0] for line in self.names(archive(), parts=parts, icons=icons) if line.startswith('  type')]
        self.assertEqual([line.rsplit(' icon ', 1)[1] for line in out],
                         ['Boom_SizeUpXZ', 'Boom_FowardDmgUp', 'Boom_SizeUp', 'Boom_Multi', 'Hook_HookRangeUp'])
        out = [line.split(':')[0] for line in self.names(archive(), parts=['Boom_11_Multi_01'], icons=['Boom_Other'])
               if line.startswith('  type')]
        self.assertTrue(out[0].endswith('icon (none)'), out)   # nothing near enough: no icon, not a wrong one

    def test_a_part_type_the_game_labels_otherwise_still_gets_its_name(self):
        parts = ['Boom_02_ForwardDmgUp_01', 'Boom_01_SizeUpXZ_01', 'Boom_03_SizeUp_01', 'Boom_04_ZoomIn_01']
        out = [line.split(' desc ')[0] for line in self.names(archive(), parts=parts) if line.startswith('  type')]
        self.assertEqual(out, ['  type Boom_SizeUpXZ=Area Up', '  type Boom_ForwardDmgUp=Forward Damage',
                               '  type Boom_SizeUp=Size Up', '  type Boom_ZoomIn=Boom_ZoomIn'])
        # Boom_SizeUp is a type of its own: its label is not taken for another; a type with no label near keeps its id

    def test_text_tags_are_not_names_and_accents_come_through(self):
        tagged = 'Pri'.encode('utf-16-le') + struct.pack('<HHHH', 0x0E, 0, 3, 2) + b'xx' + 'mo Ñu'.encode('utf-16-le')
        data = archive(**{'Glossary/WeaponName.msbt': msbt({'Shooter_A': tagged})})
        self.assertIn('weapon Shooter_A=Primo Ñu cat 0', self.names(data))

    def test_no_weapon_names_no_lists(self):
        self.assertEqual(self.names(sarc({'Glossary/GadgetName.msbt': msbt({'Hook': 'Flywire'})})), ['none'])
        self.assertEqual(self.names(b'not an archive at all, nothing to read here'), ['none'])

    def test_the_text_archive_of_each_language(self):
        for code, mals in (('en-US', 'USen'), ('en-GB', 'EUen'), ('es', 'EUes'), ('es-419', 'USes'), ('ja', 'JPja'),
                           ('zh-Hans', 'CNzh'), ('ru', 'USen'), ('', 'USen')):
            r = subprocess.run([str(self.exe), 'mals', code], capture_output=True, text=True)
            self.assertEqual(r.stdout.strip(), mals, code)


if __name__ == '__main__':
    unittest.main()
