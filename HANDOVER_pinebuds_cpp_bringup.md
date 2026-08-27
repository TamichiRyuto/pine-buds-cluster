# Handover — PineBuds Pro で C++ を動かす: Hello World → 単コア float GEMM 自己検証

**Phase 0.5 / 分散GEMM(nano-MPI + OpenMP)の基盤フェーズ**

---

## 0. この文書について

- **読み手:** リポジトリ内の Claude Code（実装担当）。完了後は人間(Ryuto)がマニュアルを読む。
- **あなた(実装担当)の成果物は2つ:**
  1. 実機で動作するファーム（下記 Milestone A + B を満たす）
  2. 人間向けマニュアル（§7b の目次を必須とする）
- **上位ゴール(今回の範囲外):** 左右2バッズを nano-MPI で結び、ノード内は OpenMP 相当の2コア並列で、GEMM および 2D 熱伝導(Red-Black SOR)ベンチを分散実行する。
- **今回のゴール:** 「C++ が BES2300YP 上で走り、UART に**検証可能な数値**を出す」ことを確立する。これが無いと以降の分散作業は一歩も進めない。

> **重要:** この文書のコマンド/パスは調査時点の `pine64/OpenPineBuds` (main) 準拠。**実装前に必ずリポジトリ HEAD の README とスクリプトを読み、差分を取ること。** 相違があればリポジトリを正とし、本書の該当箇所をマニュアル付録に「訂正メモ」として残す。

---

## 1. 対象ハードウェア(確定事実)

| 項目 | 値 |
|---|---|
| SoC | Bestechnic **BES2300YP**（各バッズに1個、独立） |
| CPU | **Dual-core ARM Cortex-M4F @ 最大300MHz** |
| FPU | **単精度HWのみ**（`double`はソフトエミュ＝遅い・禁止） |
| SRAM | 992KB（BTスタック等の常駐分を差し引いた実用値は要実測） |
| Flash | 4MB |
| デバッグ出力 | **UART @ 2,000,000 baud** |
| リポジトリ | `github.com/pine64/OpenPineBuds`（Docker + Make） |

**数値的制約(前フェーズの実測知見):** float32 では残差・誤差にフロアがある。GEMM 自己検証は**整数値で表せる決定的ケース**を使い、精度に依存しない合否判定にすること（§Appendix B 参照）。

---

## 2. スコープ

**IN(今回やる)**
- **Milestone A:** C++ の hello-world を UART に出す（static constructor が走ることの証明を含む）。
- **Milestone B:** 単コア・float の GEMM を実機で走らせ、自己検証結果(PASS/FAIL)を UART に出す。

**OUT(次のハンドオーバー)**
- nano-MPI（IBRT / inter-bud のメッセージ層）
- OpenMP 相当（2nd core のブリングアップと分割）
- 2バッズ協調・ハロー交換・Allreduce

ただし **§6 の前方互換制約**は今回の実装で必ず守ること。ここを外すと次フェーズで書き直しになる。

---

## 3. 環境セットアップ(確定コマンド)

```bash
# ホスト前提: Docker が動くこと。git。USBでクレードルが見えること。
git clone https://github.com/pine64/OpenPineBuds.git
cd OpenPineBuds

# 1) 開発コンテナ起動(初回はGCC取得で1〜3分)。privilegedで動く=書き込みのため。
./start_dev.sh
#   → プロンプトが root@<id>:/usr/src# になる

# 2) ビルド
./build.sh
#   ビルドエラー時は ./clean.sh か out/ を rm -rf してから再実行

# 3) 既存ファームのバックアップ(フラッシュで消えるため必須)
./backup.sh

# 4) 書き込み。/dev/ttyACMX は実機に合わせる(ttyACM0 / ttyACM1 等)
./download.sh   # 実際の引数/ポート指定は repo の download.sh を読んで確認
```

**書き込み時のバッズ操作:** バッズをケースから出し、**3秒待って戻す**と再起動し、プログラマがそれを捕捉する。反応しなければ再挿入してリトライ。（別法: ケース内で背面ボタン長押し〜5秒で強制再起動。ただし「ケース内ボタン押下でのDFU誘発は無効化済み」との記述があるため、**現行READMEで再起動手順を確認すること**。）

**UART観測(hello worldの唯一の可視化手段):**
```bash
# 2Mbaud。picocom でも screen でも可
picocom -b 2000000 /dev/ttyACM0
# or
screen /dev/ttyACM0 2000000
```

**ブリック復旧:** pine64 配布の Windows プログラマユーティリティ(`dld_main`)＋工場出荷ファームで復元可能。**着手前にこの復旧経路を一度確立しておくこと**（backup.sh の出力も保全）。

---

## 4. タスク(順序と受け入れ条件)

### Task 0 — リポジトリ偵察【実装前の必須調査】
コードを書く前に、以下を**発見して文書化**する。憶測で進めない。

