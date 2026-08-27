# PineBuds Pro C++ ブリングアップ マニュアル (Phase 0.5)

対象ファーム: Milestone A (C++ hello-world) + Milestone B (単コア float GEMM 自己検証)。

## 1. 概要とゴール

このファームは次の 3 点を UART 出力で証明する:

1. **C++ ランタイムが生きている** — グローバルオブジェクトのコンストラクタが走る
   (`__libc_init_array` 相当が呼ばれている) ことを `[ctor]` 行で示す
2. **アプリ初期化パスから C++ コードを呼べる** — `hello, C++ from PineBuds (core=0)`
3. **FPU の実数値計算が正しい** — N=32 の float GEMM を決定的ケース
   (A=B=全 1 ⇒ checksum=N³=32768、float32 で厳密表現可) で自己検証し PASS/FAIL を出す

この単コア GEMM が次フェーズ (nano-MPI + 2 コア並列) の**ゴールデン基準**になる:
分散版の正しさは「これと同じ checksum が出るか」で判定する。

## 2. 必要なもの

- ホスト OS: Linux (WSL2 可。ただし Docker Desktop の WSL integration が必要 — §7)
- Docker (OpenPineBuds のビルド環境はコンテナ)
- PineBuds Pro + 充電ケース + USB ケーブル (ケースが USB シリアルのプログラマを兼ねる)
- シリアル端末: `picocom` または `screen` (2,000,000 baud が出せること)

## 3. 環境構築

```bash
git clone https://github.com/TamichiRyuto/pine-buds-cluster.git
cd pine-buds-cluster
make test                          # まずホストでカーネルの単体テストが通ることを確認
./scripts/setup-openpinebuds.sh    # external/ に OpenPineBuds を clone + docker チェック

cd external/OpenPineBuds
./start_dev.sh    # 開発コンテナ起動 (初回は GCC 取得で 1〜3 分)
                  # プロンプトが root@<id>:/usr/src# になる
./build.sh        # ビルド。エラー時は ./clear.sh か rm -rf out/ してから再実行
```

注意: HANDOVER 記載の `./clean.sh` は存在せず、実体は **`./clear.sh`** (付録 B 参照)。

## 4. 書き込み手順

```bash
# 1) 既存ファームのバックアップ (フラッシュで消えるため必須。出力は必ず保全する)
./backup.sh

# 2) 書き込み
./download.sh
# または手動で (ポートは実機に合わせる):
#   bestool write-image out/open_source/open_source.bin --port /dev/ttyACM0
#   bestool write-image out/open_source/open_source.bin --port /dev/ttyACM1
```

**バッズ操作:** バッズをケースから出し、3 秒待って戻すと再起動し、プログラマがそれを
捕捉する。反応しなければ再挿入してリトライ。別法として、ケース内で背面ボタンを約 5 秒
長押しすると強制再起動する (この SDK の変更点として README に明記あり。なお「ケース内
ボタン押下での DFU 誘発」は無効化済み)。

**ブリック復旧:** pine64 配布の Windows プログラマユーティリティ (`dld_main`) + 工場出荷
ファームで復元できる。着手前に `backup.sh` の出力を別ディスクに保全し、復旧経路を一度
確認しておくこと。参照: https://pine64.org/documentation/PineBuds_Pro/Software/

## 5. UART の見方

```bash
picocom -b 2000000 /dev/ttyACM0
# or
screen /dev/ttyACM0 2000000
```

起動ログの中に、以下の 3 行 (+計測 1 行) が順に出る:

```
[ctor] GlobalProbe constructed
hello, C++ from PineBuds (core=0)
GEMM float N=32  checksum=32768.000000  expect=32768.000000  PASS
GEMM elapsed=<n> ms
```

| 行 | 意味 |
|---|---|
| `[ctor] ...` | static constructor が走った = C++ ランタイム初期化済みの証拠 |
| `hello, ...` | アプリ初期化パスから C++ 関数を呼べた証拠 |
| `GEMM ... PASS` | FPU 演算が正しい証拠。checksum は N³ の厳密一致 |
| `GEMM elapsed` | 将来の MFLOPS 比較用の計測口 |

## 6. 期待結果と合否

- **PASS 例**: `checksum=32768.000000 expect=32768.000000 PASS`。
  32768 = 32³。全要素 1 の行列積は C[i][j]=32 で整数値のみを通るため、
  float32 で丸め誤差ゼロ。**厳密一致以外は許容しない**。
- **FAIL 例**: `... FAIL` に続き `GEMM first mismatch at (i,j) value=v` が出る。
  最初に不一致した要素の位置と実測値から、メモリ破壊 (特定行だけ壊れる) か
  FPU 設定 (全要素おかしい) かの切り分けを始める。

## 7. トラブルシュート

- **`[ctor]` 行が出ない**: 起動コードが `__libc_init_array` を呼んでいない。
  ハンドオーバー Appendix A のチェックリストに従い、呼び出しを確認・追加する。
- **リンクエラー `undefined reference to __cxa_*`**: `-fno-exceptions -fno-rtti` が
  効いているか確認。残るなら `__cxa_pure_virtual` / `__cxa_atexit` の no-op スタブを足す。
- **ログが 1 行も出ない**: ボーレート (2,000,000) とポート (ttyACM0/1) を再確認。
  まず stock ファームでログが見えるかで UART 経路自体を切り分ける。
- **WSL2 で docker が見つからない**: Docker Desktop → Settings → Resources →
  WSL integration で該当ディストロを有効化してシェルを開き直す。
- **ブリック**: §4 の復旧経路。慌てて連続書き込みしない。

## 8. 付録 A: リポジトリ構造マップ (Task 0 の成果)

(調査結果を反映予定)

## 9. 付録 B: HANDOVER からの訂正メモ

| HANDOVER の記述 | 実際 (repo HEAD) |
|---|---|
| `./clean.sh` | 存在しない。`./clear.sh` が正 |

## 10. 次フェーズへの引き継ぎ

(実機確認後に確定事実を記載: エントリ点、C++ 設定、SRAM 残量、trace API)
