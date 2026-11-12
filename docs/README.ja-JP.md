# Gad — ドキュメント

Version 0.4.0 · 日本語

## ガイドを選ぶ

各言語にクイックスタート、ユーザーガイド、開発者ガイドがあります。ユーザーガイドには、その言語の実際の画面をライトテーマで撮影した22枚の画像が含まれています。

| ドキュメント | ソース | HTML |
| --- | --- | --- |
| クイックスタート | [Markdown](QUICK_START.ja-JP.md) | [HTML](html/quick-start.ja-JP.html) |
| ユーザーガイド | [Markdown](USER_GUIDE.ja-JP.md) | [HTML](html/user-guide.ja-JP.html) |
| 開発者ガイド | [Markdown](DEVELOPER_GUIDE.ja-JP.md) | [HTML](html/developer-guide.ja-JP.html) |

18冊のガイドをMarkdownとHTMLで提供します。

[テストとシナリオ](../TESTING.ja-JP.md)

## 他の言語

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md)

## 再生成 {#regeneration}

撮影にはデモ用エンジン、一時ファイル、隔離されたセッションを使用します。実際の転送やデスクトップのクリップボードは操作しません。

書き出しにはMarkdownを利用できるPython環境が必要です。画面撮影にはGadのネイティブビルド依存関係も必要です。

```sh
make docs-captures
make docs
make docs-check
```

138 captures · 18 HTML
