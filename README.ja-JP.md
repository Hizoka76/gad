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


**0.3.0** — コピー先とオプションを選び、Gad にファイルを追加します。転送中でなければリストを確認して **開始** を押します。ファイルマネージャーからの外部貼り付けは自動開始し、転送中の追加はキューに入ります。**Ctrl+C** はコピー、**Ctrl+X** は移動です。ドラッグ＆ドロップは常にコピーを準備します。

## 文書 {#documentation}

このREADMEは出発点であり、ガイドは完全な動作と制限を記述するため、詳細な指示を2回維持する必要はありません。

| 聴衆 | 英語(アメリカ) | フランセー |
|---|---|---|
| ユーザ | [ユーザーガイド](docs/USER_GUIDE.ja-JP.md) | [ガイドイラスト](docs/GUIDE_UTILISATEUR.md) |
| 開発者 | [アーキテクチャとプロトコル](docs/DEVELOPER_GUIDE.ja-JP.md) | [アーキテクチャとプロトコル](docs/GUIDE_DEVELOPPEUR.md) |
| テスト | [検証](TESTING.ja-JP.md) | [テスト](TESTING.md) |

[すべてのHTMLとMarkdownエディション](docs/README.ja-JP.md)

## インストール {#installation}

### Ubuntu PPA

[Gad PPA](https://launchpad.net/~hizo/+archive/ubuntu/gad)を使うと、APT経由でGadをインストールし、更新を受け取れます。

```sh
sudo add-apt-repository ppa:hizo/gad
sudo apt update
sudo apt install gad
```

PPAのページで、お使いのUbuntuのバージョン向けにパッケージが公開されているか確認してください。`add-apt-repository`がない場合は`software-properties-common`をインストールしてください。

### コンパイル済みファイルのダウンロード

[GitHubのリリース](https://github.com/Hizoka76/gad/releases/latest)では複数の形式を提供しています。以下の例のファイル名は、ダウンロードした実際の名前に置き換えてください。

#### Debianパッケージ `.deb`

お使いのアーキテクチャに合うパッケージをダウンロードし、APTでインストールすると、リポジトリで利用可能な依存パッケージも解決されます。

```sh
sudo apt install ./gad_VERSION_amd64.deb
```

#### `.tar.gz`アーカイブ：ソースとコンパイル済みプログラム

`gad-VERSION-linux-amd64.tar.gz`には、ソースコード、同梱のRust依存ソース、ドキュメント、`prebuilt/`内のコンパイル済みプログラムが含まれます。実行するためにコンパイルする必要はありません。

```sh
tar -xzf gad-VERSION-linux-amd64.tar.gz
cd gad-VERSION
./prebuilt/usr/bin/gad
```

#### 実行ファイルの直接ダウンロード

`gad-linux-amd64`もコンパイル済みの実行ファイルです。

```sh
chmod +x gad-linux-amd64
./gad-linux-amd64
```

Linux amd64のダウンロードはx86-64プロセッサ向けです。実行ファイル単体ではランチャーやDolphinモジュールはインストールされません。`.deb`とアーカイブには、そのバージョンに付属する連携ファイルが含まれます。Qt/KF6は別途必要で、対応バージョンはリリースノートに記載されています。リリースの内容は対応するタグの時点のもので、リポジトリの最新コードより古い場合があります。

### ソースからインストール

Linux にインストールします。 RustC++17コンパイラであるCargo `pkg-config`, Qt 6 ウィジェット/ネットワーク/ネットワーク Qt ビルドツール、KF6 WindowSystem/XmlGui, CMake、Python 3.Waylandクリップボードのサポートが使用されている。 `wl-clipboard`; リモート URL が必要です。 KIO ヒューズ。 [開発者ガイド](docs/DEVELOPER_GUIDE.ja-JP.md#building).

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

通常の Dolphin ランチャーを保護するには、次のコマンドで監視モジュールをインストールし、既存のウィンドウを一度閉じて開き直してください。毎秒ショートカットを確認して Gad の操作を修復し、テキスト欄では通常の貼り付けを維持します。

```sh
python3 scripts/dolphin_integration.py --install --protect-launcher "$HOME/.local/bin/gad"
```


他のファイルマネージャの場合:[ペーストして Gadドラッグ&ドロップ、カスタムアクション](docs/USER_GUIDE.ja-JP.md#using-gad-with-other-file-managers)。

雲道は、その種類によって異なる。 Gad日記とバックアップを **完了した操作をクリアする** (クリア終了) アンド履歴も削除します [ユーザーガイド](docs/USER_GUIDE.ja-JP.md) 履歴をクリアする前に

## 開発と検証 {#development-and-validation}

```bash
make doctor
make test
make docs
```

テストは一時的なファイルと分離セッションを使用し、Gad.[コマンドと要件のテスト](TESTING.ja-JP.md)のデスクトップインスタンスを制御しません。
