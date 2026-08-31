# PineBuds Pro C++ ブリングアップ マニュアル (Phase 0.5)

対象ファーム: Milestone A (C++ hello-world) + Milestone B (単コア float GEMM 自己検証)。

## 1. 概要とゴール

このファームは次の 3 点を UART 出力で証明する:

1. **C++ ランタイムが生きている** — グローバルオブジェクトのコンストラクタが走る
   (`__libc_init_array` 相当が呼ばれている) ことを `[ctor]` 行で示す
2. **アプリ初期化パスから C++ コードを呼べる** — `hello, C++ from PineBuds (core=0)`
3. **FPU の実数値計算が正しい** — N=32 の float GEMM を決定的ケース
   (A=B=全 1 ⇒ checksum=N³=32768、float32 で厳密表現可) で自己検証し PASS/FAIL を出す

この単コア GEMM が次フェーズ (MPI サブセットアダプタ + 2 コア並列) の**ゴールデン基準**になる:
分散版の正しさは「これと同じ checksum が出るか」で判定する。

## 2. 必要なもの

- ホスト OS: Linux (WSL2 可。ただし Docker Desktop の WSL integration が必要 — §7)
- Docker (OpenPineBuds のビルド環境はコンテナ)
- PineBuds Pro + 充電ケース + USB Type-C ケーブル。ケースは CH342DS チップで
  **USB→デュアル UART** を提供し、左右バッズが `/dev/ttyACM0` / `ttyACM1` として見える
  (プログラマ兼デバッグシリアル。公式 Wiki 確認済み)
- シリアル端末: `picocom` または `screen` (2,000,000 baud が出せること)

## 3. 環境構築

```bash
git clone https://github.com/TamichiRyuto/pine-buds-cluster.git
cd pine-buds-cluster
make test                          # まずホストでカーネルの単体テストが通ることを確認
./scripts/setup-openpinebuds.sh    # external/ に OpenPineBuds を clone + docker チェック
./scripts/install-into-sdk.sh      # カーネル+アプリを apps/main/ へ配置し app_init にフック

cd external/OpenPineBuds
./start_dev.sh    # 開発コンテナ起動 (初回は GCC 取得で 1〜3 分)
                  # プロンプトが root@<id>:/usr/src# になる
./build.sh        # ビルド。エラー時は ./clear.sh か rm -rf out/ してから再実行
```

注意: HANDOVER 記載の `./clean.sh` は存在せず、実体は **`./clear.sh`** (付録 B 参照)。

## 4. 書き込み手順

**WSL2 の場合は先に USB パススルーを設定する。** WSL2 からは USB デバイスが直接
見えないため、Windows 側で usbipd-win を使ってケース (CH342DS) を渡す:

```powershell
# Windows (管理者 PowerShell)
winget install usbipd
usbipd list                        # CH342 のバス ID を確認
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

アタッチ後にポートが出ない場合は `sudo modprobe cdc_acm` を実行する (WSL カーネルは
CDC-ACM ドライバを自動ロードしないことがある。実測 2026-08-30)。
WSL 側で `ls /dev/ttyACM*` に 2 ポート (右=ACM0, 左=ACM1 の想定) が出れば OK。
ポートは root 所有になることがあるが、ビルドコンテナは privileged なので影響しない。
ホスト側で picocom を使う場合は `sudo usermod -aG dialout $USER` 後にシェルを開き直す。

```bash
# 1) 既存ファームのバックアップ (フラッシュで消えるため必須。出力は必ず保全する)
docker compose run --rm builder ./backup.sh
```

backup.sh は**対話式**: 「Please disconnect and reconnect the bud on the right」と
表示されたら右バッズをケースから出して 3 秒待って戻す (bestool がリブートを捕捉して
読み出す)。右が終わると左も同様に促される。完了すると
`firmware-backups/firmware-<timestamp>-{0,1}.bin.bkp` が 2 つできるので、
**必ず別ディスク (例: `/mnt/c/Users/<name>/pinebuds-backup/`) へコピーして保全する**。

```bash

