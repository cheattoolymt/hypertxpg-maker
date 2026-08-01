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

## サンプルゲーム

`samples/kiri_no_mori.htgp` に、日本語のサンプル作品「霧の森の少女」を同梱しています。
本格的なノベル調の導入・会話・戦闘を一通り体験できます。

- **ノベルパート**: 開始部屋の `on_enter_event` から `auto` イベントを `next_event`
  で連鎖させ、記憶を失った旅人のプロローグが自動再生されます。
- **会話・分岐**: 泉のほとりで少女リラと出会い、選択肢によって仲間加入
  (`join_party`)・伝説を聞く・単独行を選ぶといった分岐が発生します
  (フラグ `set_flag` / 変数 `set_var` / 表示条件 `condition` を使用)。
- **鍵付き扉**: リラから受け取る「祠の鍵」で `locked_by_flag` の扉が開きます。
- **戦闘**: 道中でスライム・霧狼とのランダムエンカウント、最奥の祠では
  必中エンカウント(`chance: 100`)のボス「闇の主」とのターン制戦闘。
  プレイヤーと仲間はスキル(魔法/物理)・装備ボーナスを持ちます。

```sh
make
./htg run samples/kiri_no_mori.htgp      # そのまま実行
./htg compile samples/kiri_no_mori.htgp  # samples/kiri_no_mori.htgb を生成
./htg run samples/kiri_no_mori.htgb      # 難読化版を実行
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

### 実装済み(セクション5:戦闘システム / 仕様セクション3)

- **ターン制コマンド戦闘**(`src/battle.c`, `src/battle.h`)
  - パーティ全員(`party[0]` == 主人公 + 加入した仲間)対 敵(複数可)
  - 毎ラウンド、生存中の味方+敵を実効 `spd`(base+装備bonus)降順で行動順に整列
  - 味方コマンド:`攻撃` / `スキル・魔法` / `アイテム` / `防御` / `逃亡`
    - 逃亡はパーティ全体行動(成功で離脱)、防御はそのラウンドの被ダメを半減
  - スキルは MP を消費し `accuracy` で命中判定(通常攻撃は常に命中)
  - ダメージ = `max(1, 攻撃側実効ATK or skill.power − 防御側実効DEF)`
  - 敵AIは生存中の味方を対象にランダム攻撃(所持スキルを確率使用)
  - 勝利で敵の `drop` を入手、パーティ全滅で敗北(ゲームオーバー)
  - HP/MP はランタイム値として保持され、戦闘後も持ち越し
- **装備システムの実行時反映**(`src/runtime.c`, `src/runtime.h`)
  - 実効ステータス = base + 装備中アイテムの `equip.xxx_bonus` 合計(仕様2.3a)
  - `htg run` の「装備」メニューから武器/防具/装飾スロットへ装備変更が可能

### 実装済み(セクション6:セーブ / ロード)

- **セーブ・ロード**(`src/save.c`, `src/save.h`)
  - プレイ中の進行状態(現在地・持ち物・フラグ・変数・パーティのHP/MP・装備)を
    プロジェクトとは別の人間可読なJSONファイルへ保存/復元(仕様セクション6)
  - `htg run` のコマンドメニューから「セーブ」「ロード」を実行

### 実装済み(セクション7 ステップ7:`.htgb` コンパイル)

- **`.htgb` コンパイル / 実行**(`src/compile.c`, `src/compile.h`)
  - `htg compile <project.htgp> [出力.htgb]` で `.htgp` を `.htgb` へ変換
  - `.htgp` と同一のデータ構造(JSON バイト列)を保持したまま、
    リピートキー XOR による簡易難読化を施す(鍵はビルド定数)
  - 先頭 5 バイトのヘッダ(マジック `HTGB` + フォーマットバージョン)+
    難読化ペイロードの単純な形式
  - `htg run <project.htgb>` は拡張子で自動判別し、復号 → JSON パース →
    `.htgp` と同一のゲームエンジンで実行(仕様セクション1)

## ソース構成

```
src/json.c    src/json.h    自前JSONパーサ/シリアライザ
src/model.c   src/model.h   .htgp データモデル(ロード/セーブ)
src/ui.c      src/ui.h      共通CLI UIヘルパー(メニュー/入力)
src/editor.c  src/editor.h  対話式エディタ(CRUD・設定・保存時検証)
src/engine.c  src/engine.h  ゲーム実行エンジン(探索・イベント・条件評価・装備・セーブ/ロード)
src/runtime.c src/runtime.h 共有ランタイム状態(パーティ・実効ステータス計算)
src/battle.c  src/battle.h  ターン制戦闘システム(仕様セクション3)
src/save.c    src/save.h    セーブ/ロード(仕様セクション6)
src/compile.c src/compile.h .htgp -> .htgb コンパイル/難読化・.htgb 実行(仕様セクション7)
src/main.c                  CLIエントリポイント/コマンドディスパッチ
Makefile
```
