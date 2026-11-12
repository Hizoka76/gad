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
            self.assertEqual(entry['md'],guide.source)

    def test_install_copies_exports_and_preserves_unrelated_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source';destination=root/'installed docs'
            (source/'html').mkdir(parents=True)
            (source/'html/guide.html').write_bytes(b'new export')
            (source/'guide.md').write_bytes(b'new source')
            (source/'images').mkdir();(source/'images/overview.png').write_bytes(b'image')
            destination.mkdir();custom=destination/'custom.txt';custom.write_text('keep')
            catalog={'format':'gad-documentation','version':1,'guides':[{'locale':'ja_JP','kind':'quick','title':'クイックスタート','html':'html/guide.html','md':'guide.md'}]}
            with patch.object(install_docs,'runtime_catalog',return_value=catalog):
                install_docs.install(destination,source)
                install_docs.install(destination,source)
            self.assertEqual((destination/'html/guide.html').read_bytes(),b'new export')
            self.assertEqual((destination/'guide.md').read_bytes(),b'new source')
            self.assertEqual(json.loads((destination/'catalog.json').read_text()),catalog)
            self.assertEqual(custom.read_text(),'keep')
            self.assertEqual((destination/'images/overview.png').read_bytes(),b'image')
            self.assertFalse(list(destination.rglob('*.install')))

    def test_missing_exports_do_not_require_rendering_tools(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            with patch.object(install_docs,'runtime_catalog',return_value={'format':'gad-documentation','version':1,'guides':[{'html':'html/missing.html','md':'missing.md'}]}):
                install_docs.install(root/'destination',root/'source')
            self.assertTrue((root/'destination/catalog.json').is_file())

    def test_install_removes_only_owned_obsolete_editions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=root/'source';destination=root/'docs';(destination/'pdf').mkdir(parents=True)
            obsolete=destination/'pdf'/(GUIDES[0].slug+'.pdf');obsolete.write_text('old')
            personal=destination/'pdf/personal.pdf';personal.write_text('keep')
            install_docs.install(destination,source)
            self.assertFalse(obsolete.exists());self.assertEqual(personal.read_text(),'keep')

    def test_preexisting_temporary_links_cannot_overwrite_other_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, destination = root/'source', root/'docs'
            source.mkdir();destination.mkdir()
            (source/'guide.md').write_bytes(b'guide')
            unrelated = root/'unrelated';unrelated.write_bytes(b'keep')
            for name in ('.guide.md.install', '.catalog.json.install'):
                (destination/name).symlink_to(unrelated)
            with patch.object(install_docs, 'runtime_catalog', return_value={'guides':[{'html':'missing.html','md':'guide.md'}]}):
                install_docs.install(destination, source)
            self.assertEqual(unrelated.read_bytes(), b'keep')
            self.assertEqual((destination/'guide.md').read_bytes(), b'guide')
            for name in ('.guide.md.install', '.catalog.json.install'):
                self.assertTrue((destination/name).is_symlink())
