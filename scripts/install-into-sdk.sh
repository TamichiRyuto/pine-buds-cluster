#!/usr/bin/env bash
# Install the compute bring-up code into the OpenPineBuds checkout.
# Idempotent: safe to re-run after editing src/ or firmware/.
#
# What it does:
#   1. Copies kernel + app sources into apps/main/ (auto-globbed by its Makefile)
#   2. Defines PINEBUDS_TARGET for apps/main/ so compute_trace.h picks TRACE
#   3. Hooks compute_main() into app_init(), just BEFORE the sysfreq drop to
#      32K at the end of app_init (so the GEMM runs at full clock)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${REPO_ROOT}/external/OpenPineBuds"
DST="${SDK_DIR}/apps/main"
APPS_CPP="${DST}/apps.cpp"
MARKER="pine-buds-cluster compute hook"

[ -d "${DST}" ] || { echo "error: run scripts/setup-openpinebuds.sh first"; exit 1; }

cp "${REPO_ROOT}"/src/gemm.h "${REPO_ROOT}"/src/gemm.cpp \
   "${REPO_ROOT}"/src/gemm_selftest.h "${REPO_ROOT}"/src/gemm_selftest.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/compute_main.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/compute_trace.h \
   "${DST}/"
echo "[ok] sources copied into apps/main/"

if ! grep -q "PINEBUDS_TARGET" "${DST}/Makefile"; then
    printf '\n# %s\nccflags-y += -DPINEBUDS_TARGET\n' "${MARKER}" >> "${DST}/Makefile"
    echo "[ok] -DPINEBUDS_TARGET added to apps/main/Makefile"
else
    echo "[ok] apps/main/Makefile already defines PINEBUDS_TARGET"
fi

if grep -q "${MARKER}" "${APPS_CPP}"; then
    echo "[ok] app_init hook already present"
else
    python3 - "${APPS_CPP}" <<'EOF'
import sys

path = sys.argv[1]
# extern "C" is a linkage-specification: it must live at file scope,
# not inside a block. Declare above app_init, call inside it.
decl_anchor = "int app_init(void)"
decl = 'extern "C" void compute_main(void); /* pine-buds-cluster */\n\n'
call_anchor = "  app_sysfreq_req(APP_SYSFREQ_USER_APP_INIT, APP_SYSFREQ_32K);"
call = """  /* pine-buds-cluster compute hook: run before the sysfreq drop */
  compute_main();
"""
text = open(path).read()
assert text.count(decl_anchor) == 1, "app_init definition not unique; re-check"
assert text.count(call_anchor) == 1, "sysfreq anchor not unique; re-check"
text = text.replace(decl_anchor, decl + decl_anchor)
text = text.replace(call_anchor, call + call_anchor)
open(path, "w").write(text)
EOF
    echo "[ok] compute_main() hooked into app_init (before 32K sysfreq drop)"
fi

echo
echo "Now build inside the dev container:"
echo "  cd external/OpenPineBuds && ./start_dev.sh   # then: ./build.sh"
