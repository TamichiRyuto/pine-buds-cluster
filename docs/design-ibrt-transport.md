# MPI アダプタ IBRT トランスポート設計

対象: `adapters/mpi/` を実機 2 バッズ (左右) に載せるためのトランスポート層設計。

> **実装済み注記 (2026-08-31)**: 本設計は実装済み。実装時の確定差分は以下のとおりで、
> 本文の該当箇所より優先する。
> 1. `transport.send` / `mpi_frag_send` は **送信元 `src` を明示引数に取る**
>    (ホストでは 1 プロセス内で 2 ランクが frag 状態を共有するため、固定 self_rank では
>    送信元を表せない。deliver に dest を明示させたのと同じ理屈)
> 2. `mpi_frag_init(const mpi_frag_port*)` は self_rank を取らない (1. により不要)
> 3. **cmdcode は `0x8201` の 1 本のみ**。DATA/ACK はフレームヘッダの kind バイトで
>    判別済みのため 0x8202 は不要。M-T1 のプローブは kind=3 (PROBE) / 4 (PROBE_ECHO) を
>    同じ cmdcode に載せ、glue の RX ハンドラで frag に渡さず折り返す
> 4. M-T1/T2/T3 は別ファーム構成ではなく **1 回の起動で順に実行**する
> 実装: `adapters/mpi/mpi_frag.{h,cpp}`, `firmware/pinebuds_compute/mpi_ibrt_glue.{h,cpp}`

SDK の記述はすべて `external/OpenPineBuds` 内の実ファイル行を根拠にする。
`services/ibrt_core/` と `services/ibrt_ui/` は **`Makefile` / `inc` / `lib` しか無く、`src` が存在しない**
(プリビルト静的ライブラリ)。ヘッダから読めない事実は、実際にリンクされる
`services/ibrt_core/lib/libtws_ibrt_enhanced_stack_anc_RTX.a` を逆アセンブルして確定した。
逆アセンブル由来の事実は「**[逆アセ]**」と明記する。

---

## 1. 背景 — 埋めるべき穴

現状の `MPI_Send` は宛先に関係なくプロセス内の静的キューに積む
(`adapters/mpi/mpi_core.cpp:206`, `:214`)。ホストでは 2 ランクが同一プロセスなので成立するが、
実機では左右バッズの RAM が物理的に別なので **送信が相手に永久に届かない**。

必要なのは 2 つの穴 (seam) である。

- **TX seam**: `MPI_Send` の宛先が自分でないとき、ローカルキューを一切経由せず
  トランスポートだけを通す
- **RX seam**: IBRT 受信コールバックから呼べる投入口。ローカルキューに積んで待機中の
  `MPI_Recv` を起こす

---

## 2. SDK 調査結果

### 2.1 ビルド構成 (`make T=open_source DEBUG=1` の変数データベース実測)

`make T=open_source DEBUG=1 -pn` の変数データベースを実測した結果:
`IBRT=1` (`config/open_source/target.mk:276`)、`ENHANCED_STACK=1` / `ANC_APP=1` /
`BLE=0` / `KERNEL=RTX`、そして **`OTA_ENABLE=0`** (`target.mk:219`)、
`INTERACTION=0`、`GFPS_ENABLE=0` (`target.mk:208`)、`BISTO_ENABLE=0` (`:194`)、
`DUAL_MIC_RECORDING=0` (`config/common.mk:67`)、
`TILE_DATAPATH_ENABLED=0` (`target.mk:221`)。
リンクされる IBRT ライブラリは `services/ibrt_core/Makefile:11-34` の命名規則より
**`libtws_ibrt_enhanced_stack_anc_RTX.a`**。

### 2.2 ペイロード上限 — **前提と食い違う**

`services/ibrt_core/inc/app_tws_ctrl_thread.h:23-31` の `#if` 連鎖は、上記のとおり
`IBRT_OTA` / `__GMA_OTA_TWS__` / `OTA_ENABLED` / `__DUAL_MIC_RECORDING__` /
(`TILE_DATAPATH` && `GFPS_ENABLED`) がすべて未定義なので、**アプリ側から見た
`APP_TWS_CTRL_BUFFER_MAX_LEN` は 300** になる (`:29`)。

ところが **[逆アセ]** `tws_ctrl_send_cmd` の実体 (`app_tws_ctrl_thread.o` の先頭) は

```
000a  cmp.w  r2, #0x2a0        ; r2 = length を 672 と比較
0022  bhi.w  #0x15a            ; 超過なら 0x15a へ
015a  ldr r1,[pc]; ldr r0,[pc]; bl hal_trace_assert_dump
```

で、**実際の実行時上限は 672 (= `0x2A0`)、超過は assert (即死) であって戻り値ではない**。

乖離の理由も blob 側で裏が取れる。`app_tws_ctrl_thread.o` の DWARF は
コンパイル単位名 `app_tws_ctrl_thread.cpp` / 出力先 `.../out/best2300p_ibrt_anc` を保持しており、
**プリビルト lib は OTA 有効の別ターゲット設定でコンパイルされている**。
その設定では `:23` の条件が真になり `672` が焼き込まれる。
**ヘッダ定数 300 を信用してはいけない。設計上限として 300 と 672 のどちらを採るかは
M-T1 で実測して決める** (§9)。

関連定数 (`services/ibrt_core/inc/app_tws_ibrt_cmd_handler.h`):
`APP_TWS_IBRT_MAX_DATA_SIZE (672)` `:31`、
`APP_TWS_IBRT_CMDHEAD_SIZE` = cmdcode(2)+cmdseq(2) = **4 バイト** `:32-34`、
`app_tws_ibrt_cmd_t { uint16 cmdcode; uint16 cmdseq; uint8 content[672]; }` `:147-152`。

### 2.3 `tws_ctrl_send_cmd` の契約 [逆アセ]

宣言は `services/ibrt_core/inc/app_tws_ctrl_thread.h:70`
`int tws_ctrl_send_cmd(uint32_t cmd_code, uint8_t *p_buff, uint16_t length);`。
再構成した実体:

```c
if (length > 672)                    hal_trace_assert_dump(...);   /* 即死 */
if (!app_tws_ibrt_tws_link_connected() || !btif_besaud_is_connected()) {
    hal_trace_printf("tws cmd send failed, tws link missing cmd_code:%04x");
    return 0;                        /* ← 成功時と同じ 0 */
}
p = tws_ctrl_mailbox_heap_malloc(length);
memcpy(p, p_buff, length);           /* 呼び出し側バッファは複製される */
osMutexWait(tws_ctrl_mutex_id, osWaitForever);
mail = osMailAlloc(ctx->tws_ctl_mailbox, 0);
if (!mail) { /* 15 件ダンプ後 */ hal_trace_assert_dump(...); }      /* 即死 */
mail->{evt=cmd_code, system_time=hal_sys_timer_get(), cmd_buff={p,length}};
rc = osMailPut(...); osMutexRelease(...); return rc;
```

設計上の帰結は 4 つ。

1. **バッファはコピーされる** — 呼び出し直後に呼び出し側バッファを再利用してよい
2. **戻り値でリンク断を検出できない** — リンク断も成功も `0` を返す。
   自前で `app_tws_ibrt_tws_link_connected()` を見るか、エンドツーエンド ACK が要る
3. **メールボックス枯渇は assert = 端末リセット** — エラー戻り値ではない。
   `hal_trace_assert_dump` は `NORETURN` 宣言 (`platform/hal/hal_trace.h:468` +
   `platform/hal/plat_types.h:134`) なので戻ってこない。
   `tws_ctrl_mailbox_heap_malloc` 側も確保失敗時に同じ assert を踏む
   (逆アセ上、`memcpy` の宛先は NULL 検査されていない)
4. リソース量 [逆アセ]: メール深さ **15 件**
   (`.bss.os_mailQ_q_tws_ctl_mailbox` = 0x4c = 4+15 word、
   `.bss.os_mailQ_m_tws_ctl_mailbox` = 0x174 = 3+15×6 word、item = `TWS_MSG_BLOCK` 24B。
   `tws_ctrl_send_cmd` 内のダンプループが `movs r4,#0xf` = 15 回で独立に裏付け)、
   ペイロード用ヒープ **0xd20 = 3360 バイト**
   (`tws_ctrl_thread_init` が `multi_heap_register(pool, 0xd20)`)。
   **この 3360B と 15 件は SDK 自身の TWS 通信と共有** である

### 2.4 コマンド登録とディスパッチ [逆アセ]

`app_ibrt_find_cmd_table_index(uint16_t cmdcode, app_tws_cmd_instance_t **tbl)`:

```
0002  ubfx r2, r0, #8, #4          ; idx = (cmdcode >> 8) & 0xF
000c  ldr.w r3, [r3, r2, lsl #2]   ; handler = tws_cmd_handler[idx]
0020  cbz r3, #0x5a                ; NULL なら assert
002a  blx r3                       ; handler(&tbl, &size) でテーブル取得
0040  add.w r1, r1, #0x20          ; sizeof(app_tws_cmd_instance_t) = 32
0060  bl hal_trace_assert_dump     ; 見つからなければ assert
```

つまり **テーブル選択は cmdcode の第 2 ニブルによる直接添字**であり、
`enum CMD_ID_T { TWS_CMD_IBRT=0, TWS_CMD_CUSTOMER, TWS_CMD_OTA, TWS_CMD_IBRT_OTA }`
(`app_tws_ibrt_cmd_handler.h:47-54`) と
`APP_IBRT_CUSTOM_CMD_PREFIX 0x0100` / `OTA 0x0200` / `OTA_TWS 0x0300`
(`services/app_ibrt/inc/app_ibrt_custom_cmd.h:22-24`) に一致する。

**未登録スロット・未知の cmdcode は assert で即死する** (ログして無視、ではない)。
登録済みエントリの `cmdhandler` が NULL の場合だけログして捨てる
(`app_ibrt_cmd_rx_handler` 内の別分岐)。

登録は `services/bt_app/besmain.cpp:421-429`。スロット 0 と 1 は無条件に登録されるが、
スロット 3 は `#if defined(IBRT_OTA) || defined(__GMA_OTA_TWS__) || defined(BISTO_ENABLED)`、
スロット 2 は `#ifdef __INTERACTION__` で囲われている。

**本ビルドではそのいずれも未定義 (§2.1) なので、スロット 2 (`TWS_CMD_OTA`, cmdcode
`0x82xx`) とスロット 3 (`0x83xx`) は空きである。**
登録関数 `app_ibrt_set_cmdhandle(enum CMD_ID_T, TWS_CMD_HANDLER_T)` は
`app_tws_ibrt_cmd_handler.h:156` で公開されており、実行時に差し替えられる。

参考: `TWS_CMD_CUSTOMER` (`0x81xx`) は `0x8101`〜`0x8104` が使用済みで、
`0x8103` は `TEST3` と `SHARE_FASTPAIR_INFO` が二重定義されている
(`services/app_ibrt/inc/app_ibrt_customif_cmd.h:25`, `:29`)。空きは `0x8106`〜`0x81FF`。

テーブル要素 `app_tws_cmd_instance_t` は `app_tws_ibrt_cmd_handler.h:128-138`
(8 フィールド / packed / 32 バイト)。RX ハンドラ型は
`void (*)(uint16_t rsp_seq, uint8_t *p_buff, uint16_t length)` (`:123`)。
実例は `services/app_ibrt/src/app_ibrt_customif_cmd.cpp:76-118` のテーブルと
`:120-124` のゲッタ。

### 2.5 RX ハンドラの実行スレッド [逆アセ] — **前提と食い違う**

`app_ibrt_data_receive_handler` は受信キューから 1 件取り出し、688 バイトのスタック
バッファに載せて `app_ibrt_cmd_rx_handler(buf, len)` を呼ぶ。
`app_ibrt_cmd_rx_handler` は cmdcode で表を引き、
`cmdhandler(cmdseq, p_data+4, length-4)` を呼ぶ。

その `app_ibrt_data_receive_handler()` の呼び出し元は **`besmain()` のメインループ**
(`services/bt_app/besmain.cpp:481`) であり、`besmain()` は `BesbtThread` の本体
(`:489`)、生成は `osThreadDef(BesbtThread, (osPriorityAboveNormal), 1, BESBT_STACK_SIZE,
"bes_bt_main")` (`:165`) + `BesbtInit()` (`:493-498`)。

⇒ **我々の `cmdhandler` は BT スタックスレッド `BesbtThread` (osPriorityAboveNormal) 上で
走る。ISR ではないが、ここをブロックすると BT スタック全体が止まる。**
同じループの 1 行上 (`:480`) の `app_ibrt_data_send_handler()` が実際の送信ポンプなので、
**このスレッドを止めると送信も止まる**。計算スレッドの優先度設計に直結する (§6)。

### 2.6 線上のトランスポート実体

TX ポンプ `app_ibrt_data_send_handler()` (`app_tws_ibrt_cmd_handler.h:158`、
呼び出しは `services/bt_app/besmain.cpp:480`) が流し込む先は **BESAUD** という
BES 独自の TWS 制御プロファイルで、**L2CAP チャネル上**に載っている
(`services/bt_profiles_enhanced/inc/besaud.h:37` の `uint32 l2cap_handle`、
`:105` の `#define BESAUDC_MAX_MTU L2CAP_MTU`)。
BESAUD 自身のパケットプールは **5 個** (`besaud.h:25` `BESAUD_PACKET_COUNT (5)`)、
バッファは TX `672*4` / RX `672` (`app_tws_ibrt_cmd_handler.h:36-37`)。

⇒ **リンクが張れている限り L2CAP/ACL の ARQ により順序保証・無損失。**
リンクが切れれば in-flight は失われる。この性質が §5 の「再送しない」判断の根拠になる。
なお BESAUD の下位実装 (`services/bt_profiles_enhanced/lib/*.a`) もソース無しで、
キューイングの詳細は未検証。

### 2.7 ロール / リンク状態 / RTOS / 時刻

| 事項 | API | 根拠 |
|---|---|---|
| ロール定数 | `IBRT_MASTER 0` / `IBRT_SLAVE 1` / `IBRT_UNKNOW 0xff` | `services/ibrt_core/inc/app_tws_ibrt.h:88-91` |
| ロール取得 | `uint8_t app_tws_ibrt_role_get_callback(const void*)` | `app_tws_ibrt.h:329` |
| 同 (ラッパ, ソース可視) | `app_tws_is_master_mode()` / `app_tws_is_slave_mode()` | `services/app_tws/src/app_tws_if.cpp:594-604` |
| TWS リンク判定 | `bool app_tws_ibrt_tws_link_connected(void)` | `app_tws_ibrt.h:297` |
| リンク確立イベント | `HCI_DBG_IBRT_CONNECTED_EVT_SUBCODE` → `app_tws_if_ibrt_connected_handler()` | `services/app_ibrt/src/app_ibrt_customif_ui.cpp:60-62` |
| ロールスワップ | `HCI_DBG_IBRT_SWITCH_COMPLETE_EVT_SUBCODE` | `app_ibrt_customif_ui.cpp:68-84` |
| RTOS | CMSIS-RTOS v1 (RTX v1.01) | `include/rtos/rtx/cmsis_os.h:1-9` |
| スレッド生成の実例 | `osThreadDef(app_thread, osPriorityHigh, 1, 1024*3, "app_thread")` | `apps/common/app_thread.c:25`, 生成 `:132` |
| 優先度 enum | `osPriorityIdle -3` … `osPriorityRealtime +3` | `cmsis_os.h:164-171` |
| Mutex / Semaphore | `osMutexCreate/Wait/Release`, `osSemaphoreCreate/Wait/Release` | `cmsis_os.h:530-559`, `:587-617` |
| 別スレッドからの Release | 可 (ISR からも可と明記) | `cmsis_os.h:52-65` |
| 時刻 | `GET_CURRENT_MS()` = ticks/16 (16 kHz) | `platform/hal/hal_timer.h:93`, `:29`, `:56` |
| ラップ | 32bit ハードカウンタ、約 **3.1 日**。差分演算必須 | `platform/hal/hal_timer.c:129-146` |
| app_init 末尾 | `compute_main(); app_sysfreq_req(..., APP_SYSFREQ_32K);` | `apps/main/apps.cpp:2452-2453` |
| app_init の呼び元 | `main()` (main スレッド) | `platform/main/main.cpp:305` |

`app_init` は `main()` 上で同期実行される。ここでブロックしても `app_thread`
(`apps/common/app_thread.c:25`) と `BesbtThread` は既に別スレッドとして走っているので
BT/キー処理は止まらないが、(a) 32K へのクロック降格が起きず消費電力が上がる、
(b) 電源キー長押し → 再起動を捌く `main()` のシグナル待ちループ
(`platform/main/main.cpp:311-317`) に到達しない。
**現行の GEMM (5〜9 ms) は許容範囲だが、リンク待ちを含む MPI 実行を app_init 末尾で
同期実行するのは不可。専用スレッドに移す。**

---

## 3. 設計 — seam API

方針: `adapters/mpi/` は freestanding gnu++98 / ヒープ・STL・例外・`double` 演算なし /
静的バッファのみ / 関数ポインタ seam のみ、を維持する。
トランスポート未装着時の挙動は 1 バイトも変えない (既存ホストテストは無改変で通る)。

### 3.1 `adapters/mpi/mpi_adapter.h` への追加

```c
/* メッセージ単位の送信 seam。装着されると dest != self の MPI_Send は
   ローカルキューを一切経由せず、この send のみを通る。 */
typedef struct mpi_adapter_transport {
    int (*send)(int dest, int tag, const void *buf, int byte_len);
} mpi_adapter_transport;

void mpi_adapter_set_transport(const mpi_adapter_transport *transport);

/* 受信投入口。トランスポート側 (実機では BesbtThread) から呼ばれる。
   dest を明示引数で受けるのが要点: 呼び出しスレッドは計算スレッドではないので
   port->self_rank() を信用できない。ロックは本関数が自分で取る。 */
int mpi_adapter_deliver(int source, int dest, int tag,
                        const void *buf, int byte_len);

/* MPI_Wtime の時刻源差し替え。未設定ならホストの clock() のまま。 */
typedef double (*mpi_adapter_wtime_fn)(void);
void mpi_adapter_set_wtime(mpi_adapter_wtime_fn fn);
```

`MPI_Send` の変更は 1 箇所だけ (`mpi_core.cpp:200` の長さ検査の直後に挿入):

```c
if (g_transport_installed && dest != current_rank()) {
    return g_transport.send(dest, tag, buf, byte_len);   /* ローカル enqueue しない */
}
```

自分宛て (`dest == current_rank()`) は従来どおりローカルキュー。
`mpi_adapter_deliver` は `enqueue_message()` + `wake()` のみで、ブロックしない。

### 3.2 `adapters/mpi/mpi_frag.{h,cpp}` (新規・ターゲット非依存)

断片化は **ファームウェア glue ではなくアダプタ側の独立モジュール** に置く。
そうしないとホストテストで断片化を Red にできない。

```c
#define MPI_FRAG_HDR_BYTES     12
#define MPI_FRAG_PAYLOAD_BYTES 256
#define MPI_FRAG_FRAME_BYTES   (MPI_FRAG_HDR_BYTES + MPI_FRAG_PAYLOAD_BYTES)  /* 268 */
#define MPI_FRAG_MAX_PEERS     2
#define MPI_FRAG_KIND_DATA     0x01
#define MPI_FRAG_KIND_ACK      0x02

typedef struct mpi_frag_port {
    int  (*emit)(const void *frame, int frame_len);   /* 1 フレーム送出 */
    int  (*deliver)(int source, int dest, int tag,
                    const void *buf, int byte_len);   /* 再構成完了 */
    int  (*acquire_credit)(void);   /* 0=取得, !=0=タイムアウト */
    void (*release_credit)(void);   /* ACK 受信時 */
} mpi_frag_port;

void mpi_frag_init(const mpi_frag_port *port, int self_rank);
int  mpi_frag_send(int dest, int tag, const void *buf, int byte_len);
int  mpi_frag_on_frame(const void *frame, int frame_len);
void mpi_frag_counters(unsigned *tx, unsigned *rx, unsigned *err);
```

`mpi_frag_send` を `mpi_adapter_transport.send` に、
`mpi_frag_on_frame` を IBRT の `cmdhandler` に、それぞれ繋ぐ。

---

## 4. 断片化プロトコル

両端は同一ファームの同一 little-endian Cortex-M4 なので、
packed 構造体の `memcpy` シリアライズで十分 (エンディアン変換不要)。

| offset | size | field | 意味 |
|---|---|---|---|
| 0 | 1 | `kind` | `0x01`=DATA / `0x02`=ACK |
| 1 | 1 | `src` | 送信元 rank |
| 2 | 1 | `dst` | 宛先 rank |
| 3 | 1 | `msg_id` | 送信元ごとの通し番号 (0-255 巡回) |
| 4 | 1 | `frag_idx` | 0 起点の断片番号 |
| 5 | 1 | `frag_cnt` | 総断片数 (1..2) |
| 6 | 2 | `total_len` | メッセージ全体のバイト長 |
| 8 | 4 | `tag` | MPI タグ (int32) |

計 **12 バイト**。`total_len` は 2 境界、`tag` は 4 境界に自然に載る。
ACK は同じ 12 バイトで `kind=ACK`、`src`/`dst` を反転、`msg_id`/`frag_idx` を反響、
残りゼロ、ペイロードなし。
gnu++98 に `static_assert` が無いので
`typedef char chk[(sizeof(mpi_frag_hdr)==12)?1:-1];` でサイズを固定する。