- [ ] ビルド系: `Makefile` / `build.sh` が最終的に生成する成果物(バイナリ/hex)のパスと、`download.sh` が実際に焼く対象。
- [ ] **アプリのエントリ**: 電源投入後にユーザーコードが走る箇所（RTOSのタスク生成、`app_init` 相当、`main` 相当）。どのスレッド/どちらのコアで走るか。
- [ ] **RTOS の種別**（BES系は RTX / 独自RTOS が多い）。スレッド生成 API。
- [ ] **C++ の現状**: ビルドに `g++`/C++ 翻訳単位が含まれるか。`libstdc++`/`libsupc++` がリンクされるか。**static constructor(`__libc_init_array`)を呼ぶ起動コードがあるか**。ヒープ(`_sbrk`)と `new`/`delete` の有無。
- [ ] **UART ログAPI の実体**: `TRACE()` / `TR_INFO` 等のマクロがどこで定義され、2Mbaud UART に本当に届くか。ログの初期化タイミング。
- [ ] SRAM/Flash のリンカスクリプト上の空き（`.map` を確認）。

**受け入れ条件:** 上記の回答と、関連ファイルの相対パスを列挙した「リポジトリ構造マップ」を作り、マニュアル付録Aに載せる。

---

### Task A — C++ hello-world を UART に出す
**目的:** C++ の翻訳単位がビルド・リンク・起動・出力まで通ることの証明。

実装要件:
- freestanding C++ を有効化: 最低限 `-fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics`。STL とヒープ確保は使わない（`new` を避け静的バッファ）。
- **static constructor が走る証明**: グローバルオブジェクトを1つ用意し、その**コンストラクタ内で** UART に痕跡を出す。起動ログにその行が出れば `__libc_init_array` が呼ばれている＝C++ランタイムが生きている証拠。
- `app_init` 相当（Task 0 で特定した場所）から `hello()` を呼び、`core id` を出す。

**受け入れ条件(UART @2Mbaud で観測):**
```
[ctor] GlobalProbe constructed        <- static ctor が走った証拠
hello, C++ from PineBuds (core=0)     <- アプリから呼べた証拠
```

失敗の典型: ctor 行が出ない→起動コードが `__libc_init_array` を呼んでいない（自前で呼ぶ or リンカ/crt設定を修正）。リンクエラー `undefined reference to __cxa_*` → `-fno-exceptions -fno-rtti` と、必要なら `__cxa_pure_virtual`/`__cxa_atexit` のスタブを追加。

---

### Task B — 単コア float GEMM 自己検証
**目的:** FPU を使った実数値計算が正しく走り、結果を機械判定できることの確立。分散GEMM の**計算カーネルの原型**でもある。

実装要件:
- freestanding C++。`float` のみ。行列は**静的バッファ**（`static float A[N][N]` 等）。ヒープ不使用。
- サイズ `N` は SRAM に収まる小さめ（**まず N=32**、余裕を見て 48/64 を試す）。1枚 `N*N*4` バイト × 3枚(A,B,C)。
- カーネルは素朴な三重ループでよい（最適化は後）。**純関数**として書く（§6）。
- 自己検証は**精度に依存しない決定的ケース**（§Appendix B）: `A=B=全要素1.0f` ⇒ `C[i][j]=N`、総和 `=N^3`。N=32 なら **32768.0**（float32 で厳密表現可）。
- PASS/FAIL と実測値・期待値・所要 tick(あれば)を UART に出す。

**受け入れ条件(UART):**
```
GEMM float N=32  checksum=32768.000000  expect=32768.000000  PASS
```
（`FAIL` の場合は最初に不一致した (i,j) と値も出す。）

---

## 5. 既知の落とし穴 / リスク

1. **C++ 有効化が最大リスク**。例外・RTTI・ヒープ・static ctor・`__cxa_*` の5点セット。freestanding 前提で潰す。STL は原則持ち込まない。
2. **ブリック**。過度な書き込みで文鎮化し得る。着手前に復旧経路確立＋`backup.sh` 保全。
3. **2Mbaud UART の物理**。配線/レベル/ポート番号(ttyACM)取り違えでログが出ない。ループバックやstock挙動で経路を先に確認。
4. **SRAM 予算**。BTスタック等が常駐。計算専用にオーディオ/ANC/コーデックを削れば大きく空くが、今回はまず既存構成の空きに N=32 を収める。`.map` で残量を確認。
5. **`double` 禁止**。M4F はソフトエミュで激遅。検証も §Appendix B の整数値ケースで精度非依存にする。
6. **どちらのコア/バッズか**。hello/GEMM は片バッズ・プライマリコアのみで可。2ndコア・L/R は次フェーズ。

---

## 6. 前方互換の設計制約(次フェーズを殺さない)

今回の実装は必ず次を満たすこと。守らないと nano-MPI/OpenMP 段で書き直しになる。

- **GEMM は純関数**にする: `void gemm(int M,int N,int K, const float* A, const float* B, float* C)`。グローバル状態・出力副作用をカーネルに持ち込まない（ログは呼び出し側）。
- **データ分割を引数化**: 将来 M 次元(行)をコア間・バッズ間で分割するので、`gemm` は「担当行範囲 `[m0, m1)` だけ計算する」形に一般化できる引数設計にしておく（今回は全域を1コアで呼ぶだけ）。
- **検証は「基準一致」方式**: 分散版の正しさは「単コア版と同じ checksum が出るか」で判定する。今回の単コア GEMM がその**ゴールデン基準**になる。決定的・再現可能に保つ。
- **タイミング計測の口**を用意（tick/カウンタ）。将来 MFLOPS 比較に使う。

