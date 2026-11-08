# Gad — ドキュメント

Version 0.2.0 · 日本語

## ガイドを選ぶ

各言語にクイックスタート、ユーザーガイド、開発者ガイドがあります。ユーザーガイドには、その言語の実際の画面をライトテーマで撮影した22枚の画像が含まれています。

| ドキュメント | ソース | HTML | PDF |
| --- | --- | --- | --- |
| クイックスタート | [Markdown](QUICK_START.ja-JP.md) | [HTML](html/quick-start.ja-JP.html) | [PDF](pdf/quick-start.ja-JP.pdf) |
| ユーザーガイド | [Markdown](USER_GUIDE.ja-JP.md) | [HTML](html/user-guide.ja-JP.html) | [PDF](pdf/user-guide.ja-JP.pdf) |
| 開発者ガイド | [Markdown](DEVELOPER_GUIDE.ja-JP.md) | [HTML](html/developer-guide.ja-JP.html) | [PDF](pdf/developer-guide.ja-JP.pdf) |

18冊のガイドをMarkdown、単独で閲覧できるHTML、PDFで提供します。英語版PDFはUS Letter、その他はA4です。

[テストとシナリオ](../TESTING.ja-JP.md)

## 他の言語

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md)

## 再生成 {#regeneration}

撮影にはデモ用エンジン、一時ファイル、隔離されたセッションを使用します。実際の転送やデスクトップのクリップボードは操作しません。

書き出しにはMarkdown、Graphviz、WeasyPrintを利用できるPython環境が必要です。画面撮影にはGadのネイティブビルド依存関係も必要です。

```sh
make docs-captures
make docs
make docs-check
```

132 captures · 18 HTML · 18 PDF