線上のフレーム全体は SDK の 4 バイトヘッダを足して
`4 + 12 + 256 = 272` バイト。ヘッダ由来の保守側上限 300 に収まる (§2.2)。

**再構成状態は「送信元ごとに 1 メッセージ in-flight」に限定する。** 根拠:
`MPI_Isend` は eager で `MPI_Send` に落ちる (`mpi_core.cpp:320`) ため、
1 ランク = 1 計算スレッドである限り、送信は必ず 1 メッセージずつ直列化される。
halo 交換が Irecv×2 + Isend×2 を並べても、実際に線に出るのは常に 1 メッセージ。
`MPI_ADAPTER_MAX_PAYLOAD_BYTES` = 512 なので断片は最大 2。
別 `msg_id` の断片が再構成中に届いたらエラー計上 + `MPI_ERR_INTERN` を返し、
黙って上書きはしない。3 ランク以上に広げる場合は送信元ごとにバッファを増やす。

---

## 5. フロー制御の決定

**採用: (a) クレジット方式。自前 ACK cmdcode によるウィンドウ W (既定 2) 制。**

根拠 — 「キューサイズで押さえ込む」(c) は成立しないことを数値で示せる。
`MPI_ADAPTER_MAX_REQUESTS` = 8 個の Isend が同時に立ち、各 512B = 2 断片なら
16 断片 × 268B ≈ 4.3 KB。これは §2.3 で実測した ctrl ペイロードヒープ
**3360 バイト** を超え、超過は **エラーではなく assert = 端末リセット**。
したがって構成による上限だけでは不十分で、実際の背圧が要る。

(b) SDK の rsp 経路 (`tws_ctrl_send_rsp` + `timeout_ms` + `app_tws_rsp_handle`) は不採用。
理由は `app_ibrt_waiting_cmd_rsp()` (`app_tws_ibrt_cmd_handler.h:163`) が示すとおり
**「rsp 待ち」がシステム全体で 1 つのグローバル状態**であり、SDK 自身の TWS 同期と
競合する。さらに rsp タイムアウト時に TWS リンクを強制切断する経路がある。
我々の高頻度な計算トラフィックをそこに載せるのは危険。

採用案の内容:

- cmdcode を 2 つ使う: `MPI_IBRT_CMD_DATA` と `MPI_IBRT_CMD_ACK`
- `mpi_frag_send` は DATA 断片ごとに `acquire_credit()` → `emit()`。
  クレジットは W 個の RTX セマフォトークン
- 受信側 `mpi_frag_on_frame` は DATA を受けたら直ちに ACK を返す。
  ACK 受信で `release_credit()`
- **in-flight は最大 W × 268 = 536 バイト / W 断片**。ヒープ 3360 B・メール 15 件・
  BESAUD パケットプール 5 個 (§2.6) のいずれに対しても十分な余裕を残す
  (どれも SDK 自身のトラフィックと共有するため)
- `acquire_credit()` が `MPI_IBRT_CREDIT_TIMEOUT_MS` (既定 2000) で取れなければ
  **`MPI_ERR_OTHER` を返す**。ハングも黙殺もしない
- 再送はしない。§2.6 のとおり L2CAP/ACL が ARQ で順序保証・無損失なので、
  届かない = リンク断であり再送では回復しない。
  ACK は「フロー制御」と「リンク断検出」の二役
  (§2.3 のとおり `tws_ctrl_send_cmd` の戻り値ではリンク断を検出できないため必須)

将来の高速化余地: W を上げる / ACK を N 断片ごとにまとめる / メッセージ単位 ACK に
粗くする。いずれも定数 1 つの変更で済む形にしておく。

---

## 6. ファームウェア統合

新規ファイル `firmware/pinebuds_compute/mpi_ibrt_glue.{h,cpp}`。

```c
/* mpi_ibrt_glue.h */
extern "C" {
void mpi_ibrt_glue_start(void);   /* app_init 末尾から呼ぶ。即座に return する */
int  mpi_ibrt_rank(void);
int  mpi_ibrt_size(void);
}
```

`mpi_ibrt_glue.cpp` の責務:

1. **cmdcode テーブル登録** — `TWS_CMD_OTA` スロット (`0x82xx`) が本ビルドで空である
   ことを利用し、`app_ibrt_set_cmdhandle(TWS_CMD_OTA, mpi_ibrt_cmd_table_get)` を呼ぶ。
   cmdcode は `0x8201` (DATA) / `0x8202` (ACK)。
   **SDK ソースへのパッチが 1 行も要らず、SDK 自身のカスタムコマンドを潰さない**のが利点。
   代替案は `app_ibrt_customif_cmd.cpp:76-118` のテーブルに `0x8106`/`0x8107` を
   追記する方式 (install スクリプトでのパッチが必要)
2. **計算スレッド生成** — `osThreadDef(mpi_compute_thread, osPriorityBelowNormal, 1,
   4096, "mpi_compute")` + `osThreadCreate`。
   **優先度は必ず `BesbtThread` の `osPriorityAboveNormal` より下**にする。
   §2.5 のとおり送受信ポンプが `besmain()` ループ上にあるため、
   計算スレッドが CPU を占有すると送信が線に出ない
3. **port 実装** — `osMutexCreate` + `osSemaphoreCreate` で
   `lock/unlock/wait/wake` を実装。`self_rank` は起動時に確定した rank を返す。
   `wake` は `osSemaphoreRelease`、`wait` は unlock→`osSemaphoreWait`→lock
4. **transport 実装** — `emit` は `tws_ctrl_send_cmd(0x8201, frame, frame_len)`。
   §2.3 のとおりバッファは複製されるので静的 1 枚の送信バッファで足りる。
   呼ぶ前に `app_tws_ibrt_tws_link_connected()` を確認し、断なら `MPI_ERR_OTHER`
5. **RX ハンドラ** — `cmdhandler(rsp_seq, p_buff, length)` から `mpi_frag_on_frame`。
   **BesbtThread 上なので絶対にブロックしない。** ミューテックスを短く取り、
   memcpy して `wake` するだけ。ACK 送出も `tws_ctrl_send_cmd` 1 回で戻る。
   CMSIS-RTOS v1 は別スレッドからの mutex / semaphore 操作を明示的に許可している
   (`cmsis_os.h:52-65`) ので安全
6. **`MPI_Init` 相当のブートストラップ** — 計算スレッドの先頭で
   `app_tws_ibrt_tws_link_connected()` を 50 ms 間隔で最大
   `MPI_IBRT_LINK_TIMEOUT_MS` (既定 10000) までポーリング。
   - 接続あり: `app_tws_ibrt_role_get_callback(NULL)` が `IBRT_MASTER` なら rank 0、
     `IBRT_SLAVE` なら rank 1。`mpi_adapter_bootstrap(rank, 2)`
   - タイムアウト: **`mpi_adapter_bootstrap(0, 1)` の縮退モード**を推奨。
     `size==1` なら `MPI_Allreduce` は memcpy (`mpi_core.cpp:278-281`)、
     `MPI_Barrier` は即 return (`:243-245`) なので、片バッズだけでも
     ベンチが正しい checksum を出す。ブリングアップと片側書き込み時に有用
7. **`MPI_Wtime`** — `mpi_adapter_set_wtime` に
   `(double)(uint32_t)(GET_CURRENT_MS() - base) * 0.001` を渡す。
   基準時刻を `MPI_Init` で採り、必ず**差分**を取ることで約 3.1 日のラップを回避する

`scripts/install-into-sdk.sh` の差分: コピー対象に `mpi_ibrt_glue.{h,cpp}`、
`adapters/mpi/{mpi.h,mpi_adapter.h,mpi_frag.h,mpi_core.cpp,mpi_frag.cpp}`、
`adapters/omp/*`、`bench/gemm_mpi_omp.cpp` を追加する
(`apps/main/Makefile:3` が `*.cpp` を wildcard で拾う)。
`ccflags-y` に `-DGEMM_BENCH_NO_MAIN` を追加してベンチの `main()` を無効化する
(ベンチのソースは無改変のまま。`tests/test_gemm_bench.cpp` と同じ手口)。
`apps/main/Makefile:88-92` が `IBRT=1` のとき `-Iservices/ibrt_core/inc` 等を
既に足しているので include パスの追加は不要。
`apps.cpp` のフックは現行のまま `compute_main()` を呼び、その中で
`mpi_ibrt_glue_start()` を呼んで即 return する形にする (app_init を止めない)。

---

## 7. メモリ予算

| 項目 | サイズ | 備考 |
|---|---|---|
| **新規** 再構成バッファ | 2 × (512 + 16) = 1,056 B | 送信元ごと 1 メッセージ |
| **新規** 送信フレーム staging | 272 B | `tws_ctrl_send_cmd` が複製するので 1 枚 |
| **新規** ACK staging | 16 B | |
| **新規** glue 状態 (mutex/sem id, credit, msg_id, counters) | 約 64 B | |
| **新規** 計算スレッドスタック | 4,096 B | `app_thread` の 3,072 B を上回る |
| **新規 小計** | **約 5.5 KB** | |
| 既存 アダプタキュー 8 × 528 | 4,224 B | `mpi_core.cpp:19-28` |
| 既存 リクエスト表 8 × 28 | 224 B | `mpi_core.cpp:103-113` |
| 既存 Allreduce scratch | 512 B | `mpi_core.cpp:298` |
| ベンチ行列 3 × 32×32×4 | 12,288 B | `bench/gemm_mpi_omp.cpp:24-26` |
| **合計** | **約 22.7 KB** | うち新規は 5.5 KB |

メインコア RAM 残量は約 330 KB (`docs/manual.md` §10 の `.map` 実測) なので余裕。
**SDK 側のリソース (ctrl ヒープ 3,360 B / メール 15 件) は増やせない**ので、
W=2 で in-flight を 536 B に抑える設計 (§5) がここでも効く。

---

## 8. ホストテストリスト (t-wada スタイル / Red から始める)

`tests/test_mpi_adapter.cpp` に追加。フェイク wire は
`emit` を受けて相手側の `mpi_adapter_deliver` を呼ぶだけのテスト専用ハーネス。

- [ ] **T1 wire 接続あり (threaded)**: pthread port + transport 装着。
      rank0→rank1 の Send/Recv が成立し、**wire の tx カウンタ > 0** であること。
      カウンタ検査が「ローカル近道をしていない」ことの証拠になる
- [ ] **T2 wire 切断**: 逐次モード (port なし) で `send` が常に失敗する transport を装着。
      (a) `MPI_Send` が `MPI_SUCCESS` **以外**を返す、
      (b) 続く `MPI_Recv` が `MPI_ERR_OTHER` を返す (= ローカルに積まれていない)。
      逐次モードなら Recv がブロックしないので、待ち時間を持たずに判定できる
- [ ] **T3 断片化**: 512 バイト (128 float) を送る。wire が記録した各フレーム長が
      すべて `MPI_FRAG_FRAME_BYTES` 以下で、フレーム数が 2、
      受信側でバイト単位に完全一致すること
- [ ] **T4 単一 in-flight の強制**: 再構成中に別 `msg_id` の断片を注入すると
      エラーが計上され (`mpi_frag_counters` の err が増える)、
      進行中のメッセージが壊れないこと
- [ ] **T5 フロー制御**: `acquire_credit` が常に失敗するポートを与えると
      `MPI_Send` が `MPI_ERR_OTHER` を返す。ハングも黙殺もしない
- [ ] **T6 回帰**: transport を装着しない既存 13 テストが 1 つも変わらず通る
      (`make test` と `make check98` の両方)

---

## 9. 実機マイルストーン

前提: **左右両方に同じファームを書く** (`docs/manual.md` §4 の `download.sh` は
右→左の順に両方へ書く)。どちらが rank 0 になるかは TWS のペアリングが決めるので、
**UART は両方 (`/dev/ttyACM0` と `ttyACM1`) を開き、`role=` 行で識別する**。

### M-T1 — カスタムコマンド往復と上限実測

`0x8201`/`0x8202` の生の往復だけを行うモード。期待出力 (rank 0 側):

```
[mpi-t1] link=1 role=MASTER rank=0
[mpi-t1] probe len=4 ok rtt=3 ms
[mpi-t1] probe len=64 ok rtt=3 ms
[mpi-t1] probe len=256 ok rtt=4 ms
[mpi-t1] probe len=284 ok rtt=4 ms
[mpi-t1] probe len=296 ok rtt=4 ms
[mpi-t1] probe len=328 TIMEOUT
[mpi-t1] max_payload=296
[mpi-t1] rtt n=100 min=3 avg=4 max=12 ms
```

**安全上の必須事項**: §2.2 のとおり `length > 672` は送信側で assert = 即リセットするので、
掃引は下から上へ行い **672 を絶対に超えない**。測りたいのは
「ヘッダ由来の 300 が効くのか、blob の 672 まで通るのか」であり、
300 付近と 672 付近の 2 段階で確認する。判明した値で
`MPI_FRAG_PAYLOAD_BYTES` を確定する。両方向で実施する。

### M-T2 — MPI over IBRT スモーク

```
[mpi] init rank=0 size=2 role=MASTER link_wait=850 ms
[mpi] barrier ok
[mpi] recv from=1 tag=7 val=5.000000
[mpi] frames tx=3 rx=3 err=0
[mpi] finalize
```

rank 1 側は `[mpi] init rank=1 size=2 role=SLAVE ...` と `[mpi] send ok` を出す。

### M-T3 — ベンチ本番 (`bench/gemm_mpi_omp.cpp` 無改変)

```
[mpi] init rank=0 size=2 role=MASTER
GEMM-MPI N=32  checksum=32768.000000  expect=32768.000000  PASS
GEMM-MPI elapsed=<n> ms  frames tx=<n> rx=<n> err=0
```

合否は **checksum の 32768 厳密一致** (`docs/manual.md` §6 と同じ基準)。
単コア版の 5〜9 ms と比較して、通信オーバヘッドの実測値を得る。
`size=1` に縮退した場合も同じ PASS 行が出るので、
必ず `size=2` を確認してから合格とすること。

---

## 10. 未解決リスク

1. **ペイロード実効上限が確定していない。** ヘッダは 300、リンクされる blob の
   実行時チェックは 672 (§2.2)。受信側は `app_ibrt_data_receive_handler` が
   688 バイトのスタックに載せ替えており 672 まで許容するように見えるが、
   その途中の besaud RX バッファ (`IBRT_BESAUD_RX_BUFF_SIZE 672`,
   `app_tws_ibrt_cmd_handler.h:36`) の実挙動はソースが無く未検証。
   M-T1 で実測するまで `MPI_FRAG_PAYLOAD_BYTES = 256` の保守値を使う
2. **`tws_ctrl_send_cmd` の戻り値でリンク断を検出できない** (§2.3)。
   ACK で担保する設計にしたが、ACK 自体も同じ経路で落ちる。
   最終的な信頼性は「クレジットタイムアウト → `MPI_ERR_OTHER`」に依存する
3. **ctrl メールボックス枯渇とペイロードヒープ枯渇はいずれも assert = 端末リセット。**
   しかも SDK 自身の TWS トラフィックと共有しているため、我々が W=2 を守っても
   音楽再生等と重なれば枯渇しうる。M-T3 は BT 非接続 (モバイル未接続) 状態で行い、
   通話・再生と同時に走らせる評価は次フェーズに送る
4. **ロールスワップで rank が入れ替わる。** `HCI_DBG_IBRT_SWITCH_COMPLETE_EVT_SUBCODE`
   (`app_ibrt_customif_ui.cpp:68-84`) は存在するが、SDK 側ハンドラが
   `app_tws_if.cpp` 内で完結しており、我々がフックする公式な口を確認できていない。
   当面は `MPI_Init` で rank をラッチし、`MPI_Finalize` で再取得して
   変化していたら FAIL を出す (検出のみ、対処なし)
5. **`TWS_CMD_OTA` スロットの流用が将来のビルド設定で壊れる。**
   `OTA_ENABLE=1` や `INTERACTION=1` にした瞬間に衝突する。
   ビルド時に `#if defined(IBRT_OTA) || defined(__INTERACTION__)` で
   `#error` を出して気付ける形にする
6. **未知 cmdcode は受信側で assert する** (§2.4)。左右のファームバージョンが
   食い違うと即リセットになる。フレームヘッダに 1 バイトのプロトコル版番号を
   足すことは検討したが、`kind` の上位ビットで代用できるため今回は入れない。
   **左右は必ず同時に同じバイナリを書くこと**を手順に明記する
7. **besaud の受信キューへ積む側 (`btif_besaud_*`) はクローズドで未検証。**
   我々の `cmdhandler` の実行文脈は §2.5 で確定しているので設計への影響はない
8. **`MPI_Wtime` の `double` はソフト float になる** (M4F は単精度 FPU のみ)。
   計測点は 1 回の実行に 2 回だけなので実害は無い見込み。M-T3 で有意なら
   内部 API を `float` 版に切り替える

---

## 11. 実機初回投入の乖離と修正設計 (2026-08-31)

左右両バッズの UART 実測で 3 件の欠陥が確定した。本節が **§6 / §9 / §10 の該当箇所より優先する**。
新規逆アセンブルの対象は `services/ibrt_core/lib/libtws_ibrt_enhanced_stack_anc_RTX.a` と
`services/ibrt_ui/lib/libtws_ibrt_enhanced_stack_RTX.a` (ibrt_ui の Makefile は `_anc` を付けない:
`services/ibrt_ui/Makefile:11-27`)。構造体オフセットは同 `.o` の DWARF で確定した。

### 11.1 事実確認

#### (1) 全 `tws_ctrl_send_cmd(0x8201)` が捨てられ、`link_wait=0 ms` になった

**[逆アセ]** 3 アクセサはいずれも `.bss.g_ibrt_ctrl` の単純フィールド読みだった。DWARF より
`ibrt_ctrl_t` は byte_size=280、`init_done`=+0 / `nv_role`=+1 / `current_role`=+2 /
`is_ibrt_search_ui`=+0xa2 / `tws_conhandle`=+0x58 (ヘッダ `services/ibrt_core/inc/app_tws_ibrt.h:193-195`,
`:210`, `:232` と一致)。

```
app_tws_ibrt_role_get_callback:    ldrb r0,[r3,#2]      ; = current_role
app_tws_ibrt_nv_role_get_callback: ldrb r0,[r3,#1]      ; = nv_role
app_tws_ibrt_tws_link_connected:   ldrh.w r0,[r3,#0x58]; subs r0,#0xFFFF; it ne; movne r0,#1
                                   ; = (tws_conhandle != 0xFFFF) — besaud を一切見ていない
app_tws_ibrt_get_bt_ctrl_ctx:      ldr r0,[pc]          ; = &g_ibrt_ctrl (公開ポインタ)
```

`g_ibrt_ctrl` は **`.bss` = リセット直後ゼロ**。`0 != 0xFFFF` なので
`app_tws_ibrt_tws_link_connected()` は **初期化前に無条件で TRUE** を返す。0xFFFF を書くのは
**[逆アセ]** `app_tws_ibrt_init()` (`strh.w r3,[r4,#0x54/#0x58/#0x5c]` r3=0xFFFF、
`strb r3,[r4,#2]` で current_role=0xff)、そして関数末尾の `movs r3,#1; strb r3,[r4,#0]` =
**`init_done = 1`**。

順序が決定的である。`app_ibrt_init()` (`apps/main/apps.cpp:1845-1866`、`:1849` で
`app_tws_ibrt_init()`) は **BesbtThread へ非同期投入される**
(`apps/main/apps.cpp:2148-2149`)。一方 `compute_main()` フックは main スレッドの
`app_init` 末尾 (`apps/main/apps.cpp:2452`)。⇒ **計算スレッドが SDK の IBRT 初期化を
追い越しうる。実測 `link_wait=0 ms` はこれ。**

加えて §2.3 の送信ゲート `app_tws_ibrt_tws_link_connected() && btif_besaud_is_connected()` を
再確認した。glue の `mpi_ibrt_frag_emit` (`firmware/pinebuds_compute/mpi_ibrt_glue.cpp:192-198`) は
前者しか見ていない。`btif_besaud_is_connected` の宣言は `services/bt_if_enhanced/inc/besaud_api.h:28`。

#### (2) 両バッズが `rank=0 role=MASTER` になった

`app_tws_is_master_mode()` は `IBRT_MASTER == app_tws_ibrt_role_get_callback(NULL)`
(`services/app_tws/src/app_tws_if.cpp:594-596`) = **`current_role == 0`**。`IBRT_MASTER` は 0
(`app_tws_ibrt.h:89`) なので、(1) の `.bss` ゼロ状態では **両側とも MASTER** になる。
ログの `current_role = 0xff` は `services/app_ibrt/src/app_ibrt_customif_ui.cpp:236` の trace で、
BES_AUD 接続時 = 我々の読み取りよりずっと後。矛盾ではなく時系列差である。

安定 rank 源の比較:

- **GPIO 左右ストラップ** — `app_tws_is_right_side()` (`services/app_tws/src/app_tws_if.cpp:633`)。
  `apps/main/apps.cpp:1926` の `app_tws_set_side_from_gpio()` で `app_init` 冒頭に確定し、実体は
  P1_4 の抵抗ストラップ読み (`config/open_source/tgt_hardware.c:160-171`)。
  **BT 起動前に確定し、ロールスワップでも不変。**
