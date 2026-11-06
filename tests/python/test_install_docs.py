"""Les fichiers exportés sont distribués avec un catalogue cohérent, sans moteur de rendu."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
import install_docs
from doc_catalog import GUIDES,LABELS,runtime_catalog


class InstallDocsTest(unittest.TestCase):
    def test_runtime_catalog_matches_all_guide_exports(self):
        catalog=runtime_catalog()
        self.assertEqual(catalog['format'],'gad-documentation')
        self.assertEqual(len(catalog['guides']),len(GUIDES))
        for guide,entry in zip(GUIDES,catalog['guides']):
            self.assertEqual(entry['locale'],LABELS[guide.language]['locale'])
            self.assertEqual(entry['kind'],guide.audience)
            self.assertEqual(entry['html'],'html/'+guide.slug+'.html')
            self.assertEqual(entry['pdf'],'pdf/'+guide.slug+'.pdf')

    def test_install_copies_exports_and_preserves_unrelated_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source';destination=root/'installed docs'
            for folder in ('html','pdf'):
                (source/folder).mkdir(parents=True)
                (source/folder/('guide.'+folder)).write_bytes(b'new export')
            destination.mkdir();custom=destination/'custom.txt';custom.write_text('keep')
            catalog={'format':'gad-documentation','version':1,'guides':[{'locale':'ja_JP','kind':'quick','title':'クイックスタート','html':'html/guide.html','pdf':'pdf/guide.pdf'}]}
            with patch.object(install_docs,'runtime_catalog',return_value=catalog):
                install_docs.install(destination,source)
                install_docs.install(destination,source)
            self.assertEqual((destination/'html/guide.html').read_bytes(),b'new export')
            self.assertEqual((destination/'pdf/guide.pdf').read_bytes(),b'new export')
            self.assertEqual(json.loads((destination/'catalog.json').read_text()),catalog)
            self.assertEqual(custom.read_text(),'keep')
            self.assertFalse(list(destination.rglob('*.install')))

    def test_missing_exports_do_not_require_rendering_tools(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            with patch.object(install_docs,'runtime_catalog',return_value={'format':'gad-documentation','version':1,'guides':[{'html':'html/missing.html','pdf':'pdf/missing.pdf'}]}):
                install_docs.install(root/'destination',root/'source')
            self.assertTrue((root/'destination/catalog.json').is_file())
