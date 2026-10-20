#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
spec=importlib.util.spec_from_file_location('mkstore',Path(__file__).resolve().parents[1]/'tools/mkstore.py')
mkstore=importlib.util.module_from_spec(spec); spec.loader.exec_module(mkstore)
ROOT=Path(__file__).resolve().parents[1]
class Generator(unittest.TestCase):
    def test_generation_and_immutability(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'store'
            mkstore.build(ROOT/'store/apps.json',ROOT/'out/pkg',out)
            text=(out/'catalog.txt').read_bytes()
            expected={app['id'] for app in json.loads((ROOT/'store/apps.json').read_text())}
            self.assertEqual(text.count(b'[app]'),len(expected))
            self.assertEqual({line.split(b'=',1)[1].decode() for line in text.splitlines() if line.startswith(b'id=')},expected)
            for ident in ('minesweeper','solitaire'):
                self.assertIn(ident,expected)
                data,info,icon=mkstore.package(ROOT/'out/pkg'/f'{ident}.gzp')
                self.assertEqual((out/f'apps/{ident}/{info["version"]}/app.gzp').read_bytes(),data)
                self.assertEqual((out/f'apps/{ident}/{info["version"]}/icon.gzi').read_bytes(),icon)
            mkstore.build(ROOT/'store/apps.json',ROOT/'out/pkg',out)
            self.assertEqual(text,(out/'catalog.txt').read_bytes())
            asset=next(out.rglob('app.gzp')); asset.write_bytes(b'bad')
            with self.assertRaisesRegex(ValueError,'immutable'): mkstore.build(ROOT/'store/apps.json',ROOT/'out/pkg',out)
            self.assertEqual(text,(out/'catalog.txt').read_bytes())
    def test_bad_metadata(self):
        original=json.loads((ROOT/'store/apps.json').read_text())
        for key,value in [('id','../hello'),('description','bad\nline')]:
            with tempfile.TemporaryDirectory() as d:
                apps=json.loads(json.dumps(original))
                apps[0][key]=value
                path=Path(d)/'apps.json'; path.write_text(json.dumps(apps))
                with self.assertRaises(ValueError): mkstore.build(path,ROOT/'out/pkg',Path(d)/'out')
    def test_duplicate(self):
        with tempfile.TemporaryDirectory() as d:
            apps=json.loads((ROOT/'store/apps.json').read_text()); apps.append(apps[0])
            path=Path(d)/'apps.json'; path.write_text(json.dumps(apps))
            with self.assertRaisesRegex(ValueError,'duplicate'): mkstore.build(path,ROOT/'out/pkg',Path(d)/'out')
if __name__=='__main__': unittest.main()