- `nv_role` — **[逆アセ]** `app_tws_ibrt_start()` の `ldrb r3,[r5,#0]; strb r3,[r4,#1]` が
  `config->nv_role` を書く。値は `services/app_ibrt/src/app_ibrt_nvrecord.cpp:42-59` 由来で、
  未ペアリング時 `IBRT_UNKNOW`。
- `current_role` — スワップで反転し `.bss` 期は 0=MASTER に化ける。**rank 源にしてはならない。**

⇒ **rank は左右ストラップから採る (右=0 / 左=1)。** NV 実測 (`nv_role 00`=右 / `01`=左) とも
一致し、§10 リスク 4 が構造的に消える。

#### (3) ケース内で TWS/besaud が張れず、保たない

原因は 3 つ重なっている。

1. **起動時の FETCH_OUT 注入がスキップされる。** ケース内充電起動では
   `is_charging_poweron = true` (`apps/main/apps.cpp:2034-2038`) となり、
   `apps/main/apps.cpp:2339` / `:2174` の `if (is_charging_poweron == false)` ブロック
   — `app_ibrt_ui_event_entry(IBRT_FETCH_OUT_EVENT)` (`:2352` / `:2186`) と
   `app_ibrt_enter_limited_mode()` — が丸ごと実行されない。
   `POWER_ON_ENTER_TWS_PAIRING_ENABLED` も 0 (`config/common.mk:2623`) なので
   `apps.cpp:1861-1863` の TWS_PAIRING 注入も無い。**誰も TWS を開始しない。**
2. **5 秒ごとに CLOSE_BOX を再注入される。** `Auto_Shutdowm_Timerfun`
   (`apps/main/apps.cpp:1464`、周期 5000 ms `:1460`) の `:1502-1506` が
   充電中かつ `box_state != IBRT_IN_BOX_CLOSED` のとき `IBRT_CLOSE_BOX_EVENT` を注入する。
   **ケースで充電している限り永久に引き戻す。** 実測の `b_sta=IN_BOX_CLOSED,evt=CLOSE_BOX_EVENT`。
3. **充電器イベントも箱イベントを作る。** PMU IRQ → `apps/battery/app_battery.cpp:707-714` →
   デバウンス `:421` → user_cb → `app_ibrt_search_pair_ui.cpp:535` → `:496-513` (PLUGIN 分岐) が
   `box_event` を立て、500 ms タイマ `:73-81` 経由で `app_ibrt_if_event_entry(boxStatus)`。

注入口自体は使える。`app_ibrt_if_event_entry` (`services/app_ibrt/src/app_ibrt_if.cpp:521-535`) は
**BesbtThread へマーシャリングしてから** `app_ibrt_ui_event_entry` (`services/ibrt_ui/inc/app_ibrt_ui.h:572`)
を呼ぶので、計算スレッドから安全に呼べる。`app_ibrt_if_false_trigger_protect` は
`IBRT_SEARCH_UI` 定義時に無条件 `return false` (`app_ibrt_if.cpp:655-656`、
`config/open_source/target.mk:278` で `IBRT_SEARCH_UI=1`) なので箱状態の整合性チェックで
弾かれることは無く、`IBRT_SKIP_FALSE_TRIGGER_MASK` は不要。

ただし **[逆アセ]** `app_ibrt_ui_event_entry` は先頭で `ldrb.w r3,[r4,#77]; cmp r3,#0; beq ->`
`"ibrt_ui_log:entry return directly due to bonding failed"` を行う。DWARF より `app_ibrt_ui_t` は
byte_size=804、+77 = **`bonding_success`** (`services/ibrt_ui/inc/app_ibrt_ui.h:513`)、
+27 = `box_state` (`:490`)。この 1 を書くのは **[逆アセ]** `app_ibrt_ui_init()` の
`memset(g_ibrt_ui,0,804)` 直後の `str r2,[r3,#76]` (r2=0x00000100、LE で +77 バイトが 1)。
⇒ **`app_ibrt_ui_init()` (`apps/main/apps.cpp:1850`) 完了前に注入したイベントは全て黙殺される。**
`box_state` をアプリ側から直接書く前例は `app_ibrt_search_pair_ui.cpp:669`, `:691`。

`twsif_tws_connected, role 255` は `services/app_tws/src/app_tws_if.cpp:376-379` の trace で、
**張る能力はあり、再ドッキングで維持できないだけ**であることを示す。

#### (4) rank 0 衝突で M-T2 の Barrier が無限ブロックした

(2) の結果 `size=2` で相手不在となり、`MPI_Recv` が `mpi_ibrt_port_wait()`
(`mpi_ibrt_glue.cpp:164-171`) の 100 ms タイムアウトを回りつづけた。
**rank 決定の失敗を検出する仕組みが glue に無い**ことが本質。

### 11.2 修正設計

#### 11.2.1 準備完了述語を 2 段に分ける

`app_tws_ibrt_tws_link_connected()` 単独判定を全廃する。`ibrt_ctrl_t` / `app_ibrt_ui_t` は
ヘッダで完全公開されており、`app_tws_ibrt_get_bt_ctrl_ctx()` (`app_tws_ibrt.h:295`) と
`app_ibrt_ui_get_ctx()` (`app_ibrt_ui.h:570`) でポインタを得られる。

```c
/* stage 1: SDK の app_ibrt_init() を通過したか */
static int mpi_ibrt_stack_ready(void) {
    ibrt_ctrl_t   *c = app_tws_ibrt_get_bt_ctrl_ctx();
    app_ibrt_ui_t *u = app_ibrt_ui_get_ctx();
    return c->init_done          /* app_tws_ibrt_init() 末尾     apps.cpp:1849 */
        && c->is_ibrt_search_ui  /* app_tws_ibrt_start(cfg,true)  apps.cpp:1855 */
        && u->bonding_success;   /* app_ibrt_ui_init()            apps.cpp:1850 */
}
/* stage 2: 0x8201 が実際に線に出るか (blob の送信ゲートと同一条件) */
static int mpi_ibrt_cmd_channel_ready(void) {
    return mpi_ibrt_stack_ready()
        && app_tws_ibrt_tws_link_connected()   /* tws_conhandle != 0xFFFF */
        && btif_besaud_is_connected();         /* besaud_api.h:28 */
}
```

`is_ibrt_search_ui` は **[逆アセ]** `app_tws_ibrt_start` の `strb.w r7,[r4,#162]` が
第 2 引数 (`apps.cpp:1855` は `true`) を書く。
`mpi_ibrt_frag_emit` と PROBE 送出前のチェックを `mpi_ibrt_cmd_channel_ready()` に差し替える。

#### 11.2.2 rank は左右ストラップから確定する

`app_tws_is_unknown_side()` (`app_tws_if.h:466`) なら FAIL。そうでなければ
`rank = app_tws_is_right_side() ? 0 : 1`。`app_tws_is_master_mode()` は rank 決定から外し、
`current_role` はログ表示のみに使う。§10 リスク 4 の「Finalize で再取得して FAIL」は削除する。

#### 11.2.3 ケース内 TWS 確立 — 主: イベント注入 / 従: 直接 page

**主 (a)** — 計算スレッド先頭で順に:

1. `mpi_ibrt_stack_ready()` を 50 ms 間隔で `MPI_IBRT_STACK_TIMEOUT_MS` (既定 15000) まで待つ。
2. `c->nv_role == IBRT_UNKNOW` なら **注入せず** FAIL 行を出して縮退する。未ペアリングからの
   TWS 確立は inquiry (`app_ibrt_search_pair_ui.cpp:464-485`) を要し、ケース内では成立しない。
   ⇒ 手順書に「初回は 1 度だけケース外で左右をペアリングさせる」を明記する。
3. `app_ibrt_ui_get_ctx()->box_state = IBRT_OUT_BOX;` を書き、
   `app_ibrt_if_event_entry(IBRT_OPEN_BOX_EVENT)` → 200 ms →
   `app_ibrt_if_event_entry(IBRT_FETCH_OUT_EVENT)`。これは SDK 自身のケース外起動経路
   (`apps/main/apps.cpp:2352`) と同じ入口である。
4. `mpi_ibrt_cmd_channel_ready()` を 50 ms 間隔で `MPI_IBRT_LINK_TIMEOUT_MS`
   (既定 10000 → **20000** に引き上げ) まで待つ。

**従 (b)** — 4 で 8000 ms 経過しても未確立なら、`c->nv_role == IBRT_MASTER` のバッズ**だけ**が
`app_tws_ibrt_create_tws_connection(c->config.tws_connection_timeout)` (`app_tws_ibrt.h:298`、
前例 `services/app_ibrt/src/app_ibrt_auto_test_cmd_handle.cpp:334-336`) を 1 回呼ぶ。
**[逆アセ]** 同関数は `nv_role != 0` なら `nv_slave_delay_timer` を張って戻るだけで page せず
(`ldrb r2,[r5,#1]; cbnz r2,...`)、`nv_role == 0` かつ `tws_conhandle == 0xFFFF` のときだけ
`btif_create_acl_to_slave` に落ちる。**slave 側から呼んでも無意味。** ui SM を経由しないので
11.2.4 のパッチ無しでは次の CLOSE_BOX で切られる — これが (a) を主とする理由。

#### 11.2.4 install スクリプトによる SDK ソースパッチ (`apps/main/apps.cpp` のみ)

`scripts/install-into-sdk.sh` の既存フック機構 (マーカー + `python3` の `str.replace`、`:60-83`)
に 1 つ追加する。マーカーは `pine-buds-cluster in-case TWS hold`。いずれも一意なアンカーの
1 行置換で、`#ifdef` より差分が小さく未使用変数警告も出ない。

```
# 対象 1 (必須): apps.cpp:1502  — 充電中の CLOSE_BOX 再注入を止める
-  if (app_battery_is_charging()) {
+  /* pine-buds-cluster in-case TWS hold */
+  if (0 && app_battery_is_charging()) {

# 対象 2 (必須): apps.cpp:1517 — 5 分自動電源断を止める
-      if (auto_shutdown_cnt == Auto_Shutdowm_TIME / 5) {
+      if (0 && auto_shutdown_cnt == Auto_Shutdowm_TIME / 5) {
```

対象 2 が要るのは、11.2.3 で `box_state = IBRT_OUT_BOX` にすると `apps.cpp:1508` の条件が真になり、
モバイル未接続のまま `Auto_Shutdowm_TIME/5 = 60` tick × 5 s = **300 秒で
`app_bt_power_off_customize()`** が走るため (`services/app_ibrt/inc/app_ibrt_keyboard.h:56`)。

**対象 3 (任意)**: 充電器プラグイン由来の箱イベント (`app_ibrt_search_pair_ui.cpp:496-513`)。
PMU IRQ は `pmu_charger_set_irq_handler(NULL)` (`apps/battery/app_battery.cpp:709`) で一度きり
武装解除されるため、ドッキング済み・充電継続中の再発火は考えにくい。
**まず対象 1+2 だけで再試験し、ログに `box event:4` が出たときに限り追加する。**

#### 11.2.5 ブリングアップガード

どの段でもハングせず必ず 1 行出す。

1. `[mpi] side=%s rank=%d nv_role=%s current_role=%s init_done=%d`
2. stack readiness 失敗 →
   `[mpi] FAIL stack not ready after %u ms (init_done=%d search_ui=%d bonding=%d)` → 縮退
   (`mpi_adapter_bootstrap(0,1)` で M-T3 のみ実行)
3. cmd channel readiness 失敗 →
   `[mpi] FAIL cmd channel down after %u ms (link=%d besaud=%d)` → 縮退
4. **rank ハンドシェイク** — PROBE の byte1 に自 rank を載せ、echo 側は byte1 を自 rank に
   書き換えて返す (RX ハンドラ `mpi_ibrt_glue.cpp:115-123` に 1 行追加)。判定:
   2000 ms 以内に echo 無し → `[mpi] FAIL no peer echo` → 縮退 /
   `peer_rank == self_rank` → `[mpi] FAIL rank collision self=%d peer=%d` → 縮退 /
   相異なる → `[mpi] peer ok rank=%d peer=%d` で M-T1→M-T2→M-T3 へ進む
5. **ウォッチドッグ** — `mpi_ibrt_port_wait()` に連続タイムアウトカウンタを持たせ、
   `MPI_IBRT_STALL_WARN_MS` (既定 5000) 相当を超えたら **1 回だけ**
   `[mpi] FAIL stalled in MPI op >%u ms rank=%d tx=%u rx=%u err=%u` を出す。MPI の API 契約上
   ここから抜けることはできないので **検出と報告のみ**。4 を通っていれば到達しない。

### 11.3 期待 UART 出力 (再試験)

右バッズ:

```
[mpi] side=RIGHT rank=0 nv_role=MASTER current_role=UNKNOW init_done=1
[mpi] box forced OUT_BOX, injecting OPEN_BOX + FETCH_OUT
[mpi] init rank=0 size=2 link_wait=<n> ms besaud=1
[mpi] peer ok rank=0 peer=1
[mpi-t1] probe len=4 ok rtt=<n> ms ... [mpi-t1] max_payload=<n>
[mpi] barrier ok
[mpi] recv from=1 tag=7 val=5.000000
[mpi] frames tx=<n> rx=<n> err=0
GEMM-MPI N=32 rank=0 size=2 checksum=32768.000000 expect=32768.000000 PASS
GEMM-MPI elapsed=<n> ms frames tx=<n> rx=<n> err=0
[mpi] finalize done rank=0
```

左バッズは `side=LEFT rank=1 nv_role=SLAVE` と `[mpi] send ok`。合格条件は 4 つ:
`rank=0` と `rank=1` が**片側ずつ**出る / `size=2` / `checksum=32768.000000` 厳密一致 /
`ibrt_ui_log:tws cmd send failed` が **0 回**。

### 11.4 リスク

1. **`ibrt_ctrl_t` / `app_ibrt_ui_t` を直接読み書きする。** オフセットはヘッダと blob の DWARF で
   一致を確認済み (280 B / 804 B) だが、ライブラリ差し替えで破綻する。
   `typedef char chk[(sizeof(ibrt_ctrl_t)==280 && sizeof(app_ibrt_ui_t)==804)?1:-1];` を置いて
   ビルド時に気付ける形にする。
2. **`box_state = OUT_BOX` の副作用。** LED/音声インジケーション・探索可能状態・消費電力が
   「ケース外」相当になる。M-T3 はモバイル未接続で行う (§10 リスク 3 と同じ前提)。
3. **自動電源断を殺す** (11.2.4 対象 2) ため、このファームを書いたバッズは放置しても電源が
   落ちない。ケース内で充電されるので実害は無いが、**常用しない**ことを手順書に明記する。
4. **初回ペアリングは依然ケース外が必要。** `nv_role == IBRT_UNKNOW` からはケース内で TWS を
   張れない。NV が飛んだら 1 度だけ手動で出し入れする。
5. **`app_ibrt_ui_event_entry` を我々のタイミングで叩くのは SDK の想定外。** マーシャリング先が
   BesbtThread (`app_ibrt_if.cpp:530-531`) なのでスレッド安全性はあるが、SM 内部の遷移は追えない。
   失敗は 11.2.5 の bounded wait で必ず UART に落ちる設計にしてある。
6. **rank と IBRT ロールが一致しなくなる。** rank は左右固定、IBRT master はスワップしうる。
   `tws_ctrl_send_cmd` は master/slave どちらからも使えるので転送上は問題ないが、ログ読解時に
   混同しないこと。

### 11.5 finalize 後のクラッシュループ — RTX のスレッド自己終了バグ (Run 2)

修正版 glue での再試験 (Run 2) で、ガード類 (11.2.1 / 11.2.2 / 11.2.5) は設計どおり動作し
縮退 `size=1` で GEMM-MPI PASS まで到達した。しかし **両バッズ・毎ブートで
`[mpi] finalize done` の直後 (実測で約 2 秒後にダンプが出力される) に MemFault で
リセットし、無限のクラッシュループになる**。本項はその根本原因である。

新規に使った手段は逆アセンブルではなく **`nm out/open_source/open_source.elf` による
シンボル解決**である (ホストの binutils で ELF32 のシンボル表は読める。ARM の逆アセンブルは
クロスツールチェインが要るので行っていない)。この由来の事実は「**[シンボル解決]**」と明記する。

#### (1) 症状

`~/.claude/handover/artifacts/2026-09-01-0000-pinebuds-ibrt-crash/run2-{left,right}-uart.log`
(NUL 混在。`tr -d '\0' < file | tr '\r' '\n'` で読む) の全ブートで同一:

```
[mpi] finalize done rank=0
### EXCEPTION ###
PC =002AC1BE, ExceptionNumber=-12
R4 =2005ABF0, R5 =2005AC34, R6 =2002AA80
MSP=200A9EB8, PSP=2004DC50
XPSR=2100000B                          ; IPSR=0x0B = SVCall 実行中
CFSR =00000082                         ; MMARVALID | DACCVIOL
MMFAR=00000034, BFAR =00000034
FaultInfo : (MemFault)
FaultCause: (Data access violation) (MMFAR valid)
Possible Backtrace:
  2AC370  2AA5C0  2AA06E  2AA342
Current Task    : 0
New Running Task: 255
```

周期は電源投入から約 17.5 秒。内訳は「ブート〜`app_init` 末尾 (約 2.5 秒)」+
「`MPI_IBRT_STACK_TIMEOUT_MS` = 15000 ms の待ち切り」+「GEMM-MPI 約 20 ms」で説明がつく。

#### (2) エビデンスチェーン

**[シンボル解決]** 主要アドレスは以下に落ちる。`stack` 行の値はダンプの Stack 領域に
生で載っている戻り番地 (Thumb ビット付き) で、`Possible Backtrace` の表示は
そこから呼び出し命令側へ丸めた値なので 4〜5 小さい。

| 値 | シンボル | 意味 |
|---|---|---|
| `PC = 0x002AC1BE` | `rt_switch_req + 0x16` | 例外を起こした命令 |
| stack `0x002AC375` | `rt_tsk_delete + 0x5c` | 呼び出し元 |
| stack `0x002AA5C5` | `svcThreadTerminate + 0x14` | 同上 |
| stack `0x002AA071` | `SVC_Handler + 0x14` | 同上 (IPSR=0x0B と整合) |
| stack `0x002AA355` | `osThreadExit + 0x0` | 同上 |
| `R4 = 0x2005ABF0` | `os_idle_TCB` | `p_new` = 次に走る TCB |
| `R5 = 0x2005AC34` | `os_tsk` | スケジューラの現在/次タスク対 |
| `R6 = 0x2002AA80` | `os_thread_def_mpi_compute_thread + 0x10` | **落ちたのが我々のスレッド**である証拠 |

これを RTX のソースに突き合わせると、経路が一意に決まる。

1. glue の `mpi_compute_thread` が `MPI_Finalize()` の後にスレッド関数を **`return` で抜けた**
2. RTX はスレッド生成時に初期スタックの LR スロットへ `osThreadExit` を仕込む
   (`rtos/rtx/TARGET_CORTEX_M/rt_CMSIS.c:568`
   `*((uint32_t *)ptcb->tsk_stack + 13) = (uint32_t)osThreadExit;`)。
   よって return は必ず `osThreadExit()` (`rt_CMSIS.c:724-729`) へ落ち、
   `__svcThreadTerminate(__svcThreadGetId())` → SVC → `svcThreadTerminate()` (`:584-596`) →
   `rt_tsk_delete(ptcb->task_id)` (`:592`) と進む
3. `rt_tsk_delete` の**自己削除**分岐 (`rtos/rtx/TARGET_CORTEX_M/rt_Task.c:227-242`) は
   `os_tsk.run = NULL;` (`:240`) を代入してから `rt_dispatch(NULL);` (`:241`) を呼ぶ
4. `rt_dispatch(NULL)` (`rt_Task.c:124-131`) は `next_TCB = rt_get_first(&os_rdy)` で
   ready 最上位 (この時点では idle しかいない = `R4 = os_idle_TCB`) を取り、
   `rt_switch_req(next_TCB)` を呼ぶ
5. `rt_switch_req` (`rt_Task.c:109-120`) は `__RTX_CPU_STATISTICS__` が有効なとき
   `:114` で **`os_tsk.run->swap_out_time = HWTICKS_TO_MS(rtx_get_hwticks());`** を実行する。
   `os_tsk.run` は 3 で NULL にされている

`OS_TCB` (`include/rtos/rtx/os_tcb.h:32-74`) の `swap_out_time` オフセットは
`cb_type/state/prio/task_id` 4 + ポインタ 4 本 16 + `u16` 4 本 8 + `msg` 4 +
`stack_frame/reserved1/reserved2` 4 + `priv_stack/tsk_stack/stack` 12 + `swap_in_time` 4
= **0x34** (`std_libspace` は `__CC_ARM` 限定 `:57-60` なので GCC ビルドでは無い)。
**`MMFAR = 0x00000034` と厳密に一致する。**

`__RTX_CPU_STATISTICS__` は `config/common.mk:848` (`KERNEL=RTX` 分岐) で `=1` に固定されており、
ビルド設定で外せない。ダンプの `Current Task : 0` は
`rtx_show_current_thread()` の `os_tsk.run ? os_tsk.run->task_id : 0` (`rt_Task.c:496-498`) で、
`os_tsk.run == NULL` を独立に裏付ける。`New Running Task: 255` は
`rt_switch_req` が `:111` で先に書いた `os_tsk.new_tsk` = idle TCB である。

