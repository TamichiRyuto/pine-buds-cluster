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
