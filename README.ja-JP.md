# Gad {#gad}

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

**Gad, il copie.** — **Gestionnaire Avancé de Duplication** (高機能ファイル複製マネージャー)

[フランセ](README.md) · [クイックスタート](docs/QUICK_START.ja-JP.md)

Gadは、ファイルのコピーをもたらし、1つのネイティブKDE Plasmaウィンドウに移動. その転送エンジンは、C++ / Qt 6ウィジェットインターフェイスで、Rustで記述されています。

- フランス語、アメリカ英語、スペイン語、ポルトガル語、ドイツ語、日本語のインターフェースで、言語を自由に選べます。
- フォルダツリー、優先順位、およびドラッグ可能な行を持つ詳細なキュー。
- 最大2つの互換性のある転送を並列に、高速なリネーム、自動重複抑制。
- 一時停止制御、帯域幅制限、リクエスト毎の見積もり、およびPlasma進捗追跡。
- 読みやすいコンフリクト、MD5/プロパティ/空きスペースチェック、およびundoと履歴。
- 一部ファイルの復元、デバイス再接続待ち、失敗したファイルに限定された再試行。
- KDE接続、FTP、SFTP、SMB:KIOヒューズを介してローカルおよびリモートソースと宛先。

## 文書 {#documentation}

このREADMEは出発点であり、ガイドは完全な動作と制限を記述するため、詳細な指示を2回維持する必要はありません。

| 聴衆 | 英語(アメリカ) | フランセー |
|---|---|---|
| ユーザ | [ユーザーガイド](docs/USER_GUIDE.ja-JP.md) | [ガイドイラスト](docs/GUIDE_UTILISATEUR.md) |
| 開発者 | [アーキテクチャとプロトコル](docs/DEVELOPER_GUIDE.ja-JP.md) | [アーキテクチャとプロトコル](docs/GUIDE_DEVELOPPEUR.md) |
| テスト | [検証](TESTING.ja-JP.md) | [テスト](TESTING.md) |

[すべてのHTMLとPDFエディション](docs/README.ja-JP.md)

## インストール {#installation}

Linux にインストールします。 RustC++17コンパイラであるCargo `pkg-config`, Qt 6 ウィジェット/ネットワーク/ネットワーク Qt ビルドツール、KF6 WindowSystem、Python 3.Waylandクリップボードのサポートが使用されている。 `wl-clipboard`; リモート URL が必要です。 KIO ヒューズ。 [開発者ガイド](docs/DEVELOPER_GUIDE.ja-JP.md#building).

```bash
python3 scripts/install.py
gad
```

インストールはユーザーごとに行われ、管理者権限は不要です。 `gad --background` システムトレイから始めます

## Dolphinからコピーします。 {#copying-from-dolphin}

1. **Ctrl+C**でコピーするか、**Ctrl+X**でカットしてファイルを移動します。
2. 宛先フォルダを右クリックし、**Gadで貼り付けます。**(Gadで貼り付ける)を選択します。
3. キューに別のリクエストを追加します。

Dolphin の **Ctrl+V と Gad の連携** は、`gad --configure --dolphin-ctrl-v` で有効にし、`gad --configure --no-dolphin-ctrl-v` で無効にします。変更後は Dolphin を閉じて開き直してください。Gad 内の Ctrl+V は表示中のコピー先へ貼り付け、ドラッグ＆ドロップはファイルをコピーします。

他のファイルマネージャの場合:[ペーストして Gadドラッグ&ドロップ、カスタムアクション](docs/USER_GUIDE.ja-JP.md#using-gad-with-other-file-managers)。

雲道は、その種類によって異なる。 Gad日記とバックアップを **完了した操作をクリアする** (クリア終了) アンド履歴も削除します [ユーザーガイド](docs/USER_GUIDE.ja-JP.md) 履歴をクリアする前に

## 開発と検証 {#development-and-validation}

```bash
make doctor
make test
make docs
```

テストは一時的なファイルと分離セッションを使用し、Gad.[コマンドと要件のテスト](TESTING.ja-JP.md)のデスクトップインスタンスを制御しません。