⇒ **BES 版 RTX の潜在バグ。`__RTX_CPU_STATISTICS__` 有効時、スレッドの自己終了は必ず
MemFault になる。** SDK 内蔵スレッド (`BesbtThread` / `app_thread` など) はすべて
無限ループで `return` しないため、SDK 側では顕在化しない。

#### (3) 整合性の確認

- **Run 1 (旧 glue) でクラッシュしなかった理由**: 旧 glue は M-T2 のバリアで無限待ちに陥り、
  スレッド関数が `return` に到達しなかった。11.1(4) と整合する
- **`apps.cpp` の 2 行パッチ (§11.2.4) は無関係**。落ちているのは RTX のスケジューラであり、
  `R6` が示すとおり原因スレッドは `mpi_compute` である。対照ビルドは不要 (容疑晴れ)
- 縮退 (`size=1`) でも通常経路でも `mpi_compute_thread` の末尾は同じなので、
  **11.2.5 のガードで縮退した場合こそ確実に踏む**。Run 2 で毎ブート再現したのはこのため

#### (4) 対策 (決定済み)

**`mpi_compute_thread` は `return` しない。** `MPI_Finalize()` とサマリ行の後、
`for (;;) { osDelay(...); }` で永久にパークする。SDK 慣習「スレッドは終了しない」に合わせる。

RTX カーネル側に `if (os_tsk.run)` のガードを入れるパッチは **不採用**。理由は
SDK パッチ面を最小に保つため (§11.2.4 の方針と同じ) で、かつ我々の側だけで完全に回避できるため。

派生する一般規則として、`firmware/pinebuds_compute/` に今後スレッドを追加する場合も
**RTOS スレッド関数から `return` してはならない**。この制約はホストテストでは検出できないので
(ホストの `mpi_thread_port.h` は pthread で、join して正常終了する)、
実機側の約束事として本節に残す。

実装: `firmware/pinebuds_compute/mpi_ibrt_glue.cpp` の `mpi_compute_thread` 末尾 (適用済み)。
実機での確認は §12.6 の「`### EXCEPTION ###` が 1 度も出ないこと」で行う。

---

## 12. 充電起動で BT スタックが起動しない問題 (Run 2)

Run 2 では **`bes_bt_main` が一度も起動していなかった**。ケース内での電源投入 =
充電起動では SDK が BT 起動経路を丸ごと飛ばすためで、現行の
「ケース内抜き差しで書き込み・実行」手順では **原理的に 2 ランクが成立しない**。
本節はその原因確定と対策の設計である。§11 の修正設計 (11.2.1〜11.2.5) は
**すべてそのまま有効**で、本節はその前段 (SDK が IBRT を初期化するところまで進む) を作る。

> **行番号の基準**: 本節の `apps/main/apps.cpp` の行番号は
> **`scripts/install-into-sdk.sh` 適用前の素の SDK** (`git show HEAD:apps/main/apps.cpp`) に対するもの。
> §11 は compute hook (`int app_init(void)` = 素の `:1889` の直前に 2 行挿入) 適用後の番号なので、
> 素の 1889 行目以降について **§11 の値は本節より +2 大きい** (例: §11.1(3) の `:2352` = 本節の `:2350`)。
> それより前の `:1502` / `:1849` などは両者一致する。
> パッチのアンカーは行ではなく一意な文字列なので、実装上の影響は無い。

### 12.1 事実確認 — 原因は `is_charging_poweron` ではない

実測ログ (両バッズ・全ブート・Run 2) は必ずこの並びになる。

```
Yin BATTERY 1
app_status_indication_set 5
CHARGING!
app_key_init_on_charging
[ctor] GlobalProbe constructed        ← compute_main() は呼ばれている
...
[mpi] FAIL stack not ready after 15000 ms (init_done=0 search_ui=0 bonding=0)
```

`bes_bt_main` / `bt_stack_init_done` / `BesbtInit` / `ibrt_ui_log` の出現回数は
**両バッズとも 0**。BT スタックスレッドが生成されていない。

#### (1) `Yin BATTERY 1` が全てを決めている

`Yin BATTERY %d` は `apps.cpp:2007` の trace で、値は `nRet = app_battery_open()` (`:2006`)。
`1` は `APP_BATTERY_OPEN_MODE_CHARGING` (`apps/battery/app_battery.h:57-60` に
`INVALID (-1)` / `NORMAL (0)` / `CHARGING (1)` / `CHARGING_PWRON (2)`)。

`switch (nRet)` (`apps.cpp:2013`) の `case APP_BATTERY_OPEN_MODE_CHARGING:` (`:2017-2031`) は

```c
2017    case APP_BATTERY_OPEN_MODE_CHARGING:
2018      app_status_indication_set(APP_STATUS_INDICATION_CHARGING);
2019      TRACE(0, "CHARGING!");
2020      app_battery_start();
2022      app_key_open(false);
2023      app_key_init_on_charging();
2024      nRet = 0;
2025  #if defined(BT_USB_AUDIO_DUAL_MODE)
2026        usb_plugin = 1;
2027  #elif defined(BTUSB_AUDIO_MODE)
2028        goto exit;
2029  #endif
2030        goto exit;
```

で、**`goto exit` が二重に置かれていて必ず `exit:` (`:2412`) へ飛ぶ**。
open_source は `BTUSB_AUDIO_MODE ?= 1` (`config/open_source/target.mk:296` →
`config/common.mk:1328-1329` で `-DBTUSB_AUDIO_MODE`) なので、実際に効くのは `:2028` の側。

飛び越される範囲に **`BesbtInit()` (`:2142`)**、`app_wait_stack_ready()` (`:2143`)、
`app_bt_start_custom_function_in_bt_thread(..., app_ibrt_init)` (`:2146-2147`) がすべて入る。
⇒ `BesbtThread` も `app_ibrt_init()` も存在しない。

一方 **`compute_main()` フックは `exit:` より後ろ** (§6 のとおり `:2449` の
`app_sysfreq_req(..., APP_SYSFREQ_32K)` の直前) なので呼ばれる。
「BT のログが皆無なのに GEMM-MPI が `size=1` で PASS する」というログの姿はこれで完全に説明できる。

#### (2) `is_charging_poweron` は今回 1 度も立っていない

`is_charging_poweron` (`apps.cpp:1902`) が `true` になるのは
`case APP_BATTERY_OPEN_MODE_CHARGING_PWRON:` (`:2032-2040`) の `:2035` だけであり、
**そちらは `goto exit` を持たず、BT 起動へ素通しする**。
§11.1(3) が挙げた `:2172` / `:2314` / `:2337` の `is_charging_poweron == false` ガードは、
「BT は上がっているが箱イベントを注入しない」という**別の状態**を指しており、
Run 2 で我々が踏んだのはそれ以前の段である。

#### (3) どちらの case になるかはコンパイル時に決まる

`app_battery_open()` (`apps/battery/app_battery.cpp:486-562`) の該当箇所:

```c
542   if (app_battery_charger_indication_open() == APP_BATTERY_CHARGER_PLUGIN) {
...
552   #if (CHARGER_PLUGINOUT_RESET == 0)
553       nRet = APP_BATTERY_OPEN_MODE_CHARGING_PWRON;
554   #else
555       nRet = APP_BATTERY_OPEN_MODE_CHARGING;
556   #endif
```

`config/open_source/target.mk:386` が `-DCHARGER_PLUGINOUT_RESET=1` を渡している。
**これが単独の根本原因。**

対照が強い。BES 自身の IBRT ターゲットはいずれも **0** である
(`config/best2300p_ibrt/target.mk:345`, `config/best2300p_ibrt_anc/target.mk:348`)。
§2.2 で確定したとおり、**実際にリンクされる IBRT blob は `best2300p_ibrt_anc` 設定で
コンパイルされている**。つまり `0` の側が blob の前提であり、`1` は open_source ターゲット固有の
逸脱である。ヘッダのデフォルトも `0` (`app_battery.cpp:65-67` の `#ifndef` 既定値)。

### 12.2 `CHARGER_PLUGINOUT_RESET=1` のもう 1 つの副作用 — 抜き差しでリセット

同じマクロは実行時にも効く。

```c
/* app_battery.cpp:328-342  status==NORMAL のとき PLUGIN が来た */
329   if (prams.charger == APP_BATTERY_CHARGER_PLUGIN) {
334   #if CHARGER_PLUGINOUT_RESET
335       app_reset();
336   #else
337       app_battery_measure.status = APP_BATTERY_STATUS_CHARGING;
338   #endif

/* app_battery.cpp:364-375  status==CHARGING のとき PLUGOUT が来た */
369   #if CHARGER_PLUGINOUT_RESET
370       TRACE(0, "CHARGING-->RESET");
371       app_reset();
372   #else
373       app_battery_measure.status = APP_BATTERY_STATUS_NORMAL;
374   #endif
```

`app_reset()` (`apps.cpp:609-612`) は `system_reset()` (`platform/main/main.cpp:109-113`) で
main スレッドへ signal `0x8` を送り、`main()` のシグナル待ちループ (`main.cpp:311-331`) を
`sys_case = 2` で抜けさせ、`app_deinit()` (`:337`) → `hal_cmu_sys_reboot()` (`:353`) に至る。

Run 1 の左バッズログがこれを直接示している (ケース外起動 → 再挿入の経路):

```
app_battery_pluginout_debounce_handler PLUGIN
CHARGING-->APP_BATTERY_CHARGER :1        ← app_battery.cpp:328
app_deinit case:0                        ← main.cpp:337、リブート開始
...（この teardown 中に TWS が張れてしまった）
[mpi] init rank=1 size=2 role=SLAVE link_wait=1850 ms
...（約 770 行後にリブート）
```

**ケースへ戻すと必ずリブートする**。「TWS が張れた唯一の実績」は teardown 中の
数秒のレースであって、定常状態ではなかった。これが 12.3 で案 B を却下する主要根拠になる。

### 12.3 案の比較

| 軸 | A: `CHARGER_PLUGINOUT_RESET` を 0 にする | A′: `apps.cpp` の `goto exit` を潰す | B: ケース外起動 → 再挿入運用 |
|---|---|---|---|
| 確実性 | 高。SDK 自身が持つ CHARGING_PWRON 経路にそのまま乗る | 中。SDK が想定しない状態を作る | **不可**。12.2 の `app_reset()` を必ず踏む |
| パッチ面 | `config/open_source/target.mk` の定数 **1 文字** | `apps/main/apps.cpp` に新規 1 箇所 | 0 (ただし目的を達しない) |
| 手順の煩雑さ | 変化なし (ケースに入れるだけ) | 変化なし | 出し入れ + タイミング待ち |
| リスク | 充電と BT の同時稼働 (12.7)。書き込みトリガが変わる (12.7-3) | 二重初期化、PLUGOUT リセットが残る | UART が取れない、定常状態が無い |

#### 案 B を却下する決定的理由

1. **UART がケース経由でしか出ない。** `docs/manual.md` §4 のとおり usbipd で WSL に渡すのは
   ケース (CH342DS) 1 台で、`/dev/ttyACM0` = 右 / `ttyACM1` = 左 の 2 ポートはケースが供給する。
   バッズをケースから出している間は **UART が物理的に繋がらない**。
   「ケース外で BT を上げている時間帯」は観測できず、M-T1〜M-T3 のログが取れない
2. **書き込み (bestool) もケース経由でしかできない** (`docs/manual.md` §4)
3. **再挿入は 12.2 の `app_reset()` を踏む。** リブート後は再び充電起動に戻るので、
   「BT が上がった状態でケース内にいる」という定常状態が作れない
4. §11.2.4 対象 1 の CLOSE_BOX 再注入停止パッチは**効かない**。あれは
   `Auto_Shutdowm_Timerfun` (箱イベント) の話で、`app_reset()` は battery 側の別経路である
5. B を成立させるには結局 `CHARGER_PLUGINOUT_RESET=0` が必要になる。
   ⇒ **A の上位互換にならない**

#### 案 A′ を却下する理由

- SDK が想定していない「`APP_BATTERY_STATUS_CHARGING` のまま BT を上げる」状態を作る。
  `app_battery_start()` が `:2020` と `:2385` で二重に呼ばれ、
  `app_key_init_on_charging()` (`:2023`) と `app_key_init()` (`:2384`) が両方走る
- `app_battery_measure.status` が CHARGING のままなので、以降の抜き差しは
  `app_battery_handle_process_charging` を通り、**12.2 の PLUGOUT リセットがそのまま残る**
- パッチ面が `apps/main/apps.cpp` に 1 箇所増える。A は既存行の定数 1 文字

**⇒ 案 A を採用する。**

### 12.4 採用案 A — `CHARGER_PLUGINOUT_RESET` を 0 にする

```
# 対象: config/open_source/target.mk:384-386
 KBUILD_CPPFLAGS += \
     -DAPP_AUDIO_BUFFER_SIZE=$(AUDIO_BUFFER_SIZE) \
-    -DCHARGER_PLUGINOUT_RESET=1 \
+    -DCHARGER_PLUGINOUT_RESET=0 \
```

`scripts/install-into-sdk.sh` に §11.2.4 と同形のフックを 1 つ追加する。
マーカーは `pine-buds-cluster charging poweron`、対象は `config/open_source/target.mk`、
`-DCHARGER_PLUGINOUT_RESET=1` の出現数が 1 であることを assert してから `str.replace` する。
マクロは `KBUILD_CPPFLAGS +=` でハードに渡されるので、コマンドラインから別値を足すと
再定義になる。**target.mk を書き換える以外に方法は無い。**

#### 変更後にたどる経路 (すべて実ファイルで裏取り済み)

1. `app_battery_open()` `:542` で PLUGIN 検出 → `:553` `APP_BATTERY_OPEN_MODE_CHARGING_PWRON` (=2)
2. `apps.cpp:2032-2040` に入る。`TRACE(0, "CHARGING PWRON!")` (`:2033`)、
   `is_charging_poweron = true` (`:2035`)、`need_check_key = false` (`:2038`)、
   `nRet = 0` (`:2039`)、`break`。**`goto exit` しない**
3. `:2049` `app_key_open(false)` を通り、`:2142` `BesbtInit()` →
   `:2143` `app_wait_stack_ready()` → `:2146-2147` で `app_ibrt_init` を BesbtThread へ投入。
   ⇒ **§11.2.1 の `mpi_ibrt_stack_ready()` が真になりうる前提がここで初めて成立する**
4. `need_check_key == false` なので `:2264` で `pwron_case = APP_POWERON_CASE_NORMAL`
5. `case APP_POWERON_CASE_NORMAL:` (`:2336`) の `:2337` `if (is_charging_poweron == false)` は
   偽なので、SDK は `IBRT_FETCH_OUT_EVENT` を注入しない。
   **これは §11.2.3 が自分で注入する設計なので変更不要。**
   同時に落ちる `startLED_status(1000)` と `once_event_case = 9` のうち、9 のハンドラは
   `case 9: break;` の空処理 (`once_delay_event_Timer_fun` (`apps.cpp:1560`) の `:1621-1622`)
   なので実害なし
6. `:2384-2385` `app_key_init(); app_battery_start();` ⇒ 充電ステートマシンは通常どおり動く
7. 実行時の抜き差しは `app_battery.cpp:337` / `:373` の代入だけになり、**リブートしなくなる**

#### 変わらないもの

- **§11.2.4 の `apps.cpp` パッチ 2 件はそのまま必要**。
  `Auto_Shutdowm_Timerfun` の CLOSE_BOX 再注入 (`:1502`) と 5 分自動電源断 (`:1517`) は
  battery とは別経路で、`CHARGER_PLUGINOUT_RESET` の影響を受けない
- **§11.2.1〜11.2.5 のすべて**。rank は左右ストラップ、readiness は 2 段、
  箱イベント注入、bounded wait とガード — 設計は一切変えない
- `APP_POWERON_CASE_CHARGING` は SDK 内で一度も代入されない
  (`apps.cpp:66` の enum 値で、参照は `:2437` の `#if defined(BTUSB_AUDIO_MODE)` ブロックのみ)。
  ⇒ `app_usbaudio_entry()` 経路に迷い込むことはない

### 12.5 実機手順 (人間の操作列)

1. **(初回のみ、NV が空のとき)** 左右をケース外で 1 度ペアリングさせ、`nv_role` を確定させる
   (§11.4 リスク 4)。以後この手順は不要
2. `scripts/install-into-sdk.sh` を実行する
   (compute hook / §11.2.4 の apps.cpp パッチ / 本節 12.4 の target.mk パッチが入る)
3. ビルドコンテナで `./build.sh`
4. 両バッズをケースに入れ、ケースを USB で接続し、usbipd で WSL に渡す (`docs/manual.md` §4)
5. `./download.sh` で右 → 左の順に書き込む。**必ず左右同時に同じバイナリを書く** (§10 リスク 6)
6. **書き込みトリガ**: 新旧どちらのファームでも「出して 3 秒待って戻す」でよい。
   =0 では取り外し時の `app_reset()` は無効 (12.2) だが、充電起動したバッズは
   ケースから外れると電源ごと落ちるため、再挿入は必ず充電起動のコールドブートになる
   (Run 4 前後で 2 回実測、§12.7 リスク 3 の訂正を参照。
   「背面ボタン長押し」は実機に存在しないため使わない)
7. `picocom -b 2000000 /dev/ttyACM0` と `/dev/ttyACM1` を **両方** 開く
8. ケースの蓋を閉じ、充電状態のまま放置して 12.6 のログを待つ
9. 判定後、常用しないこと (§11.4 リスク 3 / 12.7 リスク 5)

### 12.6 期待 UART 出力と合否

SDK 側で**新たに出るべき**行 (これが出なければ 12.4 のパッチが効いていない):

```
Yin BATTERY 2                            ← apps.cpp:2007 (2 = CHARGING_PWRON)
CHARGING PWRON!                          ← apps.cpp:2033
app_wait_stack_ready: wait:<n> ms        ← apps.cpp:597
bt_stack_init_done:<n>                   ← apps.cpp:2153 (BesbtInit を通った証拠)
ibrt_ui_log:...                          ← app_ibrt_init() 以降が動いている証拠
```

`bt_stack_init_done` の値は `pwron_case` (この時点ではまだ `apps.cpp:1897` の初期値
`APP_POWERON_CASE_INVALID`、または `:1988` で入る `REBOOT`) なので数値は問わない。
**行が出ること**だけを見る。

その後は **§11.3 の期待出力がそのまま適用される**。合否条件も §11.3 の 4 つ
(`rank=0` と `rank=1` が片側ずつ / `size=2` / `checksum=32768.000000` 厳密一致 /
`ibrt_ui_log:tws cmd send failed` が 0 回) に、本節の 2 つを足す。

- `Yin BATTERY 2` / `CHARGING PWRON!` が出ること
- `[mpi] side=... init_done=1` であること (Run 2 は `init_done=0` だった)

`### EXCEPTION ###` が 1 度も出ないことも併せて確認する (§11.5 の対策の実機確認)。

### 12.7 リスク

1. **充電と BT の同時稼働。** BES 自身の IBRT ターゲット (`best2300p_ibrt` /
   `best2300p_ibrt_anc`) と同じ設定であり、リンクされる IBRT blob もその設定でビルドされている
   (§2.2) ので SDK 的には想定内。電力はケース給電で賄われる。ただし蓋を閉じた密閉状態で
   BT TX + 充電が続くため、**M-T3 は数分で終わらせ、長時間の連続運転はしない**
2. **満充電で電源が落ちる。** `app_battery_handle_process_charging` の
   `app_battery_charger_handle_process() <= 0` 分岐 (`app_battery.cpp:388-392`) が
   `app_shutdown()` を呼ぶ。Run 1 実測で `FULL_CHARGING:4230` の直後にリブートしている。
   ⇒ 計測は満充電を避けて行う。実測で計測を妨げるようなら §11.2.4 と同形の
   **任意パッチ対象 4** として `app_battery.cpp:392` の `app_shutdown()` を潰す。
   §11.2.4 対象 3 と同じ方針で、**まず実測してから足す**
3. **書き込みトリガが変わる。** ~~新ファームは抜き差しでリセットしなくなるので
   bestool がリブートを捕捉できなくなる。回避は背面ボタン長押し~~
   **→ 実測で否定 (2026-09-01 訂正)**。=0 で取り外し時の `app_reset()`
   (`app_battery.cpp` の `CHARGER_PLUGINOUT_RESET` ガード) は確かに無効だが、
   充電起動 (`CHARGING PWRON!`) で立ち上がったバッズはケースから外れると電源ごと落ちる
   (Run 4 前後の抜き差しで再挿入が毎回 `Yin BATTERY 2` のコールドブートになることを実測)。
   よって「出して 3 秒待って戻す」は従来どおり有効 (OpenPineBuds README:27 の公式手順)。
   また当初回避策とした「背面ボタン約 5 秒長押し」(README:41) は
   **実機にそのボタンが存在しない**ため使えない (ユーザー確認)。manual §4 は訂正済み
4. **`is_charging_poweron == true` のまま OUT_BOX を作る**。SDK の SM から見れば
   「充電起動なのにケース外」という想定外状態である。§11.4 リスク 5 と同種のリスクで、
   失敗は §11.2.5 の bounded wait で必ず UART に落ちる