# 2) 書き込み (bestool はコンテナ内にしか無いので docker 経由で実行する)
docker compose run --rm builder ./download.sh
# または手動で (ポートは実機に合わせる):
#   bestool write-image out/open_source/open_source.bin --port /dev/ttyACM0
#   bestool write-image out/open_source/open_source.bin --port /dev/ttyACM1
```

**バッズ操作:** バッズをケースから出し、3 秒待って戻すと再起動し、プログラマがそれを
捕捉する。反応しなければ再挿入してリトライ。別法として、ケース内で背面ボタンを約 5 秒
長押しすると強制再起動する (この SDK の変更点として README に明記あり。なお「ケース内
ボタン押下での DFU 誘発」は無効化済み)。

**注意 (`CHARGER_PLUGINOUT_RESET=0` 適用後の書き込みトリガ):** 本リポジトリのファーム
(`scripts/install-into-sdk.sh` の §12.4 パッチ適用後) はケースからの抜き差しでリセット
しなくなるため、bestool がリブートを捕捉できないことがある。旧ファーム (=1) から新ファ
ーム (=0) への **1 回目の書き込みは従来どおり抜き差しでよい**。**2 回目以降**は上記の
「ケース内で背面ボタンを約 5 秒長押し」でリブートさせること。`__POWERKEY_CTRL_ONOFF_ONLY__`
が未定義のため電源キー UP は `app_shutdown()` に入るが、充電中は PMU が即座に再投入する
ため結果的にリブートになる (詳細は `docs/design-ibrt-transport.md` §12.7 リスク 3)。

**ブリック復旧:** pine64 配布の「Windows based programmer utility」(v1.48、ベンダー製
マニュアル PDF あり) + 工場出荷ファーム (`AC08_20221102.bin`、OTA ブート
`ota_boot_rel_8054309a08.bin`) で復元できる。ケース内には SY8821 管理のリセットボタンも
ある (safety-off からの復帰用)。着手前に `backup.sh` の出力を別ディスクに保全し、復旧
経路を一度確認しておくこと。参照: https://pine64.org/documentation/PineBuds_Pro/Software/

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

### 6b. ゴールデンリファレンス検証 (ホスト)

無改変ベンチ戦略の基準一致検証は `scripts/golden-check.sh` (要 docker) で行う。
`bench/gemm_mpi_omp.cpp` を同一ソースのまま
(1) 本物の OpenMPI + libgomp (ubuntu:22.04 コンテナ) で np=1 / np=2、
(2) 自作アダプタ (adapters/) で逐次 1 ランク、
の両方でビルド実行し、全実行が `checksum=32768.000000` と `PASS` を出すことを確認する。
2 ランクのアダプタ経路は `make test` の pthread ハーネスが担保する。
2026-08-31 実測: 3 経路すべて厳密一致で PASS。

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

OpenPineBuds (HEAD, shallow clone) の調査結果。パスは SDK ルート相対。

### トップレベル構成

| ディレクトリ | 役割 |
|---|---|
| `apps/` | アプリ層。`main/apps.cpp` が中心。`common/` にアプリスレッド/メールボックス |
| `config/` | ボード別設定。有効ボードは `config/open_source/target.mk` |
| `platform/` | `main/` (リセットベクタ + `main()`)、`hal/` (BES2300P HAL)、`cmsis/` |
| `rtos/` | `rtx/` = CMSIS-RTOS v1 RTX (本ボードで使用)。FreeRTOS は不在 |
| `services/` | BT/BLE スタック、audioflinger、IBRT (TWS)、OTA 等 |
| `scripts/` | Kbuild 風ビルド基盤 (`build.mk`, `lib.mk`) と `link/` (リンカスクリプト) |
| `out/open_source/` | ビルド成果物 (git 管理外) |

### ビルド系

- ビルド: `build.sh` → `make -j$(nproc) T=open_source DEBUG=1`
- 成果物: `out/open_source/open_source.elf` / **`open_source.bin` (書き込み対象)** / `.map`
- `download.sh` は `bestool write-image out/open_source/open_source.bin --port /dev/ttyACM<n>` を右→左の順で実行

### アプリのエントリ

起動チェーン: `Boot_Loader` (`platform/main/startup_main.S:35`) → `SystemInit` → newlib `_start` →
`software_init_hook` (`rtos/rtx/TARGET_CORTEX_M/RTX_CM_lib.h:330`) が
**`__libc_init_array` を呼び (static ctor はここで走る)**、`main` を RTOS スレッドとして起動 →
`main()` (`platform/main/main.cpp:167`) → `hal_trace_open` (同 :217、**ここから TRACE が有効**) →
`app_init()` (`apps/main/apps.cpp:1889`)。

フック点: `app_init()` 末尾、`app_sysfreq_req(..., APP_SYSFREQ_32K)` (apps.cpp:2449) の**直前**。
ここなら BT スタック初期化済み・クロックが 32K に落ちる前に計算を実行できる。
`scripts/install-into-sdk.sh` がこの位置に `compute_main()` を挿入する。

注意: static ctor は **RTOS 起動前・trace 有効化前**に走る。ctor 内の TRACE は
UART オープン前のためバッファされるか失われる可能性があり、実機での見え方は要確認。

### RTOS

CMSIS-RTOS v1 RTX (`KERNEL=RTX`; `config/common.mk:822-826`、CPU=m4 のため)。
スレッド生成は `osThreadCreate` (`rtos/rtx/TARGET_CORTEX_M/rt_CMSIS.c:657`)。
使用例: `apps/common/app_thread.c:25-132` (`osThreadDef` + `osThreadCreate`)。

### C++ の現状

- `.cpp` はビルド一級市民 (`scripts/build.mk:260-267`)。`main.cpp` / `apps.cpp` 自体が C++
- フラグ: **`-std=gnu++98 -fno-rtti`** (`Makefile:432`) + 共通の `-fno-exceptions`
  `-fsingle-precision-constant -Wdouble-promotion -Wfloat-conversion` (`Makefile:400-425`)
- `__libc_init_array` は `RTX_CM_lib.h:340` で呼ばれる → **static ctor は標準で動く**
- `-fno-use-cxa-atexit` は未設定。`atexit(__libc_fini_array)` 登録あり

### TRACE / UART

- `TRACE(attr, fmt, ...)` (`platform/hal/hal_trace.h:205`)。**第 1 引数はフォーマット引数の個数**
- ボーレート: `config/open_source/target.mk:370` `TRACE_BAUD_RATE := 2000000`
- 有効化: `main.cpp:217` `hal_trace_open(HAL_TRACE_TRANSPORT_UART0)` 以降
- SDK 付属の観測スクリプト: `uart_log.sh` (minicom, 2 Mbaud)

### リンカ / メモリ

- スクリプト: `scripts/link/best1000.lds.S` (名前は legacy、BES 汎用)
- RAM: `0x20000000` 起点、`RAM_SIZE ≈ 0xC0000` (CP 領域を除く実効)。Flash 4MB (`0x3C000000` cached)
- ヒープ/スタックセクションは各 `0x1000`。SRAM 残量は ビルド後の `out/open_source/open_source.map` で確認する

### ソース追加方法

`apps/main/Makefile:3` が `*.c *.cpp *.S` を wildcard で拾うため、**apps/main/ に
ファイルを置くだけでビルドされる**。新ディレクトリの場合は `apps/Makefile:1` の
`obj-y` に `mydir/` を追加し、そのディレクトリに Kbuild 風 Makefile を置く。

## 9. 付録 B: HANDOVER からの訂正メモ

| HANDOVER の記述 | 実際 (repo HEAD) |
|---|---|
| `./clean.sh` | 存在しない。`./clear.sh` が正 |
| freestanding C++ は `-std=c++17` を想定 | SDK の C++ は **`-std=gnu++98`**。C++11 以降の機能は使えない。本リポジトリの src/ は gnu++98 互換で書き、`make check98` で担保 |
| `__libc_init_array` を呼ぶ起動コードの有無は要調査 | **呼ばれている** (`rtos/rtx/TARGET_CORTEX_M/RTX_CM_lib.h:340`)。ctor スタブ追加は不要見込み |
| `trace(...)` は printf 形式 | `TRACE(attr, fmt, ...)` で第 1 引数は**フォーマット引数の個数** |
| `-fno-use-cxa-atexit` を付ける | SDK は未設定のまま動いている。既定に合わせ、リンクエラーが出た場合のみ対処 |
| 復旧ツール名 `dld_main` | 公式ページの名称は「Windows based programmer utility」(v1.48)。工場ファームは `AC08_20221102.bin` + `ota_boot_rel_8054309a08.bin` |
| UART 2 Mbaud (公式仕様として) | 公式 Wiki に baud rate の記載なし。根拠は SDK の `config/open_source/target.mk:370` (`TRACE_BAUD_RATE := 2000000`) |
| SRAM 992KB | 公式一致。加えて **BT 共有 SRAM 64KB** が別枠で存在 (公式 Wiki) |
| ケースの USB-UART は CH342DS (公式 Wiki) | 手元の実機は **CH347** (VID:PID 1a86:55da, USB-HiSpeed-SERIAL A/B) だった。ハードリビジョン差とみられる。CH347 の UART も CDC-ACM なので手順への影響なし (2026-08-30 実測) |

## 10. 次フェーズへの引き継ぎ (MPI サブセットアダプタ + OpenMP 相当に向けて)

**コード調査で確定した事実** (付録 A 参照):

- エントリ点: `app_init()` 末尾 (apps.cpp:2449 直前) が一発実行フック。常駐タスクにするなら
  `osThreadCreate` (RTX, CMSIS-RTOS v1) または `app_set_threadhandle` でアプリスレッドに登録
- C++ 設定: gnu++98 / -fno-exceptions / -fno-rtti / -fsingle-precision-constant。
  static ctor は標準で動く。カーネルは gnu++98 互換を維持すること (`make check98`)
- trace API: `TRACE(nargs, fmt, ...)` @ 2 Mbaud UART0。`main.cpp:217` 以降有効
- タイミング計測口: `GET_CURRENT_MS()` (`hal_timer.h:93`)。compute_trace.h でラップ済み
- GEMM カーネルは行範囲 `[m0, m1)` 分割対応済み。分割等価性はホストテストで担保済み。
  分散版の合否は本ファームの checksum (32768) との基準一致で判定する

**ビルド実測 (2026-08-30, open_source.bin 904KB)** — `.map` 集計:

| 領域 | サイズ | 使用 | 残量 |
|---|---|---|---|
| FLASH (4MB) | 4,194,304 | 904,544 | 約 3.2MB |
| RAM (メインコア データ) | 696,320 | 357,680 | **約 330KB** |
| RAMCP (CP データ) | 131,040 | 83,636 | 46KB (音声 CP オフロードが使用中) |
| RAMCPX (CP コード) | 98,304 | 66,488 | 31KB (同上) |

GEMM N=32 (12KB) は余裕。float 厳密検証の範囲なら N=128 (192KB) まで搭載可能な計算。
CP を計算に使う場合は音声系の CP 使用分との取り合いになる点に注意。

**実機確認済み (2026-08-30, 右バッズ)**:

- Milestone A + B の全 4 行を UART で確認。**Phase 0.5 の受け入れ条件を達成**
- ctor 内 TRACE は **UART オープン前のため失われる** (実測)。対策として ctor は
  マジック値 (0xC7B0BEEF) を .bss に記録し、compute_main() が後追い報告する方式に変更
- GEMM N=32 の実測: **5〜9 ms** (起動時の周波数状態で変動する模様)。将来の MFLOPS 比較の基準値
- 工場ファームは backup.sh で左右とも 4MB 全量取得し、`/mnt/c/Users/<name>/pinebuds-backup/`
  に md5 検証つきで保全済み

**未確認のまま残る項目**:

- 左バッズの UART 確認 (同一ファームなので同結果の見込み。ttyACM1 で §5 と同手順)
- ブリック復旧経路の実地演習 (バックアップからの書き戻しは未実施。手順は §4)
