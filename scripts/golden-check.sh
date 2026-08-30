#!/usr/bin/env bash
# Golden-reference check for the unmodified-benchmark strategy.
#
# Builds bench/gemm_mpi_omp.cpp twice from the SAME unmodified source:
#   1. real OpenMPI + libgomp (ubuntu:22.04 container), run with np=1 and np=2
#   2. our adapters (host g++, sequential 1-rank)
# and verifies every run reports the exact same checksum (32768 = 32^3) and
# PASS. The 2-rank adapter path is covered by `make test` (pthread harness).
#
# Requires docker. Safe to re-run; the image is cached after the first build.
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=pine-buds-golden
EXPECT="checksum=32768.000000"

echo "== [1/3] build golden image (cached after first run) =="
docker build -q -t "$IMAGE" - <<'EOF'
FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    g++ make openmpi-bin libopenmpi-dev && rm -rf /var/lib/apt/lists/*
EOF

echo "== [2/3] real OpenMPI + libgomp: np=1 and np=2 =="
docker run --rm -v "$PWD:/work:ro" -w /work "$IMAGE" bash -ec "
  mpic++ -O2 -fopenmp -Wall -Werror bench/gemm_mpi_omp.cpp -o /tmp/gemm_bench
  for np in 1 2; do
    out=\$(mpirun --allow-run-as-root --oversubscribe -np \$np /tmp/gemm_bench)
    echo \"np=\$np: \$out\"
    echo \"\$out\" | grep -q '$EXPECT' || { echo 'checksum mismatch'; exit 1; }
    echo \"\$out\" | grep -q 'PASS' || { echo 'no PASS'; exit 1; }
  done
"

echo "== [3/3] adapter build (host, sequential 1-rank) =="
g++ -O2 -Wall -Werror -Wno-unknown-pragmas -Iadapters/mpi -Iadapters/omp \
    bench/gemm_mpi_omp.cpp adapters/mpi/*.cpp adapters/omp/*.cpp \
    -o build/gemm_bench_adapter
out=$(./build/gemm_bench_adapter)
echo "adapter: $out"
echo "$out" | grep -q "$EXPECT" || { echo 'checksum mismatch'; exit 1; }
echo "$out" | grep -q 'PASS' || { echo 'no PASS'; exit 1; }

echo "GOLDEN CHECK OK: real OpenMPI (np=1,2) and adapter build agree on $EXPECT"