5. **ビルド設定を変えたファームは常用しない。** 自動電源断が無効 (§11.4 リスク 3) で、
   かつ充電中も BT が動き続ける。実験終了後は工場ファーム
   (`docs/manual.md` §4 のバックアップ) に戻せる状態を保つ
6. **`CHARGER_PLUGINOUT_RESET` は open_source ターゲット全体に効く。** 本リポジトリは
   このターゲットしかビルドしないので実害は無いが、SDK を他用途と共有する場合は
   install スクリプトの適用範囲に注意する

---

### 12.8 Run 3 — 充電器 PLUGIN が CLOSE_BOX を注入して走行中のリンクを切る

#### 12.8.1 Run 3 の結果

§12.4 パッチ (実機、2026-09-01) は成功した。両バッズの UART で
`Yin BATTERY 2` → `CHARGING PWRON!` → `bt_stack_init_done` → `ibrt_ui_log:...` が出て、
§11.2.1 の `init_done=1` に到達し、`[mpi] init rank=0 size=2 link_wait=1900 ms` →
`peer ok rank=0 peer=1` まで進んだ。§11.5 で対策した `mpi_compute_thread` の自己終了
MemFault も再発しておらず、**充電起動から BT・TWS・MPI ハンドシェイクまでが定常状態で
初めて確認できた**。

しかし GEMM フレーム交換の途中 (~130 フレーム通過後) で TWS リンクが切断され、
以降のフレームがすべて失敗するようになった。§12.7 リスク 4 で予告した「充電起動なのに
ケース外」という想定外状態が、今回は §11.2.4 対象 3 (未パッチのまま残していた充電器
プラグインの箱イベント注入) という形で現実化した。

#### 12.8.2 機構

UART のエビデンスチェーンは次の順で並ぶ。

```
charger:1                                    ← app_ibrt_search_pair_ui.cpp:495
APP_BATTERY_CHARGER_PLUGIN nv_role 00        ← :498-499
box event:4                                  ← :76 (app_box_handle_timehandler)
custom event entry enter=CLOSE_BOX_EVENT     ← app_ibrt_if_event_entry 経由
entry=app_ibrt_ui_free_link_handler, action=TWS_DISCONNECT
tws cmd send failed, tws link missing        ← 以降の全送信 (SDK cmdcode 8025 含む)
```

コード側の根拠は `services/app_ibrt/src/app_ibrt_search_pair_ui.cpp` の
`app_ibrt_battery_handle_process_normal()` (`:487-533`)。バッテリーのデバウンスが完了して
`APP_BATTERY_STATUS_CHARGING` 状態で `APP_BATTERY_CHARGER_PLUGIN` を受け取ると
(`:493-496`)、

```c
504       if (p_ui_ctrl->config.check_plugin_excute_closedbox_event == true)
505         box_event = IBRT_CLOSE_BOX_EVENT;
506       else
507         box_event = IBRT_PUT_IN_EVENT;
509       if (app_box_handle_timer != NULL) {
510         osTimerStop(app_box_handle_timer);
511         osTimerStart(app_box_handle_timer, 500);
512       }
```

`check_plugin_excute_closedbox_event` は `services/app_ibrt/src/app_ibrt_customif_ui.cpp:684`
で `true` に固定されているビルド設定なので、`box_event` は無条件に
`IBRT_CLOSE_BOX_EVENT` (`= 4`、`services/ibrt_ui/inc/app_ibrt_ui.h:67`) になる。
500 ms 後、`app_box_handle_timer` のハンドラ `app_box_handle_timehandler()`
(`app_ibrt_search_pair_ui.cpp:73-81`) が `TRACE(1, "box event:%d", boxStatus)` を出しつつ
`app_ibrt_if_event_entry(CLOSE_BOX_EVENT)` (`:78`) を呼び、そこから
`app_ibrt_ui_free_link_handler` (`action=TWS_DISCONNECT`) に至って TWS リンクを
**意図的に切断する** (reason 0x16)。以降はどの `tws_ctrl_send_cmd` 呼び出しも
§2.3 のリンク切断ゲートに引っかかり `tws cmd send failed, tws link missing` を返す。

`BOX_DET_USE_GPIO` は本ビルドで未定義 (grep 済み) なので、GPIO 由来の箱検出は無い。
**この充電器 PLUGIN マッピングが、glue 自身の §11.2.3 注入を除けば、実行時に箱イベントを
発生させる唯一の経路である。** §11.2.4 対象 3 として「まず実測してから足す」と保留していたのが
今回の Run 3 で実測されたことになる。PLUGOUT 側も同型 (`:514-527`、`IBRT_FETCH_OUT_EVENT`) で、
box_state を書き換える点は同じなので併せて対処する。

#### 12.8.3 対策 (決定済み)

§11.2.4 の `apps.cpp` パッチと同じ「一意なアンカーを 1 行 `if (0 && ...)` で無効化する」方針を
`app_ibrt_search_pair_ui.cpp` にも適用する。対象は
`app_ibrt_battery_handle_process_normal()` 内の 2 箇所、PLUGIN 分岐 (`:509-512`) と
PLUGOUT 分岐 (`:523-526`) にある同一のタイマ起動ブロック:

```c
      if (app_box_handle_timer != NULL) {
        osTimerStop(app_box_handle_timer);
        osTimerStart(app_box_handle_timer, 500);
      }
```

を

```c
      if (0 && app_box_handle_timer != NULL) { /* pine-buds-cluster charger box events */
        osTimerStop(app_box_handle_timer);
        osTimerStart(app_box_handle_timer, 500);
      }
```

に置き換える。`box_event` 自体への代入とログ (`TWSCON_DBLOG`) はそのまま残すので、
充電器のプラグ/アンプラグは引き続き UART に記録されるが、`app_box_handle_timer` が
一度も起動されなくなるため `app_box_handle_timehandler()` → `app_ibrt_if_event_entry()` へは
到達しない。`scripts/install-into-sdk.sh` の手順 8 がこの置換を担う。マーカー文字列
`pine-buds-cluster charger box events` で冪等性を確保し、既存フック
(手順 6 の `apps.cpp` パッチ) と同じ「一意アンカーの `str.replace` 前に出現数を assert する」
形にした。

このパッチにより、箱状態を書き換える経路は **glue の §11.2.3 注入だけ**になる。
`BOX_DET_USE_GPIO` が未定義であることは既に確認済みなので、他に競合する箱イベント源は無い。

#### 12.8.4 リスク

- PLUGOUT 側 (`app_ibrt_search_pair_ui.cpp:523-526`) も同時に無効化するため、
  ケースから取り出したときの `IBRT_FETCH_OUT_EVENT` 注入も死ぬ。ただし本実験手順では
  バッズをケースから取り出して使うことは無い (常にケース内で充電起動する) ので影響は無い。
  工場ファームに戻せば元の挙動に戻る。§12.7 リスク 5 (常用しないこと) と同じ位置づけの
  制約であり、新たなリスク区分を増やすものではない。

### 12.9 Run 4 実測結果 — 全条件 PASS (2026-09-01)

§11.5 (スレッドパーク) + §12.4 (充電起動) + §12.8 (充電器箱イベント遮断) の 3 修正を積んだ
`BUILD_DATE=Aug 31 2026 16:35:12` (コンテナ UTC) を両バッズに書き込み、ケース内・蓋閉・
充電状態で再起動した結果:

```
右: Yin BATTERY 2 → CHARGING PWRON! → bt_stack_init_done:10
    [mpi] side=RIGHT rank=0 nv_role=[IBRT_MASTER] init_done=1
    [mpi] init rank=0 size=2 link_wait=200 ms besaud=1
    [mpi] peer ok rank=0 peer=1 → [mpi] barrier ok
    GEMM-MPI N=32 rank=0 size=2 checksum=32768.000000 expect=32768.000000 PASS
    GEMM-MPI elapsed=13 ms frames tx=2 rx=4 err=0
    [mpi] finalize done rank=0
左: 同経路で rank=1 size=2、link_wait=500 ms、[mpi] send ok、
    frames tx=2 rx=2 err=0、[mpi] finalize done rank=1
```

§11.3 の 4 条件 + §12.6 の 2 条件をすべて満たす:

1. rank=0 (右) / rank=1 (左) が片側ずつ — 左右ストラップ由来の静的 rank が設計どおり
2. 両側 size=2
3. checksum=32768.000000 厳密一致 (rank=0 が集約して判定。rank=1 は設計上出力しない)
4. MPI cmdcode 0x8201 の `tws cmd send failed` は左右とも 0 回
5. `Yin BATTERY 2` / `CHARGING PWRON!` が両側に出た
6. 両側 `init_done=1`

`### EXCEPTION ###` は左右とも 0 回・リブート無し — §11.5 のパーク対策の実機確認も完了。

備考:

- finalize の後、右ログに SDK 内部 cmdcode 0x8025 の send failed が 1 回だけ出た
  (finalize の約 160 行後)。計算完了後に SDK が TWS リンクを解放した際の周期送信の
  空振りであり、MPI 実行には無関係。合否判定 (条件 4) は MPI トラフィック (0x8201) で見る
- **実機のケースに「背面ボタン」は存在しない** (ユーザー確認、2026-09-01)。実際の
  リブートは「出して 3 秒待って戻す」で成立した — 充電起動したバッズは取り外しで電源ごと
  落ちるため、=0 でも再挿入が必ずコールドブートになる。§12.5 手順 6・§12.7 リスク 3・
  manual §4 は訂正済み

これをもって §11.3 に定義した Phase 1 実機合格条件は **達成**。

---

## 13. Bluetooth SPP ログチャネル設計 (2026-09-01)

目的: GEMM の checksum・elapsed など `COMPUTE_TRACE` 経由のログ行を、Bluetooth で接続した
Windows PC にリアルタイムで届ける。上位の狙いは「イヤホンとして使いながら裏で並列計算する
クラスタ」の観測基盤であり、その第一歩として **USB テザー (2 Mbaud UART) を撤廃できる状態**
を作る。UART 出力は 1 バイトも変えず、SPP は**同じ行を並行して流す tap** として足す。

方式は **SPP (RFCOMM)** で確定済み。根拠は本ビルドの構成そのもので、
`BLE ?= 0` (`config/open_source/target.mk:213`) と `TOTA ?= 0` (`:215`) によって
BLE も TOTA も丸ごとコンパイル除外される (`-DTEST_OVER_THE_AIR_ENANBLED` は
`target.mk:424-429` で `TOTA=1` のときだけ付く) 一方、`services/bt_app/app_spp.cpp` と
`app_rfcomm_mgr.cpp` は無条件にビルドされており
(`out/open_source/services/bt_app/app_spp.o`, `app_rfcomm_mgr.o` が存在)、
その下地となる `btif_spp_*` / `btif_sdp_*` は実際にリンクされる
`services/bt_if_enhanced/lib/ibrt_libbt_api_sbc_enc_2m_RTX.a` に定義済みである (`nm` 実測、13.1.1)。

前提として引き継ぐ事実:

- 実機 Run 4 (§12.9) で 2 ランク MPI GEMM は合格済み。運用はケース内・充電起動 (`CHARGING PWRON!`)
- rank=0 = 右バッズが `checksum` / `elapsed` を出す。**v1 は右バッズ 1 台からのストリーム**とし、
  左 (rank=1) は将来拡張とする
- MPI は TWS リンク上の cmdcode `0x8201` (§3-§10)。SPP は**モバイル ACL 側**であり別経路
- PC と PineBuds Pro は既にペアリング済み (どのファーム時点のボンドかは不明)。
  設計は「既存ボンドでの再接続」と「新規ペアリング」の両方を扱う
- WSL2 に Bluetooth は無い。受信は Windows 側の仮想 COM ポートで行い、WSL へは
  `/mnt/c` 配下のファイル経由で渡す (13.7)

---

### 13.1 SDK の SPP API 実仕様

#### 13.1.1 本ビルドに実在する API 面

`nm` 実測 (`services/bt_if_enhanced/lib/ibrt_libbt_api_sbc_enc_2m_RTX.a`) で
以下がすべて `T` で定義されていることを確認した。

```
btif_create_spp_device   btif_create_spp_service  btif_spp_init_device
btif_spp_init_rx_buf     btif_spp_service_setup   btif_spp_open
btif_spp_open_server     btif_spp_open_client     btif_spp_read
btif_spp_write           btif_spp_close           btif_spp_disconnect
btif_spp_get_server_channel  btif_sdp_create_record  btif_sdp_record_setup
btif_sdp_add_record      btif_me_set_accessible_mode
```

宣言はすべて `services/bt_if_enhanced/inc/spp_api.h:119-155` と
`services/bt_if_enhanced/inc/sdp_api.h:482-511` にある。SPP の実行基盤も
リンク済みで、実 ELF (`out/open_source/open_source.elf`) に
`create_spp_read_thread` (`0x0c066975`)、静的な `spp_read_thread` (`0x0c0668d5`)、
`spp_devices` / `spp_read_thread_id` / `spp_mailbox` が入っている。
デバイス/サービスのプールは各 6 本 (`spp_api.h:25-26`
`SPP_DEVICE_NUM 6` / `SPP_SERVICE_NUM 6`) で、本ビルドは 1 本も使っていないので全部空いている。

**注意すべき 3 つの穴** (いずれも実測):

1. `app_spp.o` / `app_rfcomm_mgr.o` は**コンパイルされているがリンク後の ELF には残っていない**。
   `nm out/open_source/open_source.elf | grep -c "app_spp_send_data\|app_rfcomm_open"` = 0。
   `Makefile:886` の `--gc-sections` が、誰も参照していないので落としている。
   我々が参照した時点で復活するので使えることに変わりはないが、
   「.o があるから今も動いている」という読み方は誤り
2. `bool btif_sppos_is_txpacket_available(struct spp_device *dev)` は `spp_api.h:121` に
   **宣言だけあってライブラリに定義が無い** (`nm --defined-only` に出ない)。
   呼ぶとリンクエラーになる。送信可否は自前のフラグで持つ (13.3.4)
3. `app_spp.cpp:47-75` の `app_spp_send_data()` は `umm_malloc()` (`:61`) で複製した
   バッファを `btif_spp_write` に渡すが、**成功パスで `umm_free` する経路がどこにも無い**
   (`:69-72` の失敗時だけ free)。送信のたびにヒープが減る。**使わない**

#### 13.1.2 サービス登録の呼び出し順 (実例: TOTA)

`services/tota/app_spp_tota.cpp:300-351` の `app_spp_tota_init()` が唯一の完全な実例である
(本ビルドではコンパイルされないが、ソースは読めるし API は上記のとおり実在する)。順序は:

```c
tota_spp_dev = btif_create_spp_device();                       /* :307 */
btif_spp_init_rx_buf(tota_spp_dev, rx_buf, SPP_RECV_BUFFER_SIZE); /* :319 */
mid          = osMutexCreate(osMutex(tota_spp_mutex));          /* :321 */
tota_spp_dev->creditMutex = osMutexCreate(osMutex(tota_credit_mutex)); /* :326-328 */
tota_sdp_record = btif_sdp_create_record();                     /* :331 */
param.attrs = TotaSppSdpAttributes1; param.attr_count = ...;    /* :333-334 */
param.COD   = BTIF_COD_MAJOR_PERIPHERAL;                        /* :335 */
btif_sdp_record_setup(tota_sdp_record, &param);                 /* :336 */
totaSppService = btif_create_spp_service();                     /* :339 */
totaSppService->rf_service.serviceId = RFCOMM_CHANNEL_TOTA;     /* :341 */
totaSppService->numPorts = 0;                                   /* :342 */
btif_spp_service_setup(tota_spp_dev, totaSppService, tota_sdp_record); /* :343 */
tota_spp_dev->portType = BTIF_SPP_SERVER_PORT;                  /* :345 */
tota_spp_dev->app_id   = BTIF_APP_SPP_SERVER_TOTA_ID;           /* :346 */
tota_spp_dev->spp_handle_data_event_func = tota_spp_handle_data_event_func; /* :347 */
btif_spp_init_device(tota_spp_dev, 5, mid);                     /* :348 */
btif_spp_open(tota_spp_dev, NULL, spp_tota_callback);           /* :350 */
```

`app_id` は `services/bt_if_enhanced/inc/bt_if.h:32-42` の
`BTIF_APP_SPP_SERVER_ID_1..10` から 1 つ選ぶ。`services/bt_app/app_spp.h:36-44` が
ID_1〜ID_9 を GSound/TOTA/OTA/AI/GREEN/RED/FastPair/TOTA_GENERAL に割り当てているので、
**`BTIF_APP_SPP_SERVER_ID_10` (`bt_if.h:41`) が唯一の未割当**である。これを使う。

RFCOMM チャネル番号は `spp_api.h:161-172` の `RFCOMM_CHANNEL_1..10` = **10..19**。
`app_spp.h:55-65` (`#if defined(ENHANCED_STACK)` の中。`ENHANCED_STACK=1` は
`config/open_source/target.mk:268` → `config/common.mk:1532` で確定) が
`RFCOMM_CHANNEL_1..9` を既存サービスに割り当てているので、
**`RFCOMM_CHANNEL_10` (= 19) が未割当**。これを使う。将来 TOTA や FastPair を有効化しても
チャネルが衝突しない。

> **実測で覆った (2026-09-01, §13.13)。** この blob では `btif_spp_open` が ID_10 / チャネル 19 に
> `BT_STS_FAILED` を返す (`rfcomm_register_server: channel 19 already existing.`)。他に 19 を
> 登録する者は居らず、自分の登録が拒否されているだけだが原因は blob 内で追えない。SDP だけが 19 を
> 広告し続け、辿った Windows は HFP チャネルに着地した (`AT+BRSF=767` を受信)。
> **採用は TOTA のスロット `RFCOMM_CHANNEL_3` (= 12) / `BTIF_APP_SPP_SERVER_ID_3`**
> (TOTA ?= 0 で本ビルドに未リンク、出荷実績のある唯一のスロット)。Run 9 で両側
> `[spplog] init chan=12 setup=0 open=0`。20 番以上の生値を使う案は出荷実績が無いので見送り。

#### 13.1.3 コールバックとスレッドコンテキスト [逆アセ]

コールバックは 2 系統ある (`spp_api.h:75-78`)。

| コールバック | 型 | 何が来るか | 実行スレッド |
|---|---|---|---|
| `spp_callback_t spp_callback` | `void(*)(spp_device*, spp_callback_parms*)` | CONNECTED / DISCONNECTED / DATA_SENT / *_IND | **`BesbtThread`** |
| `spp_handle_data_event_func_t spp_handle_data_event_func` | `int(*)(void*, uint8_t, uint8_t*, uint16_t)` | 受信データ | **`spp_read_thread`** |

根拠 **[逆アセ]**: `_btif_sppnew_callback` (`0x0c066cb9`、
`services/bt_if_enhanced/lib/ibrt_libbt_api_sbc_enc_2m_RTX.a`) がイベントを振り分けており、

- CONNECTED (`0x0c066d64-0c066d7c`)、DISCONNECTED (`0x0c066db8-0c066dd8`)、
  DATA_SENT (`0x0c066dfc-0c066e1a`) はいずれも `ldr r3,[rX,#0x34]` →
  `blx r3`、つまり `spp_device.spp_callback` を **RFCOMM のコールバック文脈からそのまま
  同期呼び出し**している。RFCOMM/L2CAP は `bt_process_stack_events()`
  (`services/bt_app/besmain.cpp:463`) から回るので、**ここは §2.5 と同じ `BesbtThread`
  (osPriorityAboveNormal)** である
- データ受信だけは `0x0c066da8` の `spp_mailbox_put` に積まれ、
  `spp_read_thread` (`0x0c0668d5`) が `spp_mailbox_get` → `btif_spp_read` →
  `blx [dev+0x38]` (= `spp_handle_data_event_func`) で処理する

⇒ **接続/切断/TX 完了のハンドラは絶対にブロックできない。** §2.5 で確認したとおり、
`BesbtThread` を止めると MPI の送受信ポンプ (`besmain.cpp:480-481`) も止まる。
本設計ではこの 3 つのコールバックで**リングバッファに触らない** (13.3.4)。

DATA_SENT の引数は `spp_callback_parms.p.other` を `struct spp_tx_done*`
(`spp_api.h:44-47`, `{uint8_t *tx_buf; uint16_t tx_data_length;}`) にキャストして読む。
実例は `services/bt_app/app_rfcomm_mgr.cpp:150-154`。

#### 13.1.4 送信の契約

| 事項 | 値 | 根拠 |
|---|---|---|
| L2CAP MTU | 672 (`__3M_PACK__` 時 980) | `services/bt_app/app_spp.h:26-30` |
| 1 パケット上限 | `SPP_MAX_DATA_PACKET_SIZE` = 672 | `app_spp.h:33` |
| 実運用パケット長の前例 | 666 | `services/tota/tota_stream_data_transfer.h:23` |
| 受信バッファ | `SPP_RECV_BUFFER_SIZE` = 672*4 = 2688 | `app_spp.h:32` |
| 送信 API | `bt_status_t btif_spp_write(struct spp_device*, char *buffer, uint16_t *nBytes)` | `spp_api.h:134` |
| 同時 in-flight パケット数 | `btif_spp_init_device(dev, numPackets, mutex)` の第 2 引数 | `spp_api.h:119` |
| TX バッファの所有権 | **複製されない。DATA_SENT が返るまで呼び出し側が保持する** | 下記 |

