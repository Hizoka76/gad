"""Vérifie le rendu multilingue sans installer Markdown, WeasyPrint ni lancer de capture."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import build_docs
from doc_catalog import GUIDES, LANGUAGES, AUDIENCES, peer


class DocumentationTest(unittest.TestCase):
    def test_catalog_has_both_audiences_in_each_language(self):
        self.assertEqual({(g.language, g.audience) for g in GUIDES},
                         {(language,audience) for language in LANGUAGES for audience in AUDIENCES})
        self.assertEqual(len({g.slug for g in GUIDES}), len(GUIDES))
        for guide in GUIDES:
            self.assertEqual(peer(guide).audience, guide.audience)
            self.assertNotEqual(peer(guide).language, guide.language)

    def test_us_page_has_english_navigation_and_letter_paper(self):
        guide = next(g for g in GUIDES if g.language == "en-US" and g.audience == "user")
        page = build_docs.page_html(guide, "<p>Contents</p>", "@page {size:A4;}")
        self.assertIn('lang="en-US"', page)
        self.assertIn('href="developer-guide.en-US.html"', page)
        self.assertIn('href="guide-utilisateur.html">Français', page)
        self.assertIn("size: Letter", page)
        self.assertIn("Gad — User Guide", page)

    def test_french_page_keeps_its_language_and_a4(self):
        guide = next(g for g in GUIDES if g.language == "fr" and g.audience == "developer")
        page = build_docs.page_html(guide, "<p>Texte</p>", "@page {size:A4;}")
        self.assertIn('lang="fr"', page)
        self.assertIn("size:A4", page)
        self.assertNotIn("size: Letter", page)
        self.assertIn('href="developer-guide.en-US.html">English (US)', page)

    def test_link_rewriting_preserves_fragments_and_external_urls(self):
        cases = {
            "USER_GUIDE.en-US.md#remaining-time": "user-guide.en-US.html#remaining-time",
            "GUIDE_DEVELOPPEUR.md#architecture": "guide-developpeur.html#architecture",
            "../TESTING.en-US.md#running-validation": "../../TESTING.en-US.md#running-validation",
            "README.en-US.md": "../README.en-US.md",
            "../native/window.cpp": "../../native/window.cpp",
            "#contents": "#contents",
            "https://example.org/manual.md#section": "https://example.org/manual.md#section",
            "mailto:example@example.org": "mailto:example@example.org",
        }
        for source, expected in cases.items():
            with self.subTest(source=source):
                self.assertEqual(build_docs.rewrite_links(f'<a href="{source}">Link</a>'), f'<a href="{expected}">Link</a>')

    def test_cover_captions_and_offline_images_use_guide_language(self):
        guide = next(g for g in GUIDES if g.language == "en-US" and g.audience == "user")
        with tempfile.TemporaryDirectory() as temp:
            docs = Path(temp)
            (docs / "images/en-US").mkdir(parents=True)
            (docs / "images/en-US/captures.json").write_text(json.dumps({"device_pixel_ratio": 2, "captures": [{"file": "dialog.png", "width": 400}]}))
            source = docs / guide.source
            source.write_text("# User Guide")
            rendered = '<h1 id="guide">User Guide</h1><h2 id="troubleshooting">Troubleshooting</h2><p><img src="images/en-US/dialog.png" alt="Dialog"></p><p><em>Real widgets.</em></p>'
            with patch.object(build_docs, "DOCS", docs), patch.object(build_docs, "render_markdown", return_value=rendered), patch.object(build_docs, "image_uri", return_value="data:image/png;base64,YQ=="):
                body = build_docs.render_body(source, guide)
            self.assertIn("Illustrated User Guide", body)
            self.assertIn('class="chapter troubleshooting"', body)
            self.assertIn("<figcaption>Real widgets.</figcaption>", body)
            self.assertIn("width:200px", body)
            self.assertNotIn('src="images/', body)

    def test_quick_start_print_layout_is_compact(self):
        guide = next(g for g in GUIDES if g.language == "ja-JP" and g.audience == "quick")
        page = build_docs.page_html(guide, "<p>Start</p>", "")
        self.assertIn('lang="ja-JP"', page)
        self.assertIn("break-before:auto", page)
        self.assertIn("break-after:auto", page)
        self.assertNotIn("size: Letter", page)

    def test_user_guides_reference_their_own_light_screenshots(self):
        import re
        for guide in GUIDES:
            if guide.audience != "user":
                continue
            text = (build_docs.DOCS / guide.source).read_text()
            captures = re.findall(r"images/([^/]+)/([0-9][0-9]-[^)]+\.png)", text)
            self.assertEqual(len({name for _, name in captures}), 13, guide.language)
            self.assertEqual({language for language, _ in captures}, {guide.language})
            manifest = json.loads((build_docs.DOCS / "images" / guide.language / "captures.json").read_text())
            self.assertEqual(manifest["theme"], "light")
            self.assertEqual(manifest["language"], guide.language)

    def test_translated_code_examples_are_preserved(self):
        import re
        for guide in GUIDES:
            if guide.language in ("fr", "en-US"):
                continue
            original = next(g for g in GUIDES if g.language == "en-US" and g.audience == guide.audience)
            code = lambda text: re.findall(r"^```[^\n]*\n[\s\S]*?^```", text, re.M)
            self.assertEqual(code((build_docs.DOCS / guide.source).read_text()), code((build_docs.DOCS / original.source).read_text()), guide.source)

    def test_source_without_a_title_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "guide.md"
            source.write_text("Missing title")
            with patch.object(build_docs, "render_markdown", return_value="<p>No heading</p>"):
                with self.assertRaisesRegex(ValueError, "h1"):
                    build_docs.render_body(source, GUIDES[0])

    def test_export_builds_every_guide_with_structured_arguments(self):
        with tempfile.TemporaryDirectory(prefix="gad docs test ") as temp:
            docs = Path(temp) / "docs"
            (docs / "tools").mkdir(parents=True)
            (docs / "tools/documentation.css").write_text("body {}")
            for guide in GUIDES:
                (docs / guide.source).write_text("# Guide")

            def render_pdf(arguments, **kwargs):
                self.assertIsInstance(arguments, list)
                self.assertEqual(kwargs, {"check": True})
                Path(arguments[2]).write_bytes(b"%PDF-1.7\n")

            with patch.object(build_docs, "DOCS", docs), patch.object(build_docs, "render_body", return_value="<p>Guide</p>"), patch.object(build_docs.shutil, "which", return_value="/mock/weasyprint"), patch.object(build_docs.subprocess, "run", side_effect=render_pdf) as run, contextlib.redirect_stdout(io.StringIO()):
                build_docs.main()
            self.assertEqual(run.call_count, len(GUIDES))
            for guide in GUIDES:
                self.assertIn(f'lang="{guide.language}"', (docs / "html" / (guide.slug + ".html")).read_text())
                self.assertTrue((docs / "pdf" / (guide.slug + ".pdf")).read_bytes().startswith(b"%PDF"))


if __name__ == "__main__":
    unittest.main()
