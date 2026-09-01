#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build slides/lt-ear-parallel.pptx from the deck defined in slides/lt-ear-parallel.md.

Deliberately theme-less: blank layout, white ground, black text, grey rules.
Structure comes from shapes and text boxes, not from a PowerPoint design theme.
"""
import os
from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "slides", "lt-ear-parallel.pptx")

W, H = 13.333, 7.5
M = 0.55                      # page margin
CW = W - 2 * M                # content width
JP = "Meiryo"
MONO = "Consolas"

INK = RGBColor(0x11, 0x11, 0x11)
MUTED = RGBColor(0x5A, 0x5A, 0x5A)
FAINT = RGBColor(0x8C, 0x8C, 0x8C)
RULE = RGBColor(0xBF, 0xBF, 0xBF)
BOX = RGBColor(0x9E, 0x9E, 0x9E)
FILL = RGBColor(0xF4, 0xF4, 0xF4)
FILL2 = RGBColor(0xE8, 0xE8, 0xE8)
CODEBG = RGBColor(0xF0, 0xF0, 0xF0)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)
OK = RGBColor(0x1E, 0x6B, 0x3A)
NG = RGBColor(0xA3, 0x2B, 0x22)

prs = Presentation()
prs.slide_width, prs.slide_height = Inches(W), Inches(H)
BLANK = prs.slide_layouts[6]


# ---------------------------------------------------------------- primitives
def font(run, name=JP, size=16, bold=False, color=INK, italic=False):
    f = run.font
    f.name, f.size, f.bold, f.italic = name, Pt(size), bold, italic
    f.color.rgb = color
    rPr = run._r.get_or_add_rPr()
    for tag in ("a:ea", "a:cs"):
        el = rPr.find(qn(tag))
        if el is None:
            el = rPr.makeelement(qn(tag), {})
            rPr.append(el)
        el.set("typeface", name)


def tf_reset(tf, pad=0.06):
    tf.word_wrap = True
    tf.margin_left = tf.margin_right = Inches(pad)
    tf.margin_top = tf.margin_bottom = Inches(pad * 0.6)


def text(slide, x, y, w, h, lines, size=16, name=JP, color=INK, bold=False,
         align=PP_ALIGN.LEFT, anchor=MSO_ANCHOR.TOP, space=4, pad=0.06, lh=None):
    """lines: str | list of str | list of (str, dict-of-overrides)."""
    sh = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = sh.text_frame
    tf_reset(tf, pad)
    tf.vertical_anchor = anchor
    if isinstance(lines, str):
        lines = [lines]
    for i, item in enumerate(lines):
        ov = {}
        if isinstance(item, tuple):
            item, ov = item
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = ov.get("align", align)
        p.space_after = Pt(ov.get("space", space))
        if lh:
            p.line_spacing = lh
        if ov.get("indent"):
            p.level = ov["indent"]
        r = p.add_run()
        r.text = item
        font(r, ov.get("name", name), ov.get("size", size),
             ov.get("bold", bold), ov.get("color", color), ov.get("italic", False))
    return sh


def rect(slide, x, y, w, h, fill=None, line=BOX, lw=0.75, shape=MSO_SHAPE.RECTANGLE):
    sh = slide.shapes.add_shape(shape, Inches(x), Inches(y), Inches(w), Inches(h))
    sh.shadow.inherit = False
    if fill is None:
        sh.fill.background()
    else:
        sh.fill.solid()
        sh.fill.fore_color.rgb = fill
    if line is None:
        sh.line.fill.background()
    else:
        sh.line.color.rgb = line
        sh.line.width = Pt(lw)
    sh.text_frame.text = ""
    return sh


def panel(slide, x, y, w, h, lines, size=13, fill=FILL, line=BOX, head=None,
          head_size=14, color=INK, name=JP, pad=0.12, anchor=MSO_ANCHOR.TOP, space=4):
    rect(slide, x, y, w, h, fill, line)
    dy = 0
    if head:
        rect(slide, x, y, w, 0.42, FILL2, line)
        text(slide, x, y, w, 0.42, head, size=head_size, bold=True,
             anchor=MSO_ANCHOR.MIDDLE, pad=0.12)
        dy = 0.42
    if lines:
        text(slide, x, y + dy, w, h - dy, lines, size=size, color=color, name=name,
             pad=pad, anchor=anchor, space=space)


def code(slide, x, y, w, h, lines, size=10.5, head=None):
    rect(slide, x, y, w, h, CODEBG, RULE)
    dy = 0
    if head:
        text(slide, x, y + 0.04, w, 0.32, head, size=11, bold=True, color=MUTED, pad=0.12)
        dy = 0.34
    body = []
    for ln in lines:
        ov = {"space": 0, "name": MONO, "size": size}
        if isinstance(ln, tuple):
            ln, extra = ln
            ov.update(extra)
        body.append((ln, ov))
    text(slide, x, y + dy, w, h - dy, body, size=size, name=MONO, pad=0.12, space=0)


def arrow(slide, x, y, w, h, shape=MSO_SHAPE.LEFT_RIGHT_ARROW, label=None, size=12):
    sh = rect(slide, x, y, w, h, FILL2, BOX, shape=shape)
    if label:
        tf = sh.text_frame
        tf_reset(tf, 0.02)
        tf.vertical_anchor = MSO_ANCHOR.MIDDLE
        p = tf.paragraphs[0]
        p.alignment = PP_ALIGN.CENTER
        r = p.add_run()
        r.text = label
        font(r, JP, size, True, INK)
    return sh


def hline(slide, x, y, w, color=RULE, lw=1.0):
    ln = slide.shapes.add_connector(1, Inches(x), Inches(y), Inches(x + w), Inches(y))
    ln.line.color.rgb = color
    ln.line.width = Pt(lw)
    return ln


def plain_table(slide, x, y, w, h, data, widths=None, size=11.5, head_size=11.5,
                row_h=0.34, aligns=None, head_fill=FILL2):
    gf = slide.shapes.add_table(len(data), len(data[0]),
                                Inches(x), Inches(y), Inches(w), Inches(h))
    tbl = gf.table
    tblPr = tbl._tbl.tblPr
    for e in tblPr.findall(qn("a:tableStyleId")):
        tblPr.remove(e)
    sid = tblPr.makeelement(qn("a:tableStyleId"), {})
    sid.text = "{5940675A-B579-460E-94D1-54222C63F5DA}"   # No Style, Table Grid
    tblPr.append(sid)
    tblPr.set("firstRow", "0")
    tblPr.set("bandRow", "0")
    if widths:
        tot = sum(widths)
        for i, cw in enumerate(widths):
            tbl.columns[i].width = Emu(int(Inches(w) * cw / tot))
    for r in range(len(data)):
        tbl.rows[r].height = Inches(row_h)
    for r, row in enumerate(data):
        for c, val in enumerate(row):
            cell = tbl.cell(r, c)
            cell.margin_left = cell.margin_right = Inches(0.07)
            cell.margin_top = cell.margin_bottom = Inches(0.02)
            cell.vertical_anchor = MSO_ANCHOR.MIDDLE
            cell.fill.solid()
            cell.fill.fore_color.rgb = head_fill if r == 0 else WHITE
            tf = cell.text_frame
            tf.word_wrap = True
            ov = {}
            if isinstance(val, tuple):
                val, ov = val
            p = tf.paragraphs[0]
            p.alignment = ov.get("align",
                                 (aligns[c] if aligns else PP_ALIGN.LEFT))
            rn = p.add_run()
            rn.text = val
            font(rn, ov.get("name", JP), ov.get("size", head_size if r == 0 else size),
                 ov.get("bold", r == 0), ov.get("color", INK))
    return tbl


def slide(title=None, kicker=None, note=None):
    s = prs.slides.add_slide(BLANK)
    if title:
        text(s, M, 0.30, CW - 2.0, 0.62, title, size=26, bold=True,
             anchor=MSO_ANCHOR.MIDDLE, pad=0.0)
        if kicker:
            text(s, W - M - 3.4, 0.36, 3.4, 0.5, kicker, size=11.5, color=FAINT,
                 align=PP_ALIGN.RIGHT, anchor=MSO_ANCHOR.MIDDLE, pad=0.0)
        hline(s, M, 1.06, CW)
    if note:
        s.notes_slide.notes_text_frame.text = note
    return s


def pagenum(s, n):
    text(s, W - M - 1.2, H - 0.46, 1.2, 0.3, str(n), size=10, color=FAINT,
         align=PP_ALIGN.RIGHT, pad=0.0)


# ------------------------------------------------------------------ slide 1
s = slide(note="つかみ:「イヤホンはコンピュータである」。BES2300YP は Cortex-M4F が 2 個載った"
               "れっきとした MCU。音を鳴らしていない時間、CPU は暇をしている。")
rect(s, M, 2.35, CW, 0.06, INK, None)
text(s, M, 2.62, CW, 1.25, "耳で並列計算してみた", size=54, bold=True, pad=0.0)
text(s, M, 3.95, CW, 0.5, "夏休みの自由研究 — ワイヤレスイヤホン 2 個を MPI クラスタにする",
     size=20, color=MUTED, pad=0.0)
rect(s, M, 4.62, CW, 0.04, RULE, None)
text(s, M, 4.95, 7.0, 1.2,
     [("[ 発表者名 ] / [ 所属 ]", {"size": 15}),
      ("github.com/TamichiRyuto/pine-buds-cluster", {"size": 14, "name": MONO,
                                                     "color": MUTED})], pad=0.0)
text(s, W - M - 4.0, 4.95, 4.0, 0.6, "LT 8 分", size=15, color=MUTED,
     align=PP_ALIGN.RIGHT, pad=0.0)

# ------------------------------------------------------------------ slide 2
s = slide("自己紹介 & サマリ", "1 / 14",
          note="この 3 行と 6 個の数字だけ持ち帰ってもらえれば成功。"
               "「速くなった」とは一言も言っていないことに注意。")
panel(s, M, 1.30, 4.05, 2.55, [
    ("名前:    [                    ]", {"space": 10}),
    ("所属:    [                    ]", {"space": 10}),
    ("普段:    [                    ]", {"space": 10}),
    ("GitHub:  TamichiRyuto", {"space": 0}),
], size=14, head="発表者")
panel(s, 4.90, 1.30, CW - 4.35, 2.55, [
    ("1.  左右のワイヤレスイヤホン 2 個を、2 ノードの計算クラスタにした", {"space": 9}),
    ("2.  MPI のサブセットを自作し、イヤホン同士の Bluetooth リンク (IBRT) に載せた", {"space": 9}),
    ("3.  標準 MPI + OpenMP で書かれた GEMM ベンチを、1 行も変えずに実機で PASS させた", {"space": 0}),
], size=15, head="3 行サマリ")

tiles = [
    ("2 ノード", "右バッズ = rank 0 / 左バッズ = rank 1"),
    ("checksum = 32768", "float32 で厳密一致 → PASS"),
    ("12 – 13 ms", "GEMM N=32 を 2 ノードで実行"),
    ("1,788 checks", "実機に焼く前のホストテスト"),
    ("5 連タップ", "耳を叩くと両バッズで再実行"),
    ("78 commits / 2 週間", "2026-08-18 → 09-01"),
]
tw, gap = (CW - 2 * 0.22) / 3, 0.22
for i, (big, small) in enumerate(tiles):
    tx = M + (i % 3) * (tw + gap)
    ty = 4.16 + (i // 3) * 1.30
    rect(s, tx, ty, tw, 1.14, FILL, BOX)
    text(s, tx, ty + 0.10, tw, 0.52, big, size=19, bold=True, align=PP_ALIGN.CENTER, pad=0.05)
    text(s, tx, ty + 0.63, tw, 0.44, small, size=11.5, color=MUTED,
         align=PP_ALIGN.CENTER, pad=0.05)
pagenum(s, 1)

# ------------------------------------------------------------------ slide 3
s = slide("並列計算のおさらい — MPI と OpenMP", "2 / 14",
          note="HPC の定石「ノード間は MPI、ノード内は OpenMP」のハイブリッド構成。"
               "今回はこの標準的な書き方をそのまま持ち込むのがテーマ。")
NX, NY, NW, NH = M, 1.24, 5.35, 2.42
for i, (nm, mem) in enumerate([("ノード A", "メモリ空間 A"), ("ノード B", "メモリ空間 B")]):
    bx = NX + i * (W - 2 * M - NW)
    rect(s, bx, NY, NW, NH, WHITE, BOX)
    text(s, bx, NY + 0.06, NW, 0.36, "%s  (%s)" % (nm, mem), size=14, bold=True,
         align=PP_ALIGN.CENTER, pad=0.05)
    for c in range(2):
        cx = bx + 0.40 + c * 2.45
        rect(s, cx, NY + 0.48, 2.10, 0.62, FILL2, BOX)
        text(s, cx, NY + 0.48, 2.10, 0.62, "コア %d" % c, size=13, bold=True,
             align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE, pad=0.03)
    rect(s, bx + 0.40, NY + 1.20, 4.55, 0.40, FILL, BOX)
    text(s, bx + 0.40, NY + 1.20, 4.55, 0.40, "同じ配列を共有 (共有メモリ)", size=12,
         align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE, pad=0.03)
    text(s, bx + 0.30, NY + 1.70, 4.75, 0.60,
         "スレッドレベル並列 = OpenMP\n#pragma omp parallel for", size=11.5, color=MUTED,
         align=PP_ALIGN.CENTER, pad=0.03)
arrow(s, 6.06, NY + 0.72, 1.21, 0.62, MSO_SHAPE.LEFT_RIGHT_ARROW, "MPI", 14)
text(s, 5.92, NY + 1.42, 1.49, 0.7, "メモリが\n物理的に別", size=10.5, color=MUTED,
     align=PP_ALIGN.CENTER, pad=0.02)
text(s, M, 3.74, CW, 0.36,
     "クラスタレベル並列 = MPI :  明示的にメッセージを送り合う — MPI_Send / MPI_Recv / MPI_Barrier / MPI_Allreduce",
     size=12.5, color=MUTED, align=PP_ALIGN.CENTER, pad=0.0)

plain_table(s, M, 4.24, 6.30, 2.55, [
    ["", "OpenMP", "MPI"],
    ["単位", "スレッド", "プロセス (ランク)"],
    ["メモリ", "共有", "分散 (別空間)"],
    ["書き方", "#pragma omp parallel for", "MPI_Send / MPI_Recv"],
    ["コスト", "安い (ns 〜 μs)", "高い (μs 〜 ms)"],
    ["今回の担当", "イヤホン内の 2 コア", "イヤホン間 (左 ⇄ 右)"],
], widths=[1.5, 2.6, 2.6], size=11, row_h=0.36)

BX, BY, BW = 7.22, 4.24, 5.56
rect(s, BX, BY, BW, 2.55, WHITE, BOX)
text(s, BX, BY + 0.05, BW, 0.34, "GEMM (行列積) を 2 ランクに分ける", size=13, bold=True,
     align=PP_ALIGN.CENTER, pad=0.05)
text(s, BX + 0.15, BY + 0.40, 1.9, 0.5, "C = A · B\nN = 32", size=12, name=MONO,
     align=PP_ALIGN.CENTER, pad=0.02)
rect(s, BX + 2.05, BY + 0.42, 3.3, 0.42, FILL2, BOX)
text(s, BX + 2.05, BY + 0.42, 3.3, 0.42, "rank 0  →  行 0 .. 15", size=12,
     anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER, pad=0.03)
rect(s, BX + 2.05, BY + 0.88, 3.3, 0.42, FILL, BOX)
text(s, BX + 2.05, BY + 0.88, 3.3, 0.42, "rank 1  →  行 16 .. 31", size=12,
     anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER, pad=0.03)
arrow(s, BX + 3.45, BY + 1.36, 0.44, 0.34, MSO_SHAPE.DOWN_ARROW)
text(s, BX + 0.12, BY + 1.74, BW - 0.24, 0.72,
     "MPI_Send / MPI_Recv で結果を集約し、\nrank 0 が全体の checksum を検証して PASS / FAIL",
     size=11.5, color=MUTED, align=PP_ALIGN.CENTER, pad=0.03)
pagenum(s, 2)

# ------------------------------------------------------------------ slide 4
s = slide("今回のクラスタ構成", "3 / 14",
          note="「USB を挿したまま」ではなく「耳に入れたまま」に近づいたことを強調する。")
for i, (nm, rk, role) in enumerate([("右バッズ", "rank 0", "IBRT_MASTER"),
                                    ("左バッズ", "rank 1", "IBRT_SLAVE")]):
    bx = 1.05 + i * 6.60
    rect(s, bx, 1.24, 4.62, 1.86, WHITE, BOX)
    text(s, bx, 1.32, 4.62, 0.42, "%s   %s" % (nm, rk), size=17, bold=True,
         align=PP_ALIGN.CENTER, pad=0.05)
    text(s, bx, 1.76, 4.62, 1.20,
         "BES2300YP   /   %s\nCortex-M4F ×2  (単精度 FPU)\nSRAM 992 KB  /  Flash 4 MB" % role,
         size=12, color=MUTED, align=PP_ALIGN.CENTER, pad=0.05, space=2)
arrow(s, 5.88, 1.86, 1.56, 0.56, MSO_SHAPE.LEFT_RIGHT_ARROW, "IBRT", 13)
text(s, 5.72, 2.46, 1.88, 0.62, "TWS 制御チャネル\ncmdcode 0x8201 (自作)\n実効 512 B",
     size=10, color=MUTED, align=PP_ALIGN.CENTER, pad=0.02, space=0)
rect(s, 1.05, 3.34, 11.22, 0.50, FILL, BOX)
text(s, 1.05, 3.34, 11.22, 0.50,
     "Bluetooth SPP (自作ログチャネル)  —  USB ケーブルを抜いても計算結果が PC に届く",
     size=13, bold=True, anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER, pad=0.05)
arrow(s, 6.44, 3.92, 0.44, 0.36, MSO_SHAPE.DOWN_ARROW)
rect(s, 4.30, 4.36, 4.72, 0.94, WHITE, BOX)
text(s, 4.30, 4.42, 4.72, 0.34, "Windows PC   (COM6)", size=13, bold=True,
     align=PP_ALIGN.CENTER, pad=0.05)
text(s, 4.30, 4.76, 4.72, 0.44, "#23 GEMM-MPI ... PASS", size=12, name=MONO,
     color=MUTED, align=PP_ALIGN.CENTER, pad=0.05)
cols = [("ノード間 = MPI", "自作 MPI サブセットを IBRT (イヤホン同士の\nTWS リンク) に載せた"),
        ("ノード内 = OpenMP", "Stage-1 は逐次スタブ。API だけ本物と同じ。\n2 コア目は今回未使用 ← 正直に言う"),
        ("観測 = SPP", "ログをリングに積んで RFCOMM で PC へ。\nseq 番号で欠落・重複を機械検出")]
tw = (CW - 2 * 0.22) / 3
for i, (h, b) in enumerate(cols):
    tx = M + i * (tw + 0.22)
    panel(s, tx, 5.52, tw, 1.32, [(b, {"space": 0})], size=11.5, head=h, head_size=12.5,
          color=MUTED)
pagenum(s, 3)

# ------------------------------------------------------------------ slide 5
s = slide("実験機材: PineBuds Pro", "4 / 14",
          note="手元の個体のケースは Wiki 記載の CH342DS ではなく CH347 だった (ハードリビジョン差)。"
               "どちらも CDC-ACM なので手順は同じ。")
plain_table(s, M, 1.24, 7.35, 4.6, [
    ["項目", "値"],
    ["SoC", "Bestechnic BES2300YP (左右に 1 個ずつ、完全に独立)"],
    ["CPU", "Dual-core ARM Cortex-M4F @ 最大 300 MHz"],
    ["FPU", "単精度のみ — double はソフトエミュ = 実質禁止"],
    ["メモリ", "SRAM 992 KB (+ BT 共有 64 KB) / Flash 4 MB"],
    ["ファーム", "OpenPineBuds (pine64) — 完全オープンソース"],
    ["ビルド", "Docker + Make、gnu++98 / -fno-exceptions / -fno-rtti"],
    ["デバッグ", "充電ケースが USB → デュアル UART を兼ねる (2 Mbaud)"],
    ["実測残量", "RAM 約 330 KB 空き / Flash 約 3.2 MB 空き (.map 集計)"],
    ["位置づけ", "実売 1 万円前後、普通に音楽が聴ける TWS イヤホン"],
], widths=[1.35, 6.0], size=11.5, row_h=0.40)
rect(s, 8.20, 1.24, 4.58, 1.55, FILL, RULE)
text(s, 8.20, 1.24, 4.58, 1.55, "[  PineBuds Pro の写真をここに  ]", size=13, color=FAINT,
     align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE, pad=0.1)
panel(s, 8.20, 2.95, 4.58, 2.10, [
    ("ケースに USB-C を挿すと", {"space": 4}),
    ("/dev/ttyACM0 (右) と /dev/ttyACM1 (左)", {"space": 4, "name": MONO, "size": 11.5}),
    ("が生えて、bestool write-image で自作ファームが焼ける。", {"space": 8}),
    ("イヤホンなのに、開発ボードとして完結している。", {"space": 0, "bold": True}),
], size=12.5, head="つまり")
code(s, 8.20, 5.22, 4.58, 1.42, [
    "$ picocom -b 2000000 /dev/ttyACM0",
    "[ctor] GlobalProbe constructed",
    "hello, C++ from PineBuds (core=0)",
    "GEMM ... checksum=32768.000000 PASS",
], size=10)
pagenum(s, 4)

# ------------------------------------------------------------------ slide 6
s = slide("なぜ「普通のワイヤレスイヤホン」ではダメだったのか", "5 / 14",
          note="「Linux が動くイヤホン」ではなく「自分のコードを載せられるイヤホン」を探した、という話。"
               "文鎮化リスクは共感を得やすい。実際いちばん最初にやったのは工場ファームの全量バックアップ。")
rect(s, M, 1.22, CW, 0.52, FILL, BOX)
text(s, M, 1.22, CW, 0.52,
     "当初は手持ちの一般的な TWS イヤホンでやりたかった。 →  自作コードを載せる手段が「ゼロ」だった。",
     size=15, bold=True, anchor=MSO_ANCHOR.MIDDLE, pad=0.12)
plain_table(s, M, 1.94, CW, 2.95, [
    ["自作コードをイヤホンで動かすのに必要なもの", "一般的な TWS", "PineBuds Pro"],
    ["① ファームウェアのソース / SDK",
     ("✗  非公開", {"color": NG, "bold": True}),
     ("✓  OSS (OpenPineBuds)", {"color": OK, "bold": True})],
    ["② 書き込み経路 (署名されていない FW を焼ける)",
     ("✗  署名検証で拒否", {"color": NG, "bold": True}),
     ("✓  bestool で普通に焼ける", {"color": OK, "bold": True})],
    ["③ 標準出力 = デバッグ UART が外に出ている",
     ("✗  出ていない", {"color": NG, "bold": True}),
     ("✓  充電ケースが UART を兼ねる", {"color": OK, "bold": True})],
    ["④ 左右間リンクを叩ける API",
     ("✗  非公開の独自 TWS", {"color": NG, "bold": True}),
     ("✓  IBRT の API がヘッダに", {"color": OK, "bold": True})],
    ["⑤ 文鎮化したときの復旧手段",
     ("✗  無い ( = 終わり )", {"color": NG, "bold": True}),
     ("✓  工場 FW + 純正ライタが配布", {"color": OK, "bold": True})],
], widths=[5.6, 3.1, 3.5], size=12, row_h=0.42)
text(s, M, 5.05, CW, 0.95, [
    ("・ ① 〜 ③ のどれか 1 つでも欠けると「コードを書いても載せられない」ので、実験自体が成立しない", {"space": 5}),
    ("・ 一般的な TWS は音質・ANC・電池のために作られていて、ユーザーが計算を載せる想定がそもそも無い", {"space": 5}),
    ("・ 分解して SWD を引き出す道もあるが、左右 2 個を同じ状態に保って何十回も焼き直す用途には向かない", {"space": 0}),
], size=12.5, color=MUTED)
rect(s, M, 6.10, CW, 0.62, FILL2, BOX)
text(s, M, 6.10, CW, 0.62,
     "→  「オープンソースファーム」と「ケースが UART プログラマ」を両立している TWS が実質ここだけだったので購入",
     size=14.5, bold=True, anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER, pad=0.12)
pagenum(s, 5)

# ------------------------------------------------------------------ slide 7
s = slide("やったこと — 全体像", "6 / 14",
          note="「アダプタが嘘をついていないこと」を本物の OpenMPI との一致で担保した、"
               "というのが一番言いたい設計判断。")
layers = [
    ("bench/gemm_mpi_omp.cpp", "標準 MPI + OpenMP API で書いた GEMM。実機用に 1 行も書き換えない ← これが要件", FILL2),
    ("adapters/mpi/  +  adapters/omp/", "自作 MPI サブセット (Init/Send/Recv/Barrier/Allreduce/Isend/Irecv/Wait) と OpenMP スタブ", FILL),
    ("transport seam (関数ポインタ 1 枚)", "ホスト = pthread ループバック (TDD 用) / 実機 = mpi_frag (断片化 + クレジット制御)", FILL),
    ("firmware/pinebuds_compute/", "IBRT glue / SPP ログチャネル / 5 連タップ起動の調停", FILL),
    ("OpenPineBuds SDK", "IBRT (TWS 制御) · BESAUD · RTX (CMSIS-RTOS v1)", FILL2),
]
LX, LW = M, 7.55
for i, (h, b, f) in enumerate(layers):
    ly = 1.22 + i * 0.84
    rect(s, LX, ly, LW, 0.76, f, BOX)
    text(s, LX + 0.10, ly + 0.05, LW - 0.2, 0.32, h, size=13, bold=True, pad=0.02)
    text(s, LX + 0.10, ly + 0.36, LW - 0.2, 0.36, b, size=10.5, color=MUTED, pad=0.02)
text(s, LX, 5.48, LW, 0.42,
     "規模: adapters 926 行 / firmware 統合 1,656 行 / tests 1,846 行 / 設計ドキュメント 2,601 行",
     size=11.5, color=MUTED, pad=0.02)
panel(s, 8.32, 1.22, 4.46, 2.30, [
    ("・ -std=gnu++98 / -fno-exceptions / -fno-rtti", {"space": 6}),
    ("   → C++11 以降が使えない", {"space": 8, "color": MUTED, "size": 11}),
    ("・ ヒープ禁止・STL 禁止・静的バッファのみ", {"space": 6}),
    ("・ double 禁止 (単精度 FPU のみ)。リテラルは 1.0f", {"space": 6}),
    ("・ make check98 で機械的に担保", {"space": 0}),
], size=12, head="SDK に合わせるしかない制約")
panel(s, 8.32, 3.66, 4.46, 2.62, [
    ("① ホストで 1,788 checks (8 バイナリ)", {"space": 5, "bold": True}),
    ("   実機に焼く前に全部を緑にする運用", {"space": 9, "color": MUTED, "size": 11}),
    ("② ゴールデンリファレンス照合", {"space": 5, "bold": True}),
    ("   bench/ の同一ソースを", {"space": 3, "color": MUTED, "size": 11}),
    ("   (a) 本物の OpenMPI + libgomp (np=1 / np=2)", {"space": 3, "color": MUTED, "size": 11}),
    ("   (b) 自作アダプタ", {"space": 3, "color": MUTED, "size": 11}),
    ("   の両方でビルド実行 → 3 経路すべて", {"space": 3, "color": MUTED, "size": 11}),
    ("   checksum=32768.000000 一致", {"space": 0, "size": 11, "name": MONO}),
], size=12, head="検証戦略")
pagenum(s, 6)

# ------------------------------------------------------------------ slide 8
s = slide("実装の山場 ① — SDK にソースが無い", "7 / 14",
          note="「ヘッダを信じるな、リンクされるバイナリを信じろ」。ここは技術者ウケが良いはず。")
rect(s, M, 1.22, CW, 0.60, FILL, BOX)
text(s, M, 1.22, CW, 0.60,
     "問題:  左右リンクの本体 services/ibrt_core/ には inc/ と lib/ しか無い  =  プリビルトの .a しか無い  →  仕様が読めない",
     size=14, bold=True, anchor=MSO_ANCHOR.MIDDLE, pad=0.12)
rect(s, M, 1.94, CW, 0.52, FILL2, BOX)
text(s, M, 1.94, CW, 0.52,
     "やったこと:  libtws_ibrt_enhanced_stack_anc_RTX.a を逆アセンブルして、仕様を実物から確定した",
     size=14, bold=True, anchor=MSO_ANCHOR.MIDDLE, pad=0.12)
plain_table(s, M, 2.62, CW, 1.80, [
    ["調べたこと", "ヘッダの記述", "逆アセンブルした実体", "設計への影響"],
    ["送信ペイロード上限", "APP_TWS_CTRL_BUFFER_MAX_LEN = 300",
     ("実行時 assert は 672", {"bold": True}), "300 と 672 のどちらで切るかで設計が変わる"],
    ["RX ハンドラのスレッド", "記述なし",
     ("BesbtThread (BT スタック本体)", {"bold": True}), "ここをブロックすると BT が全部止まる"],
    ["送信ポンプ", "記述なし", "同じループの 1 行上", "計算は別スレッドに隔離する必要がある"],
], widths=[2.4, 3.4, 3.2, 4.2], size=11, row_h=0.42)
panel(s, M, 4.62, 6.30, 2.20, [
    ("・ 計算は専用スレッドに隔離", {"space": 5, "bold": True}),
    ("   app_init 末尾で同期実行すると電源管理まで止まる", {"space": 8, "color": MUTED, "size": 11}),
    ("・ ペイロード上限は「実測で決める」", {"space": 5, "bold": True}),
    ("   起動時に 4 → 64 → ... → 668 B を掃引するモードを実装", {"space": 3, "color": MUTED, "size": 11}),
    ("   実測 max_payload = 512  (668 は TIMEOUT)", {"space": 0, "size": 12, "name": MONO}),
], size=12, head="確定した設計")
panel(s, 7.10, 4.62, 5.68, 2.20, [
    ("・ 512 B 超は断片化 + クレジット制御 (W=2)", {"space": 5}),
    ("   in-flight を 536 B に抑える", {"space": 8, "color": MUTED, "size": 11}),
    ("・ ヘッダの 300 でも blob の 672 でもなかった", {"space": 5}),
    ("・ 追加 RAM は 5.5 KB (合計 22.7 KB)", {"space": 3}),
    ("   RAM 残 330 KB に対して余裕", {"space": 0, "color": MUTED, "size": 11}),
], size=12, head="実装したトランスポート")
pagenum(s, 7)

# ------------------------------------------------------------------ slide 9
s = slide("実装の山場 ② — 実機は 1 発では動かない", "8 / 14",
          note="逆アセンブルではなく nm でシンボル解決して犯人を特定した、という小ネタも入る。"
               "「組み込みは電源とペアリングで死ぬ」は共感ポイント。")
plain_table(s, M, 1.22, 7.60, 4.50, [
    ["#", "症状", "原因", "対処"],
    ["1", "finalize の 2 秒後に MemFault で無限リブート (17.5 秒周期)",
     "RTX でスレッドが自分自身を終了させるとスケジューラが壊れる", "return させず park"],
    ["2", "充電起動すると BT スタックが上がらない (size=1 に縮退)",
     "CHARGER_PLUGINOUT_RESET=1 の副作用", "target.mk を 0 にパッチ"],
    ["3", "走行中にリンクが切れる",
     "充電器の PLUGIN が CLOSE_BOX を注入していた", "箱イベントを遮断"],
    ["4", "起動 15 〜 60 秒で勝手に電源が落ちる",
     "FULL_CHARGING → app_shutdown() (満充電シャットダウン)", "app_shutdown() を if(0)"],
    ["5", "PC と再ペアリングできない",
     "通常ビルドは page scan だけで inquiry scan を出さない", "専用ビルドで discoverable"],
    ["6", "SPP ログの先頭 512 B だけ二重に届く",
     "1 回の RFCOMM open で CONNECTED が 2 回上がる", "送信 FSM で重複を無視"],
], widths=[0.4, 3.0, 3.4, 2.2], size=10, row_h=0.55)
code(s, 8.35, 1.22, 4.43, 3.55, [
    "[mpi] finalize done rank=0",
    "### EXCEPTION ###",
    "PC =002AC1BE, ExceptionNumber=-12",
    "XPSR=2100000B    ; IPSR=0x0B = SVCall",
    "CFSR =00000082   ; MMARVALID|DACCVIOL",
    "FaultInfo : (MemFault)",
    "FaultCause: (Data access violation)",
    "Current Task    : 0",
    ("New Running Task: 255   <- 次が居ない", {"bold": True}),
], size=10.5, head="Run 2 : 両バッズ・毎ブート再現")
panel(s, 8.35, 4.94, 4.43, 1.78, [
    ("・ 修正は全部ホストのテストに落としてから直した", {"space": 6}),
    ("・ 実機で試す前に 1,788 checks を緑にする", {"space": 6}),
    ("・ SPP の重複送信バグも 5 本のテストを先に書いた", {"space": 0}),
], size=11.5, head="効いた運用", color=MUTED)
rect(s, M, 5.92, 7.60, 0.60, FILL2, BOX)
text(s, M, 5.92, 7.60, 0.60, "6 個の地雷のうち、計算に起因するものはゼロ。全部が電源・接続・RTOS。",
     size=13.5, bold=True, anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER, pad=0.1)
pagenum(s, 8)

# ------------------------------------------------------------------ slide 10
s = slide("実装の山場 ③ — 観測をワイヤレスに / 耳で操作する", "9 / 14",
          note="デモ的に一番「おっ」となるところ。ケースの中でもタッチは反応する。")
rect(s, M, 1.22, CW, 0.46, FILL, BOX)
text(s, M, 1.22, CW, 0.46, "USB ケーブルで繋がったイヤホンは、もはやイヤホンではない",
     size=14, bold=True, anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER, pad=0.1)
panel(s, M, 1.84, 6.05, 2.62, [
    ("・ COMPUTE_TRACE のログ行をリングバッファに積み、", {"space": 3}),
    ("   SPP (RFCOMM) で Windows に流す", {"space": 7, "color": MUTED}),
    ("・ PC 側は spp_tail.py COM6 で受けるだけ", {"space": 3}),
    ("   #<seq> 番号で欠落・重複を機械検出できる", {"space": 7, "color": MUTED}),
    ("・ 結果: USB を抜いても PASS 行が PC に届く", {"space": 0, "bold": True}),
], size=12, head="① Bluetooth SPP に自作ログチャネルを生やす")
panel(s, 6.78, 1.84, 6.00, 2.62, [
    ("・ タッチパッドは BES2300YP の電源キー入力。", {"space": 3}),
    ("   ジェスチャは SDK がソフトで判定 (hal_key.c)", {"space": 7, "color": MUTED}),
    ("・ 単タップ 〜 4 連・長押しは既存機能で埋まっていた", {"space": 3}),
    ("   → 未割当の RAMPAGECLICK (5 連) を奪う", {"space": 7, "color": MUTED}),
    ("・ 調停ロジックはホストで 8 テスト書いてから実装", {"space": 0, "bold": True}),
], size=12, head="② 5 連タップで GEMM を再実行する")
FY = 4.90
steps = [("5 連タップ", "app_key_compute_run\nevent 12"),
         ("タップした側", "[mpi] run #1\ntrigger=local peer_notified=1"),
         ("相手側", "[mpi] run #1\ntrigger=peer"),
         ("両バッズで GEMM-MPI", "rank 0 が checksum 検証\n[mpi] run #1 done rank=0")]
sw = 2.72
gap = (CW - 4 * sw) / 3
for i, (h, b) in enumerate(steps):
    sx = M + i * (sw + gap)
    rect(s, sx, FY, sw, 1.42, FILL if i % 2 == 0 else WHITE, BOX)
    text(s, sx, FY + 0.08, sw, 0.36, h, size=13, bold=True, align=PP_ALIGN.CENTER, pad=0.04)
    text(s, sx, FY + 0.46, sw, 0.86, b, size=10, name=MONO, color=MUTED,
         align=PP_ALIGN.CENTER, pad=0.04, space=1)
    if i < 3:
        arrow(s, sx + sw + 0.06, FY + 0.52, gap - 0.12, 0.36, MSO_SHAPE.RIGHT_ARROW)
text(s, M, 6.42, CW, 0.40,
     "START フレームは既存の cmdcode 0x8201 に kind 5 を足しただけ ( [kind, seq LE32] の 5 バイト )。実行中の連打は捨てる。",
     size=11.5, color=MUTED, align=PP_ALIGN.CENTER, pad=0.0)
pagenum(s, 9)

# ------------------------------------------------------------------ slide 11
s = slide("結果 ① — 実機ログ (両バッズ / 全条件 PASS)", "10 / 14",
          note="32768 = 32³。全要素 1 の行列積は整数値しか通らないので float32 で厳密に一致する。"
               "誤差 1 ulp も許さない判定にしてある、は必ず言う。")
code(s, M, 1.22, 7.55, 5.30, [
    ("== 右バッズ (rank 0) ==", {"bold": True, "size": 11}),
    "CHARGING PWRON!  ->  bt_stack_init_done:10",
    "[mpi] side=RIGHT rank=0 nv_role=[IBRT_MASTER] init_done=1",
    "[mpi] init rank=0 size=2 link_wait=200 ms besaud=1",
    "[mpi] peer ok rank=0 peer=1",
    "[mpi] barrier ok",
    ("GEMM-MPI N=32 rank=0 size=2 checksum=32768.000000", {"bold": True}),
    ("         expect=32768.000000 PASS", {"bold": True}),
    ("GEMM-MPI elapsed=13 ms  frames tx=2 rx=4 err=0", {"bold": True}),
    "[mpi] finalize done rank=0",
    "",
    ("== 左バッズ (rank 1) ==", {"bold": True, "size": 11}),
    "[mpi] side=LEFT rank=1 nv_role=[IBRT_SLAVE] init_done=1",
    "[mpi] init rank=1 size=2 link_wait=500 ms besaud=1",
    "[mpi] peer ok rank=1 peer=0  ->  [mpi] barrier ok",
    "[mpi] send ok",
    "[mpi] frames tx=2 rx=2 err=0",
    "[mpi] finalize done rank=1",
], size=11, head="Run 4  (2026-09-01)  —  ケース内・充電起動・両バッズ同時")
checks = [
    "rank 0 / rank 1 が左右で 1 つずつ (静的 rank が設計どおり)",
    "両側 size=2 — 縮退していない = 本当に 2 ノードで走った",
    "checksum=32768.000000 の厳密一致 (float32 で誤差ゼロ)",
    "MPI フレーム (cmdcode 0x8201) の送信失敗 0 回",
    "### EXCEPTION ###  0 回・リブート無し",
    "両側 init_done=1 (BT スタックが本当に上がっている)",
]
rect(s, 8.32, 1.22, 4.46, 3.60, FILL, BOX)
rect(s, 8.32, 1.22, 4.46, 0.42, FILL2, BOX)
text(s, 8.32, 1.22, 4.46, 0.42, "合否条件 6 項目 — すべて充足", size=13.5, bold=True,
     anchor=MSO_ANCHOR.MIDDLE, pad=0.12)
for i, c in enumerate(checks):
    cy = 1.72 + i * 0.50
    text(s, 8.44, cy, 0.30, 0.42, "✓", size=15, bold=True, color=OK, pad=0.0)
    text(s, 8.76, cy, 3.94, 0.46, c, size=11, pad=0.0)
panel(s, 8.32, 4.98, 4.46, 1.74, [
    ("32768 = 32³", {"space": 5, "bold": True, "size": 14, "name": MONO}),
    ("全要素 1 の行列積は C[i][j]=32 で整数値しか通らない", {"space": 3, "color": MUTED}),
    ("→ float32 でも丸め誤差がゼロ", {"space": 3, "color": MUTED}),
    ("→ 厳密一致以外を許さない判定にできる", {"space": 0, "bold": True}),
], size=11.5, head="なぜ checksum で判定できるのか")
pagenum(s, 10)

# ------------------------------------------------------------------ slide 12
s = slide("結果 ② — ワイヤレス受信と 5 連タップ再実行", "11 / 14",
          note="「イヤホンをトントントントントンと叩くと、両耳で行列積が走って、"
               "結果が Bluetooth で PC に飛ぶ」で締める。")
code(s, M, 1.22, 6.55, 2.62, [
    "#0  [ctor] GlobalProbe constructed",
    "#1  hello, C++ from PineBuds (core=0)",
    "#2  GEMM float N=32  checksum=32768.000000  PASS",
    "#3  GEMM elapsed=9 ms",
    "#7  [mpi] peer ok rank=1 peer=0",
    "#16 [mpi-t1] probe len=512 ok rtt=124 ms",
    ("#18 [mpi-t1] rtt n=100 min=44 avg=169 max=407 ms", {"bold": True}),
    ("#19 [mpi-t1] max_payload=512", {"bold": True}),
    "#20 [mpi] barrier ok",
    "#23 [mpi] finalize done rank=1",
], size=11, head="Run 12 : Bluetooth SPP で PC が受けたログ (欠落・重複ゼロ)")
plain_table(s, 7.30, 1.22, 5.48, 2.55, [
    ["run", "トリガ", "結果", "elapsed"],
    ["起動時", "自動", "PASS  size=2", "12 ms"],
    ["#1", "右を 5 連タップ", "PASS  size=2", "128 ms"],
    ["#2", "左を 5 連タップ", "PASS  size=2", ("5 ms", {"bold": True})],
    ["#3", "右を 5 連タップ", "PASS  size=2", "115 ms"],
    ["#4", "左を 5 連タップ", "PASS  size=2", "83 ms"],
], widths=[0.9, 1.9, 1.6, 1.1], size=11, row_h=0.40,)
text(s, 7.30, 3.80, 5.48, 0.30, "Run 13 : 右 = rank 0 の UART", size=10.5, color=MUTED, pad=0.02)
panel(s, 7.30, 4.14, 5.48, 2.10, [
    ("・ タップした側が先に走り出し、相手は START の伝搬待ちで合流する", {"space": 4}),
    ("   → タップした側 (rank 0) の elapsed が伸びる  (#1 / #3)", {"space": 8, "color": MUTED}),
    ("・ 逆に相手からの START で後から合流した #2 は 5 ms", {"space": 4}),
    ("   起動時ラン (12 ms) より速い = クロック引き上げが効いている", {"space": 0, "color": MUTED}),
], size=11.5, head="elapsed のばらつきの読み方")
code(s, M, 3.96, 6.55, 2.00, [
    "app_key_compute_run event 12",
    ("[mpi] run #1 trigger=local peer_notified=1", {"bold": True}),
    "GEMM-MPI N=32 rank=0 size=2 checksum=32768.000000 PASS",
    "GEMM-MPI elapsed=128 ms frames tx=1 rx=1 err=0",
    "[mpi] run #1 done rank=0",
    ("[mpi] run #2 trigger=peer", {"bold": True}),
    "GEMM-MPI elapsed=5 ms frames tx=1 rx=0 err=0",
], size=10.5, head="Run 13 : 5 連タップ時の右バッズ (rank 0) UART")
rect(s, M, 6.10, 6.55, 0.72, FILL2, BOX)
text(s, M, 6.10, 6.55, 0.72,
     "USB を抜いた状態で、耳を 5 回叩くと\n両バッズで行列積が走り、結果が PC に流れてくる",
     size=13.5, bold=True, anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER, pad=0.08, space=2)
pagenum(s, 11)

# ------------------------------------------------------------------ slide 13
s = slide("結果 ③ — 数字で見る", "12 / 14",
          note="「速くなりませんでした」で終わらせず、なぜ遅いかが定量的に説明できることを成果として出す。")
plain_table(s, M, 1.22, 7.55, 4.55, [
    ["指標", "実測値", "コメント"],
    ["GEMM N=32  単コア (1 ノード)", "5 〜 9 ms", "Phase 0.5 のゴールデン基準"],
    ["GEMM N=32  2 ノード MPI", ("12 〜 13 ms", {"bold": True}),
     ("並列化して遅くなっている", {"bold": True})],
    ["左右間 RTT (100 回)", "min 44 / avg 169 / max 407 ms", "sniff 復帰待ちで大きく揺れる"],
    ["左右間 実効ペイロード", "512 B", "ヘッダの 300 でも blob の 672 でもない"],
    ["追加 RAM", "5.5 KB (合計 22.7 KB)", "RAM 残 330 KB に対して余裕"],
    ["ホストテスト", "1,788 checks / 8 バイナリ", "焼く前に全部緑にする"],
    ["コード規模", "アダプタ 926 / 統合 1,656 行", "テスト 1,846 行 — 本体より多い"],
    ["設計ドキュメント", "2,601 行", "大半が逆アセンブル結果の記録"],
], widths=[2.6, 2.5, 2.9], size=10.5, row_h=0.42)
rect(s, 8.32, 1.22, 4.46, 4.55, FILL, BOX)
rect(s, 8.32, 1.22, 4.46, 0.42, FILL2, BOX)
text(s, 8.32, 1.22, 4.46, 0.42, "なぜ遅いのか  =  この実験の本題", size=13.5, bold=True,
     anchor=MSO_ANCHOR.MIDDLE, pad=0.12)
rect(s, 8.52, 1.82, 4.06, 0.86, WHITE, BOX)
text(s, 8.52, 1.86, 4.06, 0.80,
     "N=32 GEMM の計算量\n2 × 32³ ≒ 65,000 flop  →  数 ms", size=12,
     align=PP_ALIGN.CENTER, pad=0.05, space=2)
rect(s, 8.52, 2.80, 4.06, 0.86, WHITE, BOX)
text(s, 8.52, 2.84, 4.06, 0.80,
     "左右間 1 往復のレイテンシ\n44 〜 407 ms", size=12, bold=True,
     align=PP_ALIGN.CENTER, pad=0.05, space=2)
arrow(s, 10.33, 3.72, 0.44, 0.32, MSO_SHAPE.DOWN_ARROW)
text(s, 8.44, 4.10, 4.22, 1.55, [
    ("通信 1 回で、計算 100 回ぶんの時間が飛ぶ", {"space": 8, "bold": True, "size": 13}),
    ("「計算量 / 通信量」が小さい問題を分散しては いけない — HPC の教科書どおりの結論を、"
     "耳の中で再現してしまった。", {"space": 8, "size": 11.5, "color": MUTED}),
    ("N を大きくすれば逆転するが、今度は RAM (330 KB) が先に尽きる。",
     {"space": 0, "size": 11.5, "color": MUTED}),
], size=12, pad=0.06)
rect(s, M, 5.95, 7.55, 0.72, FILL2, BOX)
text(s, M, 5.95, 7.55, 0.72,
     "速くはならなかった。が、なぜ遅いのかを 定量的に説明できる状態にはなった。",
     size=13.5, bold=True, anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER, pad=0.1)
pagenum(s, 12)

# ------------------------------------------------------------------ slide 14
s = slide("わかったこと / できていないこと", "13 / 14",
          note="できていないことを先に自分で言い切ると、質疑が建設的になる。")
items_ok = [
    ("標準 API のまま実機に載る", "bench/ のソースは本物の OpenMPI でも自作アダプタでも無改変でビルドでき、3 経路すべて checksum 一致。アダプタ境界の設計が正しかった"),
    ("ソースが無くても仕様は確定できる", "プリビルト .a の逆アセンブルで、ヘッダと食い違う実挙動 (上限 672 / RX が BT スタックスレッド) を根拠付きで確定できた"),
    ("組み込みの敵は計算ではない", "踏んだ 6 個の地雷のうち、計算に起因するものはゼロ。全部が電源・ペアリング・RTOS"),
    ("ホストで TDD できる形に切ると実機デバッグが激減する", "実機で 1 回試す前にホストで 1,788 checks。実機で出たバグもホストのテストに落としてから直した"),
]
items_ng = [
    ("ノード内 2 コア並列は未達", "OpenMP は Stage-1 の逐次スタブのまま。2 個目のコア (CP) は音声処理が使っており、計算に取ると取り合いになる"),
    ("slave 側のログが電波に乗らない", "IBRT の snoop リンクでは slave の SPP 送信がスタック上は成功扱いなのに実際には飛ばない。rank 0 が master の時しか PC に届かない"),
    ("音楽再生との同時実行は未検証", "「イヤホンとして使いながら裏で計算」はまだ道半ば"),
    ("N を大きくした時のスケーリングは未計測", "RAM 330 KB が上限で、N=128 程度まで"),
]
for col, (head, items, mark, mc) in enumerate([
        ("わかったこと", items_ok, "✓", OK), ("できていないこと (正直に)", items_ng, "✗", NG)]):
    cx = M + col * (CW / 2 + 0.12)
    cw = CW / 2 - 0.12
    rect(s, cx, 1.22, cw, 5.42, FILL if col == 0 else WHITE, BOX)
    rect(s, cx, 1.22, cw, 0.46, FILL2, BOX)
    text(s, cx, 1.22, cw, 0.46, head, size=15, bold=True, anchor=MSO_ANCHOR.MIDDLE, pad=0.14)
    for i, (h, b) in enumerate(items):
        iy = 1.80 + i * 1.22
        text(s, cx + 0.14, iy, 0.32, 0.36, mark, size=15, bold=True, color=mc, pad=0.0)
        text(s, cx + 0.48, iy - 0.02, cw - 0.66, 0.36, h, size=12.5, bold=True, pad=0.0)
        text(s, cx + 0.48, iy + 0.32, cw - 0.66, 0.82, b, size=10.5, color=MUTED, pad=0.0)
pagenum(s, 13)

# ------------------------------------------------------------------ slide 15
s = slide("まとめ", "14 / 14",
          note="締め:「みなさんの耳にも、暇をしている Cortex-M4F が 2 個入っています」。")
rect(s, M, 1.22, CW, 0.86, FILL2, BOX)
text(s, M, 1.22, CW, 0.86, "1 万円のワイヤレスイヤホン 2 個  =  2 ノードの MPI クラスタ",
     size=24, bold=True, anchor=MSO_ANCHOR.MIDDLE, align=PP_ALIGN.CENTER, pad=0.1)
res = [(OK, "✓", "標準 MPI + OpenMP で書いたベンチが、無改変で耳の中で PASS した"),
       (OK, "✓", "checksum=32768.000000 厳密一致 / 両バッズ size=2 / EXCEPTION 0 回"),
       (OK, "✓", "5 連タップで再実行、結果は Bluetooth SPP で PC にワイヤレス配信"),
       (NG, "✗", "速くはならなかった (通信 407 ms vs 計算 数 ms) — が、その理由は定量的に説明できる")]
for i, (c, m, t) in enumerate(res):
    ry = 2.32 + i * 0.50
    text(s, M + 0.20, ry, 0.34, 0.42, m, size=16, bold=True, color=c, pad=0.0)
    text(s, M + 0.58, ry + 0.02, CW - 0.9, 0.42, t, size=14, pad=0.0)
nexts = [("① ノード内 2 コア並列", "OpenMP Stage-2。CP コアを音声処理と\nどう分け合うかが論点"),
         ("② 通信に耐える問題を選ぶ", "Red-Black SOR — ハロー交換は境界だけ\n= 通信量が小さい"),
         ("③ 音楽再生との同時実行", "本当に「イヤホンとして使いながら\n裏で計算」させる")]
tw = (CW - 2 * 0.22) / 3
for i, (h, b) in enumerate(nexts):
    tx = M + i * (tw + 0.22)
    panel(s, tx, 4.52, tw, 1.42, [(b, {"space": 0})], size=11.5, head=h, head_size=12.5,
          color=MUTED)
hline(s, M, 6.28, CW)
text(s, M, 6.42, CW, 0.9, [
    ("github.com/TamichiRyuto/pine-buds-cluster", {"space": 4, "size": 16, "bold": True,
                                                   "name": MONO, "align": PP_ALIGN.CENTER}),
    ("設計・実機ログ・失敗の記録はすべて docs/design-ibrt-transport.md (2,601 行) にあります",
     {"space": 0, "size": 11.5, "color": MUTED, "align": PP_ALIGN.CENTER}),
], pad=0.0)
pagenum(s, 14)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
prs.save(OUT)
print("wrote %s (%d slides)" % (OUT, len(prs.slides.__iter__.__self__._sldIdLst)))
