# hypertxpg-maker

`htg` (HyperTxPG) — ノーコードでテキストRPGを作成・実行できるCLIツール。

C言語(C11)で実装。POSIX標準ライブラリのみを使用し、外部ライブラリ非依存で
`gcc` によりOS非依存にビルドできます。

## ビルド

```sh
make          # ./htg を生成
make clean    # 生成物を削除
```

## 使い方

```
htg new <project_name>          # 新規 .htgp を生成
htg edit <project.htgp>         # 対話式エディタを開く
htg run <project.htgp|.htgb>    # ゲームを実行(プレイ)
htg compile <project.htgp>      # .htgp -> .htgb にコンパイル(難読化)
htg version                     # バージョン表示
htg help                        # ヘルプ表示
```

## 実装状況

仕様書(`htg_spec.md`)の推奨実装順序に沿って段階的に実装しています。

### 実装済み(セクション1〜2)

- **JSONパーサ/シリアライザ**(自前実装 / `src/json.c`, `src/json.h`)
  - オブジェクト・配列・文字列・数値・真偽値・null、UTF-8/`\uXXXX`(サロゲート対応)
  - プリティ出力に対応したシリアライザ
- **データモデル**(`src/model.c`, `src/model.h`)
  - 仕様セクション2の `.htgp` スキーマ(meta / flags / vars / rooms / items /
    actors / skills / events)を型付き構造体へロード
  - 装備(`equip`)、効果(`effect`)、パーティ(`recruitable` / `max_party_size`)、
    複合条件用の `condition` 文字列、`choices[].join_party` 等の項目も保持
  - 未モデル化フィールドも元のJSONツリー(`source`)を保持することで
    ロード/セーブのラウンドトリップで失われないよう配慮
  - `.htgp` の読み込み・保存(プリティJSON)
- **コマンドディスパッチ**(`src/main.c`)
  - `htg version` / `htg help` / `htg new` を実装
  - `htg edit` / `htg run` / `htg compile` はディスパッチと入力検証
    (拡張子判定・プロジェクトのロード検証)まで実装。各機能本体は後続段階で実装

### 未実装(セクション3以降)

- 対話式エディタ本体(CRUD)/ ゲーム実行エンジン / 戦闘システム /
  セーブ・ロード / `.htgb` コンパイル(XOR難読化)

## ソース構成

```
src/json.c   src/json.h    自前JSONパーサ/シリアライザ
src/model.c  src/model.h   .htgp データモデル(ロード/セーブ)
src/main.c                 CLIエントリポイント/コマンドディスパッチ
Makefile
```