TX バッファが複製されない根拠: TOTA は `MAX_SPP_PACKET_SIZE * 5` の静的配列を
5 スロットのリングとして回し (`tota_stream_data_transfer.cpp:58-61`)、
`_get_tx_buf_ptr()` にコピーしてから `app_spp_tota_send_data()` →
`btif_spp_write` に渡し (`:143-146`, `app_spp_tota.cpp:292-298`)、
スロットの解放を **TX done のセマフォ解放** (`tota_stream_data_transfer.cpp:156`
`app_tota_tx_done_callback()`、呼び元は `app_spp_tota.cpp:283-284` の
`BTIF_SPP_EVENT_DATA_SENT`) で行っている。スロット数 5 は
`btif_spp_init_device(tota_spp_dev, 5, mid)` (`app_spp_tota.cpp:348`) と一致する。
**この「静的スロット + TX done で返す」形をそのまま踏襲する。**

⇒ §2.3 の `tws_ctrl_send_cmd` (バッファを複製する) とは契約が逆である。
MPI 側の送信バッファ 1 枚設計 (§7) の理屈を SPP に持ち込んではいけない。

#### 13.1.5 採用する API 面 (決定)

**`btif_spp_*` / `btif_sdp_*` を直接呼ぶ。** `app_spp.cpp` の `app_spp_send_data()` /
`app_spp_open()` も `app_rfcomm_mgr.cpp` の `app_rfcomm_open()` も使わない。理由:

- `app_spp_send_data()` は 13.1.1-3 のとおりヒープを漏らす
- `app_rfcomm_mgr.cpp` は ServiceClassIDList を **128 bit カスタム UUID 固定**で組む
  (`:41-48` の `RFCOMM_NULL_UUID_128` テンプレート + `:288-295` で
  `ptConfig->rfcomm_128bit_uuid` を埋める)。13.2 のとおり Windows に COM ポートを
  生やすには `0x1101` が必要なので、この経路では要件を満たせない
- `btif_*` を直接呼べば **SDK ソースの変更はサービス初期化フック 1 行のみ** (13.8) で済む

---

### 13.2 SDP レコードと UUID の決定

#### 13.2.1 公式仕様側の確認

- SPP の SDP レコードでは `ServiceClassIDList` に **SerialPort UUID `0x1101`** を、
  `ProtocolDescriptorList` に **L2CAP + RFCOMM (server channel 付き)** を持つことが必須。
  `BluetoothProfileDescriptorList` (SerialPort `0x1101` / version `0x0102`) は
  SPP v1.2 以降で必須、`ServiceName` は付けるのが通例
  (Bluetooth Core SDP 仕様、SPP 仕様 v1.1/v1.2)
- レコードへの**属性追加は許可されている**ので、カスタム 128 bit UUID を
  `0x1101` と**併記**するのは適法。ただし `0x1101` を**置き換える**のは不可 ——
  Windows は SerialPort サービスクラスを見て COM ポートを生やすため
- Windows は SPP でペアリングしたデバイスに対し**発信 (outgoing) と着信 (incoming) の
  2 本の仮想 COM ポート**を作る。発信ポートは「開いた瞬間に RFCOMM 接続を張る」挙動
  (Microsoft の COM Port Emulation の記述)。UI 上の位置は
  「設定 → Bluetooth とデバイス → デバイス → その他の Bluetooth 設定 → **COM ポート**」
- SDP キャッシュ: Microsoft Learn は Winsock (`WSALookupServiceBegin`) について
  「Bluetooth は近傍デバイスの SDP レコードを先読みキャッシュしないし、過去の問い合わせも
  積極的にはキャッシュしない」「確実に線に出したければ `LUP_FLUSHCACHE` を渡せ」と明記している。
  **ただしこれは API 層の話で、既存ボンドのデバイスにファーム側が後から SPP サービスを
  足したとき、設定 UI の COM ポート一覧が自動で追随するかは Microsoft 文書では確認できなかった。**
  ⇒ **実機確認項目 (13.11-1)**
- connectable-but-not-discoverable なデバイスに対して発信 COM ポートを作れるかも
  Microsoft 文書では確認できなかった ⇒ **実機確認項目 (13.11-2)**

#### 13.2.2 決定

**標準 SerialPort UUID `0x1101` を採用し、カスタム 128 bit UUID は使わない。**
`services/tota/app_spp_tota.cpp:157-160` の `TotaSppClassId` と同型にする。
COM ポートを自動で生やす条件が公式に取れていない以上、
「SDP レコードの中身が原因」という変数を最初から消しておくのが最短である。
サービスの識別は ServiceName 文字列と RFCOMM チャネル番号で足りる。

```c
/* firmware/pinebuds_compute/spp_log_service.cpp (target-only) */

static const U8 LogSppClassId[] = {                 /* app_spp_tota.cpp:157-160 と同型 */
    SDP_ATTRIB_HEADER_8BIT(3),                      /* sdp_api.h:160 */
    SDP_UUID_16BIT(SC_SERIAL_PORT),                 /* 0x1101  sdp_api.h:271, :192 */
};

static const U8 LogSppProtoDescList[] = {           /* app_spp_tota.cpp:162-186 と同型 */
    SDP_ATTRIB_HEADER_8BIT(12),
    SDP_ATTRIB_HEADER_8BIT(3),
    SDP_UUID_16BIT(PROT_L2CAP),                     /* 0x0100  sdp_api.h:358 */
    SDP_ATTRIB_HEADER_8BIT(5),
    SDP_UUID_16BIT(PROT_RFCOMM),                    /* 0x0003  sdp_api.h:339 */
    SDP_UINT_8BIT(RFCOMM_CHANNEL_LOG),              /* = RFCOMM_CHANNEL_10 = 19  sdp_api.h:223 */
};

static const U8 LogSppProfileDescList[] = {         /* app_spp_tota.cpp:191-199 と同型 */
    SDP_ATTRIB_HEADER_8BIT(8),
    SDP_ATTRIB_HEADER_8BIT(6),
    SDP_UUID_16BIT(SC_SERIAL_PORT),
    SDP_UINT_16BIT(0x0102),                         /* SPP v1.2  sdp_api.h:227 */
};

static const U8 LogSppServiceName[] = {             /* app_spp_tota.cpp:204-210 と同型 */
    SDP_TEXT_8BIT(12),                              /* sdp_api.h:247 */
    'P','i','n','e','B','u','d','s','L','o','g','\0'
};

static sdp_attribute_t LogSppSdpAttributes[] = {
    SDP_ATTRIBUTE(AID_SERVICE_CLASS_ID_LIST,   LogSppClassId),       /* sdp_api.h:362, :148 */
    SDP_ATTRIBUTE(AID_PROTOCOL_DESC_LIST,      LogSppProtoDescList), /* sdp_api.h:365 */
    SDP_ATTRIBUTE(AID_BT_PROFILE_DESC_LIST,    LogSppProfileDescList),/* sdp_api.h:370 */
    SDP_ATTRIBUTE((AID_SERVICE_NAME + 0x0100), LogSppServiceName),   /* sdp_api.h:375 */
};
```

初期化本体 (13.1.2 の順序をそのまま踏襲):

```c
static struct spp_device  *s_dev;
static struct spp_service *s_service;
static btif_sdp_record_t  *s_record;
static uint8_t             s_rx_buf[256];   /* 受信は使わないが btif_spp_init_rx_buf は必須 */

osMutexDef(spp_log_mutex);
osMutexDef(spp_log_credit_mutex);

extern "C" void spp_log_service_init(void)      /* BesbtThread 上で 1 回だけ (13.8) */
{
    btif_sdp_record_param_t param;

    s_dev = btif_create_spp_device();
    btif_spp_init_rx_buf(s_dev, s_rx_buf, sizeof(s_rx_buf));
    s_dev->creditMutex = osMutexCreate(osMutex(spp_log_credit_mutex));

    s_record = btif_sdp_create_record();
    param.attrs      = &LogSppSdpAttributes[0];
    param.attr_count = ARRAY_SIZE(LogSppSdpAttributes);
    param.COD        = BTIF_COD_MAJOR_PERIPHERAL;   /* 0x00000500  me_api.h:559 */
    btif_sdp_record_setup(s_record, &param);        /* sdp_api.h:488 */

    s_service = btif_create_spp_service();
    s_service->rf_service.serviceId = RFCOMM_CHANNEL_LOG;
    s_service->numPorts             = 0;
    btif_spp_service_setup(s_dev, s_service, s_record);

    s_dev->portType                 = BTIF_SPP_SERVER_PORT;   /* spp_api.h:37 */
    s_dev->app_id                   = BTIF_APP_SPP_SERVER_ID_10;  /* bt_if.h:41 */
    s_dev->spp_handle_data_event_func = spp_log_rx_discard;
    btif_spp_init_device(s_dev, SPP_LOG_TX_SLOTS /* = 2 */,
                         osMutexCreate(osMutex(spp_log_mutex)));
    btif_spp_open(s_dev, NULL, spp_log_callback);   /* NULL = server, 待ち受け */
}
```

受信は使わないので `spp_log_rx_discard()` は `return 0;` だけ置く
(登録しないと `spp_read_thread` の `[dev+0x38]` が NULL で、逆アセ上は
`0x0c06694a-0c066950` の `cbz` で無視されるだけだが、明示しておく)。

---

### 13.3 ログタップの構造

#### 13.3.1 方針

**SDK の TRACE 全量は流さない。`COMPUTE_TRACE` を通った行だけを流す。**
`firmware/pinebuds_compute/compute_trace.h:14` の定義を 2 段にする。

```c
#ifdef PINEBUDS_TARGET
#include "hal_timer.h"
#include "hal_trace.h"
void compute_log_tap(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define COMPUTE_TRACE(nargs, fmt, ...)          \
    do {                                        \
        TRACE(nargs, fmt, ##__VA_ARGS__);       \
        compute_log_tap(fmt, ##__VA_ARGS__);    \
    } while (0)
#else
/* ホスト側は現行のまま printf。tap はホストでは存在しない */
#define COMPUTE_TRACE(nargs, fmt, ...) std::printf(fmt "\n", ##__VA_ARGS__)
#endif
```

`compute_log_tap` は `vsnprintf` で 1 行を組む。これは新規の依存ではない ——
SDK の `TRACE` 自身が `hal_trace_printf` → `hal_trace_format_va()` →
`vsnprintf` (`platform/hal/hal_trace.c:1113-1118`) を通っており、
`vsnprintf` / `snprintf` は実 ELF に既にリンクされている (`nm` 実測)。
既存の `COMPUTE_TRACE` 呼び出しは `%f` を一切使わず整数に落としてある
(`firmware/pinebuds_compute/compute_main.cpp:48-50` のコメントのとおり)
ので、浮動小数変換のスタック/ヒープ挙動を踏まない。**この制約は §13 でも維持する。**

なお `hal_trace.c:1119-1123` により UART 側は `TRACE_CRLF` で `\r\n` が付く。
SPP 側は `\n` のみとし、受信側での正規化を不要にする (13.6)。

#### 13.3.2 リングバッファ (ターゲット非依存の純ロジック)

配置は **`firmware/pinebuds_compute/log_ring.{h,cpp}`**。理由:

- リポジトリ規約 (`CLAUDE.md`) では `src/` は「ターゲット非依存の**計算カーネル**、純関数のみ」。
  リングバッファは状態を持つので `src/` の定義に合わない
- `adapters/` は「標準 API 互換層」であり、これも該当しない
- `firmware/pinebuds_compute/` は統合層だが、`log_ring.{h,cpp}` は **SDK ヘッダを一切
  include しない**ので `tests/` から `g++` で直接コンパイルできる。`Makefile` の
  `TARGET_DIALECT` は既に `-Ifirmware/pinebuds_compute` を持っているため
  `check98` にもそのまま乗る

```c
/* firmware/pinebuds_compute/log_ring.h — gnu++98 / freestanding / ヒープ無し */
enum { LOG_RING_CAPACITY = 4096 };   /* 2 のべき乗 (& (CAP-1) でインデックス) */

struct log_ring {
    char     buf[LOG_RING_CAPACITY];
    unsigned free_pos;    /* 絶対位置。ここより前は破棄済み */
    unsigned write_pos;   /* 絶対位置。次に書く場所 */
    unsigned dropped;     /* 捨てた行数 (累積) */
};

void     log_ring_init(struct log_ring *r);

/* 1 行を積む (末尾の '\n' は log_ring 側で付ける)。
   空きが足りなければ足りるまで最古の行を丸ごと捨て、そのたび dropped++。
   積めたら 1。len+1 > LOG_RING_CAPACITY なら何も捨てずに 0 を返し dropped++。 */
int      log_ring_push(struct log_ring *r, const char *line, unsigned len);

/* 送信用に最大 max バイトを dst にコピーする。消費はしない (const)。
   *base_out にコピー開始の絶対位置を返す。行の途中で切ってよい
   (SPP はバイトストリームであり、行境界は受信側の '\n' で回復する)。 */
unsigned log_ring_peek(const struct log_ring *r, char *dst, unsigned max,
                       unsigned *base_out);

/* base から n バイトが確かに送れたことを確定する。
   free_pos = max(free_pos, base + n) (符号なし差分比較)。
   push 側の drop で既に free_pos が追い越していれば no-op。 */
void     log_ring_commit(struct log_ring *r, unsigned base, unsigned n);

unsigned log_ring_used(const struct log_ring *r);
unsigned log_ring_take_dropped(struct log_ring *r);  /* 読んで 0 に戻す */
```

設計上のキモは **`peek` が何も動かさない**ことである。SPP の TX バッファは
DATA_SENT まで生かす必要がある (13.1.4) が、その間もリングは押し出され続けてよい。
`commit(base, n)` を「絶対位置での前進のみ」にしておけば、
in-flight 中に drop-oldest で `free_pos` が追い越しても整合が壊れない。
所有権も単純になる:

- **producer** (`log_ring_push`) — `compute` スレッドと `BesbtThread` (glue の RX ハンドラ)
- **consumer** (`peek` / `commit` / `take_dropped`) — ログ送信スレッドのみ

#### 13.3.3 タップ本体

```c
static struct log_ring s_ring;
static osMutexId       s_ring_mid;
static osThreadId      s_log_tid;
static unsigned        s_seq;
static unsigned        s_contended;   /* try-lock に失敗して捨てた行数 */

extern "C" void compute_log_tap(const char *fmt, ...)
{
    char    line[192];
    va_list ap;
    int     n;

    if (s_log_tid == NULL) return;                 /* 初期化前 */
    if (osMutexWait(s_ring_mid, 0) != osOK) {      /* 0 = 待たない (cmsis_os.h:550) */
        s_contended++;                             /* 統計だけ取って戻る */
        return;
    }
    n = snprintf(line, sizeof line, "#%u ", s_seq);
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (unsigned)n, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        s_seq++;
        log_ring_push(&s_ring, line, (unsigned)n);
    }
    osMutexRelease(s_ring_mid);
    osSignalSet(s_log_tid, 0x1);                   /* cmsis_os.h:497 */
}
```

**`osMutexWait(..., 0)` の try-lock がこの設計の要である。**
`compute` スレッド (osPriorityBelowNormal、§6) も `BesbtThread` (osPriorityAboveNormal、§2.5)
も**一切待たない**。取れなければその行を落として `s_contended++` するだけなので、
GEMM のタイミングにも BT スタックにも待ちを持ち込まない。
`osSignalSet` は別スレッドから呼んで安全 (`cmsis_os.h:52-65`、§2.7 と同じ根拠)。

#### 13.3.4 送信ステートマシン

送信は専用スレッドが回す。**`BesbtThread` 上では `btif_spp_write` を呼ばず、
リングにも触らない。**

```c
osThreadDef(spp_log_thread, osPriorityLow, 1, 1024, "spp_log");  /* cmsis_os.h:165, :347-356 */

enum { SPP_LOG_CHUNK = 512 };          /* < L2CAP_MTU 672 (app_spp.h:29) */
static char     s_tx_slot[SPP_LOG_CHUNK];
static volatile int      s_connected;
static volatile int      s_inflight;
static volatile unsigned s_done_len;   /* BesbtThread が書き、log スレッドが読む */
static unsigned          s_inflight_base;
static unsigned          s_inflight_len;
```

`spp_log_callback` (= `spp_device.spp_callback`、**`BesbtThread`**):

```c
static void spp_log_callback(struct spp_device *dev, struct spp_callback_parms *info)
{
    switch (info->event) {
    case BTIF_SPP_EVENT_REMDEV_CONNECTED:        /* spp_api.h:29 */
        s_connected = 1; s_inflight = 0;
        TRACE(0, "[spplog] connected");
        break;
    case BTIF_SPP_EVENT_REMDEV_DISCONNECTED:     /* spp_api.h:30 */
        s_connected = 0; s_inflight = 0;
        TRACE(0, "[spplog] disconnected");
        break;
    case BTIF_SPP_EVENT_DATA_SENT: {             /* spp_api.h:31 */
        struct spp_tx_done *d = (struct spp_tx_done *)info->p.other;  /* spp_api.h:44-47 */
        s_done_len = d->tx_data_length;
        s_inflight = 0;
        break;
    }
    default: break;
    }
    osSignalSet(s_log_tid, 0x1);   /* 実処理はログスレッドへ */
}
```

ログスレッド:

```c
static void spp_log_thread_body(void const *arg)
{
    for (;;) {
        osSignalWait(0x1, 200);            /* 200 ms でも起きる = access mode 再アーム周期 */
        spp_log_rearm_access_mode();       /* 13.4 */

        if (s_inflight) continue;
        if (s_inflight_len) {              /* 直前の送信を確定 */
            log_ring_commit(&s_ring, s_inflight_base,
                            s_done_len < s_inflight_len ? s_done_len : s_inflight_len);
            s_inflight_len = 0;
        }
        if (!s_connected) continue;

        unsigned d = log_ring_take_dropped(&s_ring);
        unsigned n;
        if (d != 0) {
            /* drop マーカは単独チャンクで送る。リングは 1 バイトも消費しない */
            n = (unsigned)snprintf(s_tx_slot, sizeof s_tx_slot,
                                   "#- [log] dropped=%u contended=%u\n", d, s_contended);
            s_inflight_base = 0; s_inflight_len = 0;
        } else {
            osMutexWait(s_ring_mid, osWaitForever);      /* log スレッドだけは待ってよい */
            n = log_ring_peek(&s_ring, s_tx_slot, sizeof s_tx_slot, &s_inflight_base);
            osMutexRelease(s_ring_mid);
            s_inflight_len = n;
        }
        if (n == 0) continue;

        uint16_t len = (uint16_t)n;
        s_inflight = 1;
        if (btif_spp_write(s_dev, s_tx_slot, &len) != BT_STS_SUCCESS) {
            s_inflight = 0; s_inflight_len = 0;          /* 次の周期で再試行 */
        }
    }
}
```

優先度は **`osPriorityLow` (`cmsis_os.h:165`)**。`compute` スレッド
(osPriorityBelowNormal、§6-2) より下に置く。GEMM は 13 ms (§12.9) で終わるので
計算中に送信が止まっても実害は無く、むしろ **GEMM の `elapsed` に SPP 送信の摂動を
乗せない**ことを優先する。in-flight は 1 本 (`s_inflight`)、
`btif_spp_init_device(dev, 2, ...)` でスタック側スロットは 2 本取り、
1 本は余裕として残す。

メモリ増分: リング 4096 B + TX スロット 512 B + RX 2688 B + スレッドスタック 1024 B +
状態 ≒ **約 8.3 KB**。§7 の合計 22.7 KB に足しても RAM 残量 (約 330 KB) に対して十分。
(2026-09-01 実機訂正: 当初 RX 256 B としていたが、Run 5 で
`_btif_spp_create_channel` が `rx buffer is too small` を ASSERT。スタックの最小要件は
`SPP_RECV_BUFFER_SIZE` = `L2CAP_MTU*4` = 2688 B — TOTA (`app_spp_tota.cpp:59`) と同じ)

---

### 13.4 接続可能モード (page scan) の設計

#### 13.4.1 事実 — Run 4 では MPI 完了後に非接続可能になる

Run 4 の右バッズログ (`run4-right-uart.log`) から:

```
685: j_scan=OPEN_BOX,    l_type=NO_LINK_TYPE, mode=0x2, p_mode=0
687: ibrt_ui_log:set_access_mode=2, ca=0xc05341f
689: write_scan_enable=2
...
817: ibrt_ui_log:set_access_mode=0, ca=0xc05141b
819: write_scan_enable=0
823: j_scan=CONNECTED,   l_type=TWS_LINK, mode=0x0, p_mode=0
827: ibrt_ui_log:filter access mode=0, current access mode=0
```

`ca=` は呼び出し元アドレスであり、実 ELF のシンボル表で解決すると:

| `ca` | 解決先 |
|---|---|
| `0xc05341f` | `app_ibrt_ui_open_box_event_handler+0x19` |
| `0xc05141b` | `app_ibrt_ui_judge_scan_type+0x286` |
| `0xc0552a7` | `app_ibrt_ui_global_handler+0x1b6` |
| `0xc059303` | `app_tws_ibrt_create_tws_connection+0x2d` |
| `0xc058f99` | `app_tws_ibrt_delay_slave_create_connection+0x1b` |

