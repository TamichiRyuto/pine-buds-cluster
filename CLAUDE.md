# pine-buds-cluster

PineBuds Pro (Bestechnic BES2300YP, dual-core Cortex-M4F) を計算クラスタ化する実験リポジトリ。
現フェーズ (Phase 0.5) のゴールは `HANDOVER_pinebuds_cpp_bringup.md` に定義:
C++ hello-world と単コア float GEMM 自己検証を実機 UART (2 Mbaud) で確認する。
上位ゴールは左右バッズを nano-MPI で結び、ノード内 2 コア並列で GEMM / Red-Black SOR を分散実行すること。

## リポジトリ構成

- `src/` — ターゲット非依存の計算カーネル (freestanding C++)。ホストでも実機でもそのままコンパイルできる純関数のみを置く
- `tests/` — ホスト側ユニットテスト (g++ でビルドして実行)。TDD はここで回す
- `firmware/` — OpenPineBuds への統合層 (パッチ / 追加ファイル / 統合手順)
- `scripts/` — 環境構築・ビルド・書き込みの補助スクリプト
- `docs/` — 人間向けマニュアル (HANDOVER §7b の目次に従う)
- `external/` — OpenPineBuds のクローン先 (git 管理外)

## ビルドとテスト

```bash
make test          # ホスト側ユニットテストをビルドして実行 (最優先の検証手段)
```

実機ビルドは OpenPineBuds の Docker 環境で行う (`scripts/` と `docs/` を参照)。
この WSL では Docker Desktop の WSL integration が無効なことがある。docker が見つからない場合は
Windows 側 Docker Desktop の Settings → Resources → WSL integration を有効にするよう人間に依頼する。

## ターゲット制約 (src/ のコードは必ず守る)

- **freestanding C++, dialect は gnu++98**: SDK が `-std=gnu++98` でビルドするため
  C++11 以降の機能は使えない (`make check98` で担保)。例外・RTTI・STL・ヒープ
  (`new`/`malloc`) 禁止。静的バッファのみ
- **`double` 禁止**: M4F は単精度 FPU のみ。リテラルは `1.0f`、`float` のみ使用
- **カーネルは純関数**: グローバル状態・I/O をカーネルに持ち込まない。ログは呼び出し側
- **データ分割を引数化**: 行範囲 `[m0, m1)` を引数に取る形にし、将来のコア間/バッズ間分割に備える
- **検証は精度非依存**: 整数値で厳密表現できる決定的ケースで判定 (例: A=B=1 ⇒ checksum=N^3)

## 開発スタイル

### Tidy First

構造の変更 (リネーム・抽出・移動・整形) と振る舞いの変更 (機能追加・バグ修正) を
**同じコミットに混ぜない**。構造を先に整えてから振る舞いを変える。

### TDD (t-wada スタイル)

1. これから書くテストの **テストリスト** を作る (TODO として tests/ 内コメント等に残してよい)
2. リストから 1 つ選び、**失敗するテストを先に書く** (Red) — 失敗を必ず実行して確認する
3. **最小の実装** で通す (Green) — 仮実装 (ベタ書き) → 三角測量 → 明白な実装、の順に段階を踏んでよい
4. **リファクタリング** (Refactor) — テストが通る状態を保ったまま重複を除去
5. 1 サイクルごとに小さくコミットする

テストの実行を省略しない。「通るはず」で先に進まない。実機で確認できない項目
(UART 出力・実機タイミング) はホストテストの対象外とし、docs/ の手動確認手順に落とす。

## コミュニケーション

ユーザーとのやり取りは日本語。コード内コメント・ドキュメントの言語は既存ファイルに合わせる
(docs/ は日本語、src/ のコメントは英語)。

## Commit rules (English, mandatory)

Write all commit messages in English, following Conventional Commits:

```
<type>(<scope>): <summary in imperative mood, <= 72 chars>

<body: what & why, wrapped at 72 cols. Optional for trivial changes.>
```

- **type**: `feat` | `fix` | `test` | `refactor` | `tidy` | `docs` | `build` | `chore`
  - `tidy` = structure-only change (Tidy First). Never mix with behavior changes
  - `test` = adding/adjusting tests only (the Red step may be committed together
    with its Green step as one `feat`/`fix`; committing test-first separately is also fine)
- **scope** (optional): `gemm`, `firmware`, `docs`, `scripts`, ...
- Summary: imperative mood ("add", not "added"/"adds"), no trailing period
- One logical change per commit. Keep commits small (one TDD cycle ≈ one commit)
- Examples:
  - `feat(gemm): compute row range [m0, m1) only`
  - `tidy(gemm): extract checksum helper`
  - `docs: add flashing procedure with recovery steps`