---

## 7. 成果物の仕様

### 7a. ファーム
- Task A + Task B を満たす単一ファーム。起動時に ctor 痕跡 → hello → GEMM 自己検証 を順に UART 出力し、`PASS`/`FAIL` で終える。
- 追加した/変更したファイルは最小限にし、既存のBT/オーディオ動作を壊さない（少なくとも起動は完走すること）。

### 7b. 人間向けマニュアル(必須目次)
`docs/` 以下に Markdown で作成。**この目次を必須とする:**
1. 概要とゴール（このファームが何を証明するか）
2. 必要なもの（ホストOS、Docker、ケーブル、シリアル端末）
3. 環境構築（clone → start_dev → build の実行ログ例つき）
4. 書き込み手順（backup → download、バッズ操作、失敗時リトライ）
5. UART の見方（2Mbaud接続、期待される出力の全文、各行の意味）
6. 期待結果と合否（PASS例・FAIL例・数値の読み方）
7. トラブルシュート（ctorが出ない/リンクエラー/ログが出ない/ブリック復旧）
8. 付録A: リポジトリ構造マップ（Task 0 の成果）
9. 付録B: 本書からの訂正メモ（HEAD との差分）
10. 次フェーズへの引き継ぎ（nano-MPI + OpenMP に向けて確定した事実：エントリ点、C++設定、SRAM残量、trace API）

---

## 8. Definition of Done

- [ ] Task 0 の調査結果が付録Aに文書化されている
- [ ] `[ctor] ...` 行が UART に出る（static ctor 実行の証明）
- [ ] `hello, C++ from PineBuds (core=0)` が出る
- [ ] `GEMM float N=32 checksum=32768.000000 expect=32768.000000 PASS` が出る
- [ ] 既存ファーム機能を壊さず起動が完走する
- [ ] ブリック復旧手順を実際に確認済み（または未確認なら明記）
- [ ] マニュアル(§7b の全10章)が揃っている
- [ ] `gemm` が §6 の純関数・分割可能・基準一致検証の要件を満たす

---

## Appendix A — freestanding C++ 起動チェックリスト(Task A補助)
- [ ] コンパイル: `-std=c++17 -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -ffreestanding`
- [ ] リンク: C++の未定義参照が出たら `__cxa_pure_virtual`, `__cxa_atexit`(no-op), `operator new/delete`(未使用なら禁止orトラップ) のスタブを用意
- [ ] 起動: リセットハンドラ/crt が `__libc_init_array()` を呼ぶか確認。無ければ `app_init` 冒頭で明示的に呼ぶ
- [ ] ヒープ: `new` を使わないなら `_sbrk` 不要。使うならリンカの heap 領域と `_sbrk` 実装を確認

## Appendix B — GEMM 自己検証カーネル(参考スケルトン)
> `trace(...)` はファームの UART TRACE マクロに置換すること。`gemm` は §6 準拠の純関数。

```cpp
// freestanding C++: no STL, no heap, float only.
static constexpr int N = 32;
static float A[N][N], B[N][N], C[N][N];

// 担当行 [m0,m1) だけ計算(将来のコア/ノード分割用)。今回は m0=0,m1=N。
void gemm(int m0, int m1, int n, int k,
          const float* a, const float* b, float* c) {
    for (int i = m0; i < m1; ++i)
        for (int j = 0; j < n; ++j) {
            float acc = 0.0f;
            for (int p = 0; p < k; ++p)
                acc += a[i*k + p] * b[p*n + j];
            c[i*n + j] = acc;
        }
}

// 精度非依存の決定的自己検証: A=B=1 => C[i][j]=N, sum=N^3
bool gemm_selftest() {
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) { A[i][j] = 1.0f; B[i][j] = 1.0f; }

    gemm(0, N, N, N, &A[0][0], &B[0][0], &C[0][0]);

    float sum = 0.0f;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) sum += C[i][j];

    const float expect = (float)N * (float)N * (float)N; // N^3 = 32768 for N=32
    bool pass = (sum == expect);
    trace("GEMM float N=%d  checksum=%f  expect=%f  %s\r\n",
          N, sum, expect, pass ? "PASS" : "FAIL");
    return pass;
}
```

## Appendix C — 参考リンク
- Repo: https://github.com/pine64/OpenPineBuds
- README(ビルド/フラッシュ): https://github.com/pine64/OpenPineBuds/blob/main/README.md
- DeepWiki 構造/フラッシュ: https://deepwiki.com/pine64/OpenPineBuds
- PineBuds Pro Wiki(ハード): https://wiki.pine64.org/wiki/PineBuds_Pro
- Software/工場ファーム/Windowsプログラマ: https://pine64.org/documentation/PineBuds_Pro/Software/