値の意味は `services/bt_if_enhanced/inc/me_api.h:382-392`:
`BTIF_BAM_NOT_ACCESSIBLE 0x00` (`:384`) / `BTIF_BAM_DISCOVERABLE_ONLY 0x01` (`:390`) /
`BTIF_BAM_CONNECTABLE_ONLY 0x02` (`:389`) / `BTIF_BAM_GENERAL_ACCESSIBLE 0x03` (`:385-386`)。
bit0 = inquiry scan (discoverable)、bit1 = page scan (connectable) で
HCI `Write_Scan_Enable` (`services/bt_profiles_enhanced/inc/hci.h:920` `0x0C1A`) の
パラメータに 1:1 対応する。

⇒ **TWS リンク確立後、右バッズは `mode=0x0` = page scan も inquiry scan も止まる。
この状態では PC からは繋がらない。**

**[逆アセ]** `app_ibrt_ui_judge_scan_type` (`0x0c051195`、宣言は
`services/ibrt_ui/inc/app_ibrt_ui.h:619`) は
`app_ibrt_ui_inqscan_enable_needed()` (`:618`) の結果を bit0、
`app_ibrt_ui_pagescan_enable_needed(trigger)` (`:617`) の結果を bit1 として
`orr r5,r5,#2` (`0x0c0511ae`) で合成し、`j_scan=...` を出したうえで
`b.w app_tws_ibrt_set_access_mode` (`0x0c05122c` → `0x0c059acc`) に**末尾ジャンプ**する。
すなわち **UI イベントのたびに access mode は再計算されて上書きされる。**

#### 13.4.2 使う API

`app_bt_accessmode_set()` は **IBRT ビルドでは何もしない** ——
`services/bt_app/app_bt.cpp:271` の実体は `:275-277` が
`#if defined(IBRT) return; #endif` で即 return する。`app_set_accessmode()`
(`services/bt_app/app_bt_func.cpp:71`) も本体が `#if !defined(IBRT)` (`:72`) の中である。

使うのは **`bt_status_t app_tws_ibrt_set_access_mode(btif_accessible_mode_t)`**
(`services/ibrt_core/inc/app_tws_ibrt.h:310`)。SDK 内の前例は
`services/app_ibrt/src/app_ibrt_search_pair_ui.cpp:243`
(`app_tws_ibrt_set_access_mode(BTIF_BAM_CONNECTABLE_ONLY);` — コメント
「after change the bd_addr, we should reset access mode again」) である。
現在値は `ibrt_ctrl_t.access_mode` (`app_tws_ibrt.h:230`)、
送信中フラグは `access_mode_sending` (`:231`) で読める
(`ibrt_ctrl_t.access_mode` を直接読む前例は
`services/app_ibrt/src/app_ibrt_customif_ui.cpp:583`、`apps/main/apps.cpp:1411`、
および Run 4 ログの `checker: nv_role:0 current_role:0 access_mode:3` を出している
`services/app_ibrt/src/app_ibrt_if.cpp:592-594`)。

#### 13.4.3 決定 — 有効化は「`[mpi] peer ok` の直後」

```c
static int s_scan_forced;   /* glue がここを 1 にする */

void spp_log_enable_connectable(void) { s_scan_forced = 1; }

static void spp_log_rearm_access_mode(void)   /* ログスレッドから 200 ms 周期 */
{
    ibrt_ctrl_t *c;
    if (!s_scan_forced) return;
    c = app_tws_ibrt_get_bt_ctrl_ctx();               /* app_tws_ibrt.h:295 */
    if (c->access_mode_sending) return;               /* :231 */
    if (c->access_mode != BTIF_BAM_CONNECTABLE_ONLY)  /* :230 */
        app_tws_ibrt_set_access_mode(BTIF_BAM_CONNECTABLE_ONLY);
}
```

`mpi_ibrt_glue.cpp` からの呼び出しは **§11.2.5 のステップ 4 (`[mpi] peer ok` を出した直後)、
M-T1 の前**とする。比較したのは次の 3 案:

| 案 | 有効化点 | 却下/採用の理由 |
|---|---|---|
| A | `spp_log_service_init()` と同時 (BT スタック起動直後) | **却下**。§11.2.3 の in-case TWS 確立は `OPEN_BOX → mode=0x2 → CONNECT_TWS` という access mode 遷移そのものを使う。ここに割り込むと Run 4 で通った経路を変えてしまう |
| B | GEMM 開始直前 | **却下**。PC 側の RFCOMM 接続確立には秒オーダーかかるので、計算中に接続済みである保証が得られない |
| **C** | **`[mpi] peer ok` 直後 (採用)** | TWS 確立が完了した後なので §11.2 の経路を 1 バイトも変えない。M-T1 の RTT 掃引 (数百 ms〜秒) と GEMM の間に接続が間に合う可能性が高く、間に合わなくてもログはリングに残って接続後に流れる |

**discoverable は不要。** 既存ボンド前提なら `BTIF_BAM_CONNECTABLE_ONLY` (page scan のみ) で
再接続できるはずである。新規ペアリングが必要になった場合に限り、
一時的に `BTIF_BAM_GENERAL_ACCESSIBLE` (0x03) にする手動手順を用意する (13.7-手順 0)。
(実装: `spp_log_service.cpp` の再アーム値はマクロ `SPP_LOG_ACCESS_MODE`。
`make ... SPP_LOG_PAIRING=1` で `install-into-sdk.sh` 手順 11 が Makefile に足した ifeq が
`GENERAL_ACCESSIBLE` に切り替える。§13.13-4 で実測済み)
ただし「既存ボンドのデバイスに Windows が発信 COM ポートを作れるか」は 13.2.1 のとおり
未確証なので **実機確認項目 (13.11-2)**。

再アームが要る理由は 13.4.1 のとおり `judge_scan_type` が UI イベントのたびに
上書きするため。SDK 側には重複設定を弾くフィルタがある
(`ibrt_ui_log:filter access mode=%d, current access mode=%d`、Run 4 ログ 827 行目) ので、
200 ms ごとに条件付きで呼んでも HCI コマンドは実際には飛ばない。

---

### 13.5 IBRT との干渉

#### 13.5.1 モバイル ACL が張られたときに SDK が何をするか

オープンソース側の入口は
`services/app_ibrt/src/app_ibrt_customif_ui.cpp:117`
`app_ibrt_customif_ui_global_handler_ind(link_type, evt_type, status)`。
着信 ACL の分岐は `:139-146`:

```c
case BTIF_BTEVENT_LINK_CONNECT_IND:
  if (MOBILE_LINK == link_type) {
    if (BTIF_BEC_NO_ERROR == status) {
      app_status_indication_set(APP_STATUS_INDICATION_CONNECTED);
      app_tws_if_mobile_connected_handler(p_ibrt_ctrl->mobile_addr.address);
    }
  }
```

`app_tws_if_mobile_connected_handler()` (`services/app_tws/src/app_tws_if.cpp:397-408`) は
`IBRT_MASTER` のとき `app_tws_if_sync_info(TWS_SYNC_USER_GFPS_INFO)` を呼ぶだけで、
**ロールスワップもリンク共有起動も行わない**。登録側の
`app_ibrt_customif_mobile_connected_ind()` (`:401-407`) も
`app_ibrt_if_config_keeper_mobile_update(addr)` だけである。

一方、**実際の役割スイッチとリンク共有起動は閉じた `app_ibrt_ui.o` の中で走る。**
状態遷移の語彙は `services/ibrt_ui/inc/app_ibrt_ui.h` に公開されており:

```
IBRT_UI_W4_MOBILE_CONNECTION      (:212)
IBRT_UI_W4_MOBILE_MSS_COMPLETE    (:213)   ← 携帯との master/slave switch
IBRT_UI_W4_SET_ENV_COMPLETE       (:214)
IBRT_UI_W4_MOBILE_ENTER_ACTIVE_MODE (:215)
IBRT_UI_W4_START_IBRT_COMPLETE    (:216)   ← IBRT リンク共有の起動
IBRT_UI_W4_IBRT_DATA_EXCHANGE_COMPLETE (:217)
```

対応するアクションは `:245-251` (`IBRT_ACTION_MOBILE_CONNECT` …
`IBRT_ACTION_START_IBRT`, `IBRT_ACTION_TWS_SWITCH`)。
関連関数は `app_ibrt_ui_judge_ibrt_role()` (`:584`)、
`app_ibrt_ui_ibrt_start_needed()` (`:572`)、
`app_tws_ibrt_do_mss_with_mobile()` (`app_tws_ibrt.h:311`) で、
いずれもオープンソース側に呼び出し元は無い。

⇒ **モバイル ACL が右バッズ (rank=0 / nv_role=MASTER) に張られると、
IBRT の SM は MSS → IBRT 開始のシーケンスを回そうとする。**
その過程で TWS ロールスワップが起きうる。これが MPI にとっての最大の干渉源である。

#### 13.5.2 実験パッチ済み状態 (§11.2.4 + §12.8) での予想挙動

現行ファームは「箱イベントを注入する経路が glue の強制 OUT_BOX だけ」になっており
(§12.8.3)、`app_ibrt_ui` の SM そのものは生きている。よって:

1. SPP 接続は **RFCOMM (L2CAP) チャネル 1 本**であり、A2DP/HFP の profile 接続ではない。
   `IBRT_UI_W4_MOBILE_CONNECTION` 以降が回っても、共有すべき profile が無いので
   `IBRT_ACTION_START_IBRT` が実質的に何もしない可能性が高い
2. しかし **MSS (`IBRT_UI_W4_MOBILE_MSS_COMPLETE`) は ACL 単体でも起こりうる**。
   MSS 自体は TWS リンクを切らないが、`current_role` が変わると
   §11.2.2 で「rank は左右ストラップから確定する」ようにした判断は影響を受けない
   (`app_tws_is_right_side()` は物理ストラップ) ため、**rank は壊れない**
3. ロールスワップ中は `tws_ctrl_send_cmd(0x8201)` が失敗しうる。
   MPI 実行中に SPP 接続が確立すると `[mpi] frames err` が増える可能性がある
   ⇒ **13.9 の合否条件で「MPI 2 ランク PASS が SPP 併用でも維持される」を必ず見る**
4. SPP 接続は MPI 完了後に確立する運用 (13.4.3 案 C) なら 3 のリスクはほぼ消える。
   v1 はこれを既定とする

#### 13.5.3 観測ポイント (UART で見る行)

| 行 | 意味 | 出所 |
|---|---|---|
| `[spplog] connected` / `disconnected` | 自前のコールバック | 13.3.4 |
| `ibrt_ui_log:set_access_mode=2` + `write_scan_enable=2` | 再アームが効いた | 13.4 |
| `ibrt_ui_log:filter access mode=2, current access mode=2` | 既に 2 なので HCI は飛ばない (正常) | `app_ibrt_ui_global_handler` |
| `BTIF_BTEVENT_LINK_CONNECT_IND` / `twsif_mobile_connected` | モバイル ACL が張られた | `app_ibrt_customif_ui.cpp:139`, `app_tws_if.cpp:398` |
| `ibrt_ui_log:judge role,local_role=…,peer_role=…` | SM がロール判定に入った | `app_ibrt_ui.o` |
| `ibrt_ui_log:tws switch callback,local_role=…` | ロールスワップが起きた ← **要注視** | `app_ibrt_ui.o` |
| `tws cmd send failed, tws link missing` | MPI が線に出せていない ← **合否条件 4** | §2.3 |

---

### 13.6 ワイヤフォーマット

**v1 は素のテキスト行。フレーミングもチェックサムも付けない。**
理由は (a) 受信側が COM ポートを `readline` するだけで済む、
(b) RFCOMM は L2CAP/ACL の ARQ 上にあるので順序保証・無損失であり
(§2.6 と同じ理屈)、線上の破損を検出する層は要らない、
(c) 欠落は**バッファ溢れでしか起きない**ので、それだけ検出できればよい。

```
#<seq> <UART と同一の本文>\n
```

- `seq` は `compute_log_tap` が push 時に採番する単調増加の 10 進数 (13.3.3)。
  **UART 側には出さない** (既存出力を 1 バイトも変えない)
- 欠落検出は **seq の飛び**で行う。番号は push 時に採るので、
  リング溢れで捨てられた行の番号は線に出ない ⇒ `#12` の次が `#47` なら 34 行欠落
- 補助として、drop が発生した後の最初のチャンクで
  `#- [log] dropped=<行数> contended=<行数>\n` を単独で送る (13.3.4)。
  `contended` は try-lock に失敗して捨てた行数 (13.3.3)
- 改行は `\n` のみ。UART 側の `\r\n` (`hal_trace.c:1119-1123`) とは異なる
- 文字コードは ASCII のみ

---

### 13.7 Windows 受信側

#### 13.7.1 人間が 1 回だけやる操作列

0. (新規ペアリングが必要な場合のみ) バッズを一度ケースから出してペアリングモードにするか、
   13.4.3 のとおり一時的に `BTIF_BAM_GENERAL_ACCESSIBLE` にしたファームを焼く
1. 設定 → Bluetooth とデバイス → デバイス → **その他の Bluetooth 設定**
2. **COM ポート**タブ → 追加 → **発信 (Outgoing)** → デバイスに PineBuds Pro を選択 →
   サービスに `PineBudsLog` (13.2.2 の ServiceName) を選択
3. 割り当てられた `COMn` を控える
4. 拾えない場合: デバイスを一度「デバイスの削除」してから再ペアリングし、2 をやり直す
   (13.2.1 のとおり、既存ボンドに後から足したサービスを Windows が拾うかは未確証)

> **実測 (2026-09-01, §13.13):** 手順 0 は「`SPP_LOG_PAIRING=1` で焼いた両バッズをケース内で
> 同時起動 → `peer ok` 後に `aMode=0x3` → Windows の「デバイスの追加」」で成立した。
> ペアリングと同時に Windows が SDP の SerialPort サービスから **COM ポートを自動生成**するので
> 手順 1〜3 は不要だった: `COM6` = 発信 (`{00001101-…}` → `202211338768`)、`COM3` = 着信。
> `HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM` と `Get-PnpDevice -Class Ports` の InstanceId
> (`BTHENUM\{00001101-…}_LOCALMFG&001D\…&202211338768_C00000000`) で見分ける。

#### 13.7.2 受信スクリプト

Windows 側 (WSL には Bluetooth が無い) で `pyserial` を使う。置き場所は
`C:\Users\Ryuto\pinebuds-logs\` とし、WSL からは `/mnt/c/Users/Ryuto/pinebuds-logs/` で読む。

```python
# C:\Users\Ryuto\pinebuds-logs\spp_tail.py   (Windows の python で実行)
import serial, sys, time, pathlib

port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
out  = pathlib.Path(r"C:\Users\Ryuto\pinebuds-logs") / time.strftime("run-%Y%m%d-%H%M%S.log")

# 発信 COM ポートは open した瞬間に RFCOMM 接続を張る。
# timeout を必ず指定する (未指定だと readline が永久にブロックしうる)。
with serial.Serial(port, 115200, timeout=1) as s, out.open("w", buffering=1) as f:
    prev = None
    for raw in iter(s.readline, b""):
        line = raw.decode("ascii", "replace").rstrip("\r\n")
        if not line:
            continue
        if line.startswith("#") and not line.startswith("#-"):
            n = int(line[1:].split(" ", 1)[0])
            if prev is not None and n != prev + 1:
                f.write(f"!! GAP {prev} -> {n}\n")
            prev = n
        f.write(line + "\n")
        print(line)
```

ボーレートは仮想 COM では意味を持たない (RFCOMM は自前でフロー制御する) が、
`pyserial` は引数を要求するので任意の値を渡す。

WSL 側からは `tail -f /mnt/c/Users/Ryuto/pinebuds-logs/run-*.log` で追う。
TCP ブリッジは、行レートが数十行/実行しかないので **v1 では不要**と判断する。

---

### 13.8 install スクリプトのフック追加 (手順 9)

`scripts/install-into-sdk.sh` に §12.4/§12.8 と同じマーカー機構でもう 1 つ追加する。
マーカーは `pine-buds-cluster SPP log`。対象は
`services/bt_app/besmain.cpp` の 1 箇所だけ。

```
# 対象: besmain.cpp:451 — SPP ログサービスを BesbtThread のイベントループ突入前に登録する
-  osapi_notify_evm();
+  /* pine-buds-cluster SPP log */
+  spp_log_service_init();
+  osapi_notify_evm();
```

加えて、ファイル先頭 (`besmain()` の定義より前) に
`extern "C" void spp_log_service_init(void); /* pine-buds-cluster SPP log */` を挿入する。

- `osapi_notify_evm();` は同ファイル内で **1 回しか出現しない**ことを確認済み
  (grep で 1 件)。既存フックと同じく `str.replace` の前に出現数を assert する
- ここが正しい登録点である根拠: SDK 自身が同じ位置 (`besmain.cpp:448-449`) で
  `#ifdef TEST_OVER_THE_AIR_ENANBLED` → `app_tota_init()` を呼んでおり、
  `app_tota_init()` は `services/tota/app_tota.cpp:74` で `app_spp_tota_init()` を呼ぶ。
  つまり **SPP サービス登録は `BesbtThread` 上で、イベントループに入る直前に行う**のが
  SDK の作法である
- コピー対象に `firmware/pinebuds_compute/{log_ring.h,log_ring.cpp,spp_log_service.cpp}` を
  追加する (手順 4 と同じ `apps/main/` へのフラットコピー)。
  `Makefile:889` の `--whole-archive` により、`services/bt_app/built-in.a` から
  `apps/built-in.a` 内のシンボルを参照しても解決される

---

### 13.9 合否条件 (§11.3 形式)

右バッズ UART に §11.3 の既存 4 条件 + §12.6 の 2 条件が出たうえで、

```
[spplog] connected
ibrt_ui_log:set_access_mode=2
write_scan_enable=2
```

が出ること。Windows 側 `run-*.log` に

```
#1 [ctor] GlobalProbe constructed
...
#<n> GEMM-MPI N=32 rank=0 size=2 checksum=32768.000000 expect=32768.000000 PASS
#<n+1> GEMM-MPI elapsed=<m> ms frames tx=2 rx=4 err=0
#<n+2> [mpi] finalize done rank=0
```

が出ること。合格条件は 7 つ:

1. **SPP 接続確立** — UART に `[spplog] connected` が出て、Windows 側で COM ポートが
   open できる
2. **内容一致** — Windows 側の行 (先頭 `#<seq> ` を除去したもの) が、UART 側の
   `COMPUTE_TRACE` 由来の行と**完全に同一の集合**である。
   SDK 自身の TRACE 行 (`ibrt_ui_log:` など) は 1 行も混ざらない
3. **checksum 一致** — `checksum=32768.000000` が SPP 側でも厳密一致
4. **drop 0** — Windows 側の `!! GAP` が 0 回、`#- [log] dropped=` が 0 回
5. **MPI 回帰** — MPI cmdcode `0x8201` の `tws cmd send failed` が左右とも 0 回、
   両側 `size=2`、`[mpi] finalize done` が両側に出る (§11.3 と同一条件を維持)
6. **タイミング非退行** — `GEMM-MPI elapsed` が Run 4 の 13 ms に対して
   **20 ms 以内**に収まる (SPP 送信が計算を乱していない)
7. **到達遅延** — バッズが行を出してから Windows 側ファイルに現れるまでの目安が
   **1 秒以内**。ログスレッドの周期 200 ms + RFCOMM の sniff 復帰を見込んだ値。
   厳密な計測はしない (13.11-6)

条件 2 の照合は、UART ログを `tr -d '\0' | tr '\r' '\n'` で正規化したうえで
`COMPUTE_TRACE` 由来の行だけを抜き、SPP ログ側の `#<seq> ` を剥がして `diff` する。

---

### 13.10 リスク

1. **帯域とタイミング摂動。** ログスレッドは `osPriorityLow` なので `compute` スレッド
   (osPriorityBelowNormal) を止めないが、`btif_spp_write` の内部は `BesbtThread` の
   仕事を増やす。§2.5 のとおり MPI の送受信ポンプも `BesbtThread` にあるため、
   **SPP のトラフィックは MPI のスループットを直接削る**。
   v1 で SPP 接続を MPI 完了後に確立する運用 (13.4.3 案 C) にしたのはこのため。
   合否条件 6 で退行を検出する
2. **ボンド / リンクキー。** ペアリング済みと言っても、どのファーム時点のボンドかが不明。
   §12.4 のパッチでバッズの BD アドレスは変えていないので既存ボンドは生きているはずだが、
   `app_ibrt_config_the_same_bd_addr()` (`app_ibrt_search_pair_ui.cpp:223-243`) が
   走る条件に入ると BD アドレスが書き換わる。その場合は Windows 側で削除→再ペアリングが必要
3. **charging-boot との相互作用。** §12.4 で `CHARGER_PLUGINOUT_RESET=0` にし、
   §12.8 で充電器由来の箱イベントを殺してある。SPP 接続はこれらの経路を使わないので
   新たな相互作用は想定していないが、**モバイル ACL が張られると
   `apps.cpp:1508` 周辺の「モバイル未接続なら自動電源断」の条件が変わる**。
   §11.2.4 対象 2 で自動電源断自体を殺してあるので実害は無い
