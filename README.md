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

### 実装済み(セクション1〜4)

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
- **共通CLI UIヘルパー**(`src/ui.c`, `src/ui.h`)
  - 仕様セクション4のメニュー様式(`=== タイトル ===` + 番号入力 + `>> `)を統一提供
  - 数値/文字列/y-n入力、範囲外・非数値の再入力ループ、空Enterスキップに対応
- **対話式エディタ**(`src/editor.c`, `src/editor.h` / 仕様セクション3〜5)
  - トップメニュー(4.1)と各カテゴリ共通サブメニュー(4.2: 新規/編集/削除/一覧/戻る)
  - 部屋 / キャラ(Actor)/ アイテム / スキル / イベントのCRUD、ゲーム設定
    (タイトル・開始部屋・最大パーティ人数・フラグ・変数の追加/削除)
  - 入力はJSONソースツリーを直接編集するため、未モデル化フィールドを壊さずに保存
  - ID重複・文字種の即時チェック(4.3)、保存時の横断検証(セクション5:
    exits遷移先 / on_enter_event / on_use_event / goto / next_event / join_party /
    参照アイテム・スキル・ドロップ / start_room の存在チェックを警告表示)
- **ゲーム実行エンジン**(`src/engine.c`, `src/engine.h` / 仕様セクション4)
  - 部屋の説明・出口表示・移動、`locked_by_flag` による施錠扉の判定
  - 床アイテムの取得、持ち物メニュー、`effect` / `on_use_event` によるアイテム使用
  - `on_enter_event` 再生、`auto` イベントの `next_event` 連鎖、`choice` イベントの選択
  - `choices[].set_flag` / `set_var` / `join_party` / `goto` アクション
  - 表示条件の評価(`flag:x==bool` / `var:x <op> N`、`&&` / `||` の複合条件を左から順次評価)
  - パーティ加入(`max_party_size` 上限)/ 状況表示

### 未実装(セクション5以降)

- ターン制戦闘システム(現段階ではエンカウント通知のみ。戦闘解決は後続段階)
- セーブ・ロード / `.htgb` コンパイル(XOR難読化)・`.htgb` の実行

## ソース構成

```
src/json.c    src/json.h    自前JSONパーサ/シリアライザ
src/model.c   src/model.h   .htgp データモデル(ロード/セーブ)
src/ui.c      src/ui.h      共通CLI UIヘルパー(メニュー/入力)
src/editor.c  src/editor.h  対話式エディタ(CRUD・設定・保存時検証)
src/engine.c  src/engine.h  ゲーム実行エンジン(探索・イベント・条件評価)
src/main.c                  CLIエントリポイント/コマンドディスパッチ
Makefile
```
