# pine-buds-cluster

PineBuds Pro (BES2300YP, dual-core Cortex-M4F) を並列計算クラスタにする実験。

現フェーズ (Phase 0.5) のゴールは [HANDOVER_pinebuds_cpp_bringup.md](HANDOVER_pinebuds_cpp_bringup.md) 参照:

- **Milestone A**: C++ hello-world を UART (2 Mbaud) に出す（static ctor 実行の証明込み）
- **Milestone B**: 単コア float GEMM 自己検証 (N=32, checksum=32768) を PASS させる

## 構成

| パス | 内容 |
|---|---|
| `src/` | ターゲット非依存の計算カーネル (freestanding C++, float のみ, 純関数) |
| `tests/` | ホスト側ユニットテスト (`make test`) |
| `firmware/` | OpenPineBuds への統合層 |
| `scripts/` | 環境構築・ビルド補助 |
| `docs/` | 人間向けマニュアル |
| `external/` | OpenPineBuds クローン先 (git 管理外) |

## クイックスタート

```bash
make test                          # ホストでカーネルの単体テスト
./scripts/setup-openpinebuds.sh    # SDK クローン + docker チェック
```

開発ルールは [CLAUDE.md](CLAUDE.md)（tidy-first / TDD / コミット規約）を参照。