4. **将来の A2DP 同時動作。** 現在は SPP 単独の ACL しか張らない前提だが、
   実際に音楽を再生しながら計算する段階では A2DP + SPP + TWS + MPI が同居する。
   そのとき `app_ibrt_ui_judge_scan_type` の `IBRT_A2DP_PLAYING_TRIGGER`
   (`app_ibrt_ui.h:178`) で page scan パラメータが変わり、
   13.4.3 の再アームと競合する可能性がある。**v1 では A2DP を使わない**ことを手順書に明記する
5. **`vsnprintf` の 2 度呼び。** UART の TRACE と tap で同じ書式を 2 回整形する。
   1 行あたりの CPU コストが倍になるが、`hal_trace.c:1113-1118` と同じ処理を
   もう 1 回するだけであり、GEMM 1 回あたりのログは十数行しかない。
   `%f` を使わない制約 (13.3.1) を破るとここが一気に重くなる
6. **`spp_device` を直接触る。** `s_dev->portType` / `app_id` /
   `spp_handle_data_event_func` / `creditMutex` への代入は
   `spp_api.h:87-109` の公開構造体に対する操作であり、TOTA (`app_spp_tota.cpp:345-347`) と
   `app_rfcomm_mgr.cpp:190-214` が同じことをしている。ただし
   `services/bt_if_enhanced/lib/*.a` はソース非公開なので、
   ライブラリ差し替えで破綻する点は §11.4 リスク 1 と同じ性質を持つ

---

### 13.11 実機確認項目 (ホストテスト対象外)

いずれもホストでは検証できない。`docs/manual.md` の手動確認手順に落とす。

1. 既にペアリング済みのデバイスに後から SPP サービスを足したとき、Windows の
   COM ポートタブが再探索するか。しないなら「デバイスの削除 → 再ペアリング」が要るか
2. `BTIF_BAM_CONNECTABLE_ONLY` (discoverable なし) の状態で、
   Windows から発信 COM ポートを開いて接続できるか。できなければ
   一時的に `BTIF_BAM_GENERAL_ACCESSIBLE` にする必要がある
3. SDP レコードが Windows 側で `PineBudsLog` という名前で見えるか
   (ServiceName 属性が正しく読まれているか)
4. RFCOMM チャネル 19 (`RFCOMM_CHANNEL_10`) が実際に払い出されるか
   (`btif_spp_get_server_channel()` の戻り値を UART に出して確認する)
5. `BTIF_SPP_EVENT_DATA_SENT` の `tx_data_length` が、渡した長さと常に一致するか
   (部分送信が起きるか)。起きるなら `log_ring_commit` の部分確定パスが実際に走る
6. バッズが行を出してから Windows 側に現れるまでの実測遅延 (合否条件 7 の裏取り)
7. SPP 接続中に TWS ロールスワップ (`ibrt_ui_log:tws switch callback`) が起きるか
8. SPP 接続確立が MPI 実行中に起きた場合、`[mpi] frames err` が増えるか
9. `GEMM-MPI elapsed` が SPP 併用で何 ms になるか (合否条件 6)
10. 左バッズ (rank=1) も同時に SPP 接続した場合の挙動 (v1 の範囲外だが、
    Windows が 2 台とも COM ポートを持てるかだけ見ておく)

実測で答えが出たもの (2026-09-01, §13.13): **1** = 既存ボンドのまま COM ポート追加は可能だった
(Run 9 前、`BusReportedDeviceDesc=PineBudsLog`) が、SDP キャッシュが古いチャネルを指し続けるので
サービス変更後は削除→再ペアリングが確実。**2** = ボンドがあれば `CONNECTABLE_ONLY` で Windows
から再接続できる (Run 9 で A2DP が MOBILE_LINK で戻ってきた)。ボンドを消すと discoverable が要る
(§13.13-4)。**3** = 見える (`PineBudsLog`)。**4** = 払い出されない (`open=1`)。チャネル 12 に変更。
**5** = 未確定。初回チャンクが丸ごと再送された (§13.13-6) ので、初回 `DATA_SENT` の
`tx_data_length` が 0 だった可能性あり。**9** = 13 ms (合否条件 6 内)。

---

### 13.12 テストリスト (TDD / t-wada スタイル / Red から始める)

対象は `log_ring` の純ロジックと、そこから切り出した送信ステートマシンの
チャンク計算だけである。SPP API・SDP・COM ポート・遅延・access mode は
**ホストテストの対象外**とし、13.11 と 13.9 に置く。

新規テストファイルは **`tests/test_log_ring.cpp`**。既存 5 スイートと同じ形で
`tests/test_framework.h` を使う。

#### リングバッファ (`log_ring`)

- [ ] **R1 初期状態**: `log_ring_init` 後、`log_ring_used()==0`、
      `log_ring_peek()` が 0 を返す、`log_ring_take_dropped()==0`
- [ ] **R2 1 行の往復**: `push("abc",3)` → `peek` が `"abc\n"` の 4 バイトを返し、
      `base` が 0 であること
- [ ] **R3 消費は commit まで起きない**: R2 の直後にもう一度 `peek` すると
      **同じ 4 バイトが同じ base で返る** (peek は非破壊)
- [ ] **R4 commit で進む**: `commit(base,4)` 後に `peek` が 0 を返し、`used()==0`
- [ ] **R5 部分 commit**: 8 バイト分 push → `peek` で 8 バイト取得 → `commit(base,3)` →
      次の `peek` が残り 5 バイトを `base+3` から返す
- [ ] **R6 二重 commit は no-op**: `commit(base,4)` を 2 回呼んでも `used()` が負に回らない
- [ ] **R7 古い commit は無視される**: `commit(base,4)` の後に `commit(base,2)` を呼んでも
      `free_pos` が戻らない
- [ ] **R8 peek の上限**: 100 バイト積んだ状態で `peek(dst,16,&base)` が 16 を返す
- [ ] **R9 折返し**: `LOG_RING_CAPACITY` の境界をまたぐように push/commit を繰り返し、
      取り出した全バイト列が push した全バイト列と一致すること
- [ ] **R10 満杯時に drop-oldest**: 容量いっぱいまで行を積み、さらに 1 行 push すると
      **最古の 1 行が丸ごと消え**、`take_dropped()==1` になること
- [ ] **R11 drop は行単位**: R10 の後、`peek` で取れる先頭が
      「2 番目に古い行の先頭」であること (行の途中から始まらない)
- [ ] **R12 複数行 drop**: 1 行で複数行分の空きが要る場合、必要な数だけ drop され
      `take_dropped()` がその行数を返すこと
- [ ] **R13 take_dropped はクリアする**: 2 回目の `take_dropped()` が 0 を返す
- [ ] **R14 容量超の行は拒否**: `LOG_RING_CAPACITY` 以上の長さの行を push すると
      0 を返し、**既存の内容を 1 バイトも壊さず**、`take_dropped()` が 1 増える
- [ ] **R15 in-flight 中の drop と commit の整合**: `peek(base)` した直後に
      リングを溢れさせて `free_pos` が `base+n` を追い越す状況を作り、
      その後 `commit(base,n)` を呼んでも `free_pos` が**戻らない**こと (R7 の一般形)
- [ ] **R16 空 push**: `push(line,0)` は `"\n"` 1 バイトを積む (空行として扱う)

#### 送信ステートマシンのチャンク計算

送信ロジックのうち、SDK に触らない部分を
`log_ring_next_chunk(ring, dropped, contended, dst, max, &base, &consumes_ring)`
という純関数に切り出し、`spp_log_thread_body` はこれを呼ぶだけにする。

- [ ] **S1 通常チャンク**: drop なし・リングにデータありのとき、
      リング内容がそのまま `dst` に入り `consumes_ring==1`、`base` が正しい
- [ ] **S2 drop マーカは単独チャンク**: `dropped>0` のとき、`dst` は
      `"#- [log] dropped=<n> contended=<m>\n"` **だけ**になり、
      `consumes_ring==0`、`base` は使われない
- [ ] **S3 マーカの次は通常チャンク**: S2 の直後にもう一度呼ぶと S1 に戻る
      (`dropped` が消費済み)
- [ ] **S4 空リング**: drop なし・リング空のとき 0 を返す
- [ ] **S5 チャンク上限**: `max` を超えるデータがあっても戻り値が `max` 以下

計 21 件。実機でしか確認できない項目 (SDP / COM ポート / 遅延 / access mode /
IBRT 干渉) は 13.11 に 10 件、合否条件は 13.9 に 7 件として分離した。

#### Makefile への組み込み

既存 5 スイートの並びにそのまま足す。

```make
RINGBIN  := $(BUILDDIR)/test_log_ring
RINGSRC  := firmware/pinebuds_compute/log_ring.cpp

test: $(TESTBIN) $(MPIBIN) $(OMPBIN) $(BENCHBIN) $(FRAGBIN) $(RINGBIN) check98
	...
	./$(RINGBIN)

$(RINGBIN): tests/test_log_ring.cpp $(RINGSRC) firmware/pinebuds_compute/log_ring.h \
            tests/test_framework.h
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -Ifirmware/pinebuds_compute -Itests \
		tests/test_log_ring.cpp $(RINGSRC) -o $@
```

`check98` のコンパイル対象にも `firmware/pinebuds_compute/log_ring.cpp` を足す。
`TARGET_DIALECT` は既に `-Ifirmware/pinebuds_compute` を持っているので追加の
include パスは要らない。`spp_log_service.cpp` は SDK ヘッダを使うため
**ホストではビルドしない** (`mpi_ibrt_glue.cpp` と同じ扱い)。

### 13.13 実機実測 (Run 7〜11, 2026-09-01)

UART キャプチャは `~/.claude/handover/artifacts/2026-09-01-run7-spp-diag/` (Run 7〜9) と
Run 10/11 のスクラッチパッド。Windows 側の受信ファイルは
`C:\Users\Ryuto\pinebuds-logs\run-20260901-134307.log`。

1. **スロット (Run 7/8 → 9)。** チャネル 19 / ID_10 は `open=1` (§13.1.2 の注記)。
   `RFCOMM_CHANNEL_3` / `SERVER_ID_3` に変更した Run 9 で両側 `open=0`。`portType` を
   `service_setup` の前に設定する順序変更 (SDK の `app_spp_open` と同順) は単独では効果なし。
2. **再アームは効く (Run 9)。** `[mpi] peer ok` 直後に `set_access_mode=2, ca=0xc08d1df`
   (再アーム) → `[BTEVENT] ACCESSIBLE_CHANGE aMode=0x2` → Windows が A2DP を
   `j_scan=CONNECTED, l_type=MOBILE_LINK` で再接続してきた。以後 SDK が `mode=0x0` に戻すたび
   再アームが 2 を再主張する (7 回)。
3. **満充電シャットダウン。** `FULL_CHARGING:42xx → app_shutdown → system_shutdown!!` が
   毎起動 15〜60 秒で来る (Run 9: `peer ok` の 15 秒後)。音楽再生では減らない。
   `install-into-sdk.sh` 手順 10 で `apps/battery/app_battery.cpp` の `app_shutdown()` を
   `if (0)` 化。パッチ後は `FULL_CHARGING:4227` / `EXT PIN-->FULL_CHARGING` が出ても走り続ける
   (左で 20 分以上観測)。副作用: シャットダウン保留が無くなるため「出して 3 秒待って戻す」で
   **再起動しないことがある** (右: 充電端子の `PLUGIN` だけ出て `CHARGING PWRON` 無し。
   左は同じ操作で再起動した)。原因未特定。書き込み前の再起動手順に影響するので要観察。
4. **ペアリングし直し。** Windows でデバイスを削除すると再ペアリングできなくなる: バッズは
   PC のリンクキーを持ったまま page するだけで inquiry scan を出さず、再アームも 0x2 に固定する。
   純正の「ケース外でペアリングモード」は充電起動バッズが取り出しで落ちるので使えない。
   手順 11 の `SPP_LOG_PAIRING=1` ビルド (逆アセで `cmp r3,#3 / movs r0,#3` を確認) を
   焼くと `peer ok` 後に `aMode=0x3`、Windows の「デバイスの追加」で `PineBuds Pro`
   (`DEV_202211338768`) がペアリングでき、COM3/COM6 が自動生成された (§13.7.1 の注記)。
   discoverable になるのは `peer ok` 後だけなので、**左右同時起動で TWS が組めること**が前提。
5. **受信成功 (Run 11, 13:43)。** `spp_tail.py COM6` を開くと 1 秒強で RFCOMM が張られ、
   右バッズ (rank 0) のリング全量 `#0`〜`#25` が届いた:
   `#23 GEMM-MPI N=32 rank=0 size=2 checksum=32768.000000 expect=32768.000000 PASS`、
   `#24 GEMM-MPI elapsed=13 ms frames tx=2 rx=4 err=0`、`#25 [mpi] finalize done rank=0`。
   COM6 を閉じて再オープンすると 1.3 秒で接続・受信 0 バイト (リングは commit 済み)。
6. **初回チャンクの重複。** 受信ファイルの先頭 12 行 (`#0`〜`#11 [mpi-t1]` の途中、≈512 B =
   `kSppLogChunk` 1 個分) の直後に `#0` から全量が再送され、`spp_tail.py` が
   `!! GAP 11 -> 1` を記録した。以降は重複も欠落も無い。commit されずに再送されたのは
   初回チャンクだけなので、候補は (a) Windows の COM オープン時に RFCOMM が一度切れて
   繋ぎ直され `CONNECTED` で `s_inflight=0` に戻った (再送は設計どおりの at-least-once)、
   (b) 初回 `DATA_SENT` の `tx_data_length` が 0 だった (§13.11-5)。右 UART が本 Run で
   無反応だったため `[spplog] connected` の回数で切り分けられなかった。**未解決。**
7. **§13.9 判定。** 1: Windows 側 ✓ / UART `[spplog] connected` は右 UART 不通で未観測。
   2: Windows 側の行は全て `COMPUTE_TRACE` 由来で SDK の TRACE は混入なし ✓、UART との
   集合比較は右 UART 不通で未実施。3 ✓。4: `#- [log] dropped=` 0 回 ✓、`!! GAP` 1 回
   (6 の重複由来、欠落ではない) △。5: 左 `cmd_code:8201` 失敗 0 回・`size=2`・
   `finalize done rank=1` ✓、右は SPP 側の行で `size=2`・`finalize done rank=0` ✓。
   6: 13 ms ✓。7: 未計測。
8. **その他の観測。** `[mpi-t1] probe len=64 以上 TIMEOUT`、`max_payload=4` (右) / `0` (左) は
   Run 9 以前から同じで GEMM-MPI は完走する。左の `[mpi] FAIL stalled in MPI op >5000 ms rank=1`
   は `peer ok` 直後のプローブ試験中の警告で旧ファームでも出ていた。退行ではない。
   右 UART (ACM0) は本 Run で受信がほぼ途絶 (bestool も boot sync は受かるが ack が届かない) —
   接触不良の再発。抜き差しで復活した前例あり。
9. **Windows 側の落とし穴。** WSL から `timeout N powershell.exe ... python spp_tail.py` を
   打つと WSL 側の powershell だけ死に **Windows の python.exe が COM を掴んだまま残る**
   (`PermissionError(13, ..., 5)` = `ERROR_ACCESS_DENIED`)。
   `Get-CimInstance Win32_Process | ? CommandLine -match spp_tail | Stop-Process` で回収する。
   COM オープン失敗の読み方: `FILE_NOT_FOUND` = Windows ローカル (ポート再作成)、
   `REM_NOT_LIST` = 相手不在 (バッズ電源断)、`ACCESS_DENIED` = 別プロセスが掴んでいる、
   open 成功 + `AT+BRSF` = HFP 着地 (サーバ不在 / SDP キャッシュが古い)。
10. **Run 12 (13:53, ユーザー操作)。** 両バッズ再起動後に Windows 側で `spp_tail.py COM6` を
    起動したところ、今度は**左バッズ (rank 1)** が応答し `#0`〜`#23` が**欠落・重複なし**で届いた
    (`run-20260901-135303.log`、`!! GAP` 0 回 — §13.9 条件 4 をこのランは満たす)。
    どちらのバッズが応答するかは Windows がどちらの page scan に当たるかで決まる (両側が同じ
    BD アドレスを広告)。同ランの `[mpi-t1]` は `probe len=512 ok` / `max_payload=512` /
    `rtt n=100 min=44 avg=169 max=407 ms` で、Run 11 (右, `max_payload=4`) より大幅に良い。
    初回チャンクの重複 (6) は再現せず。

---

## 14. 5 連タップで GEMM-MPI を再実行する (2026-09-01)

§13 までのランは起動時の一発実行だった (`compute_main()` → `mpi_compute_thread` → TWS 立ち上げ
→ 握手 → プローブ → M-T3 → `MPI_Finalize` → 永眠)。「イヤホンとして使いながら裏で計算する」
デモに向けて、**タッチ操作で何度でも再実行**できるようにする。

### 14.1 SDK 側の事実 (Explore 調査、2026-09-01)

- `open_source` ビルドにタッチ IC ドライバは無い。タッチパッドは BES2300YP **PMU の電源キー
  入力**として入り、ジェスチャ判定は `platform/hal/hal_key.c` がソフトで行う
  (クリック間隔 `CFG_SW_KEY_DBLCLICK_THRESH_MS` 400 ms、長押し 1500 ms、ポーリング 40 ms)。
  N 連クリックは `HAL_KEY_EVENT_CLICK + cnt_click` で合成される (`hal_key.c:928/:941`)
- 経路: PMU 割り込み → `hal_key_debounce_handler` (TIMER01 ISR) → `key_event_process`
  (`apps/key/app_key.cpp:37`) → `app_mailbox_put` → **`app_thread`** (osPriorityHigh、
  スタック 3 KB) → `app_key_handle_process` → 登録済みハンドラ
- このフォークのキーテーブルは `apps/main/key_handler.cpp:216-247` `app_key_init()` に
  あり、CLICK=再生/停止、DOUBLE/TRIPLE/ULTRA(4)/LONGPRESS が割り当て済み。
  **`APP_KEY_EVENT_RAMPAGECLICK` (5 連) は未割当**。`app_key_handle_registration()` は同じ
  (code, event) を上書きするので、単タップを奪わずに増やすには未割当イベントを使うしかない
- ハンドラは**タップされた側のバッズだけ**で 1 回走る。TWS 相手にはデコード済みの
  アクション (`IBRT_ACTION_PLAY` 等) しか送られず、生のキーイベントは転送されない
  (`app_ibrt_if_start_user_action`, `app_ibrt_keyboard.cpp:263`)
- 充電起動でも `app_key_open()` (`apps.cpp:2053`) と `app_key_init()` (`:2388`) は
  `compute_main()` (`:2454`) より前に走る (§12.4 手順 3/6)

### 14.2 設計

1. **純ロジック `run_trigger`** (`firmware/pinebuds_compute/run_trigger.{h,cpp}`、
   ホストテスト `tests/test_run_trigger.cpp` T1〜T8): 実行中は全入力を無視。idle で
   ローカルタップ → `START_NOTIFY` (seq+1、相手に通知)、idle で相手からの START →
   `START` (seq は max を採用)。TWS コマンドは送信元にエコーされないので seq による
   重複排除は不要。seq はログのラベル
2. **START フレーム**: 既存の cmdcode 0x8201 に kind 5 (`kKindStart`) を追加
   (`[kind, seq LE32]`、5 バイト)。受信は `mpi_ibrt_cmdhandler` (BesbtThread) で
   `run_trigger_on_peer_start` → `START` ならセマフォ `g_run_sem` を解放
3. **入口 `mpi_ibrt_trigger_run()`** (app_thread): `run_trigger_on_local_tap` →
   `START_NOTIFY` なら cmd channel が生きていれば START を送って自分のセマフォも解放。
   ブロックしない (mutex は数命令、`tws_ctrl_send_cmd` はキュー投入)
4. **compute スレッド**: 起動時ランはそのまま。`finalize done` の後、永眠の代わりに
   `g_run_sem` で待ち、起きたら `app_sysfreq_req(APP_SYSFREQ_USER_APP_4, 104M)` →
   `mpi_ibrt_install_seams` (MPI を再ブートストラップ、冪等) → M-T3 → `MPI_Finalize` →
   sysfreq を 32K に戻す → `[mpi] run #<seq> done rank=%d` → `run_trigger_on_run_done`。
   `APP_SYSFREQ_USER_APP_4` は SDK が `UNUSED` と明記しているスロット (`app_utils.h:29`)。
   起動時ランが終わるまで (`g_runs_enabled`) タップは無視する
5. **キー登録**: `install-into-sdk.sh` 手順 12 が `key_handler.cpp` に
   `{APP_KEY_CODE_PWR, APP_KEY_EVENT_RAMPAGECLICK} → app_key_compute_run()` を足す
6. **SPP ログ**: 各ランの行はリングに積まれ、COM6 が開いていればそのまま流れる。seq は
   起動からの通し番号なので `spp_tail.py` の GAP 検出はそのまま有効

### 14.3 既知の割り切り

- 相手の START が落ちる、または相手がまだ実行中で無視した場合、タップした側だけが走り
  MPI は §11 のストール検出 (5 s) 経由で FAIL 行を出して完走する。ラン中の連打は捨てる
- `mpi_frag_counters` は累積なので `frames tx/rx` はラン間で増え続ける
  (`install_seams` の `mpi_frag_init` でリセットされる場合はその限りでない — 実測で確認)
- ケース内でタッチパッドに触れるかは未確認 (単タップで再生/停止が効くかで判定できる)
