#!/usr/bin/env bash
# Install the compute bring-up code into the OpenPineBuds checkout.
# Idempotent: safe to re-run after editing src/ or firmware/.
#
# What it does:
#   1. Copies kernel + app sources into apps/main/ (auto-globbed by its Makefile)
#   2. Defines PINEBUDS_TARGET for apps/main/ so compute_trace.h picks TRACE
#   3. Hooks compute_main() into app_init(), just BEFORE the sysfreq drop to
#      32K at the end of app_init (so the GEMM runs at full clock)
#   4. Copies the MPI/OpenMP adapters, the unmodified GEMM+MPI+OpenMP bench,
#      and the IBRT transport glue, flat into apps/main/ alongside the rest
#      (docs/design-ibrt-transport.md §6)
#   5. Adds -Iapps/main (so bench/gemm_mpi_omp.cpp's `#include <mpi.h>` /
#      `<omp.h>` angle-bracket includes resolve to the copies from step 4)
#      and -DGEMM_BENCH_NO_MAIN (disables the bench's own main(), same hook
#      tests/test_gemm_bench.cpp uses on host) to apps/main/Makefile
#   6. Patches apps.cpp to hold TWS up while docked/charging: disables the
#      5 s CLOSE_BOX re-injection and the 5 min auto-shutdown timer that
#      would otherwise undo the compute thread's forced OUT_BOX bring-up
#      (docs/design-ibrt-transport.md §11.2.3/§11.2.4). One-line `if (0 &&
#      ...)` guards, not #ifdef, to keep the diff minimal and warning-free
#   7. Patches config/open_source/target.mk to flip CHARGER_PLUGINOUT_RESET
#      from 1 to 0, so in-case power-on takes the SDK's CHARGING_PWRON path
#      and still runs BesbtInit (BES's own IBRT targets use 0). Side effect:
#      charger plug/unplug no longer resets the buds
#      (docs/design-ibrt-transport.md §12.4)
#   8. Patches services/app_ibrt/src/app_ibrt_search_pair_ui.cpp to neuter
#      the two box-event timer posts in
#      app_ibrt_battery_handle_process_normal() (PLUGIN and PLUGOUT
#      branches), so a debounced charger PLUGIN can no longer inject
#      IBRT_CLOSE_BOX_EVENT and tear down the TWS link mid-run. Box state
#      stays owned solely by the MPI glue's forced OUT_BOX (§11.2.3). Same
#      one-line `if (0 && ...)` guard style as step 6
#      (docs/design-ibrt-transport.md §12.8)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${REPO_ROOT}/external/OpenPineBuds"
DST="${SDK_DIR}/apps/main"
APPS_CPP="${DST}/apps.cpp"
TARGET_MK="${SDK_DIR}/config/open_source/target.mk"
SEARCH_PAIR_UI="${SDK_DIR}/services/app_ibrt/src/app_ibrt_search_pair_ui.cpp"
MARKER="pine-buds-cluster compute hook"
MPI_MARKER="pine-buds-cluster MPI/IBRT hook"
TWS_HOLD_MARKER="pine-buds-cluster in-case TWS hold"
CHARGE_MARKER="pine-buds-cluster charging poweron"
BOX_EVENT_MARKER="pine-buds-cluster charger box events"

[ -d "${DST}" ] || { echo "error: run scripts/setup-openpinebuds.sh first"; exit 1; }

cp "${REPO_ROOT}"/src/gemm.h "${REPO_ROOT}"/src/gemm.cpp \
   "${REPO_ROOT}"/src/gemm_selftest.h "${REPO_ROOT}"/src/gemm_selftest.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/compute_main.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/compute_trace.h \
   "${DST}/"
echo "[ok] sources copied into apps/main/"

cp "${REPO_ROOT}"/adapters/mpi/mpi.h "${REPO_ROOT}"/adapters/mpi/mpi_adapter.h \
   "${REPO_ROOT}"/adapters/mpi/mpi_frag.h "${REPO_ROOT}"/adapters/mpi/mpi_core.cpp \
   "${REPO_ROOT}"/adapters/mpi/mpi_frag.cpp \
   "${REPO_ROOT}"/adapters/omp/omp.h "${REPO_ROOT}"/adapters/omp/omp_stub.cpp \
   "${REPO_ROOT}"/bench/gemm_mpi_omp.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/mpi_ibrt_glue.h \
   "${REPO_ROOT}"/firmware/pinebuds_compute/mpi_ibrt_glue.cpp \
   "${DST}/"
echo "[ok] MPI/OpenMP adapters + bench + IBRT glue copied into apps/main/"

if ! grep -q "PINEBUDS_TARGET" "${DST}/Makefile"; then
    printf '\n# %s\nccflags-y += -DPINEBUDS_TARGET\n' "${MARKER}" >> "${DST}/Makefile"
    echo "[ok] -DPINEBUDS_TARGET added to apps/main/Makefile"
else
    echo "[ok] apps/main/Makefile already defines PINEBUDS_TARGET"
fi

if ! grep -q "${MPI_MARKER}" "${DST}/Makefile"; then
    printf '\n# %s\nccflags-y += -Iapps/main -DGEMM_BENCH_NO_MAIN\n' \
        "${MPI_MARKER}" >> "${DST}/Makefile"
    echo "[ok] -Iapps/main -DGEMM_BENCH_NO_MAIN added to apps/main/Makefile"
else
    echo "[ok] apps/main/Makefile already has the MPI/IBRT build flags"
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

if grep -q "${TWS_HOLD_MARKER}" "${APPS_CPP}"; then
    echo "[ok] in-case TWS hold patch already present"
else
    python3 - "${APPS_CPP}" <<'EOF'
import sys

path = sys.argv[1]
# docs/design-ibrt-transport.md §11.2.4: hold TWS up while docked/charging
# so the compute thread's forced OUT_BOX bring-up (§11.2.3) survives the
# SDK's own 5 s box-state poll and 5 min auto-shutdown timer. Target 3
# (charger-plugin box event) is deliberately NOT patched here -- §11.2.4
# says to add it only if "box event:4" shows up in re-test logs. It did
# (Run 3, §12.8), so step 8 below patches it in search_pair_ui.cpp.
target1_old = "  if (app_battery_is_charging()) {"
target1_new = """  /* pine-buds-cluster in-case TWS hold */
  if (0 && app_battery_is_charging()) {"""

target2_old = "      if (auto_shutdown_cnt == Auto_Shutdowm_TIME / 5) {"
target2_new = """      /* pine-buds-cluster in-case TWS hold */
      if (0 && auto_shutdown_cnt == Auto_Shutdowm_TIME / 5) {"""

text = open(path).read()
assert text.count(target1_old) == 1, "apps.cpp:1502 anchor not unique; re-check"
assert text.count(target2_old) == 1, "apps.cpp:1517 anchor not unique; re-check"
text = text.replace(target1_old, target1_new)
text = text.replace(target2_old, target2_new)
open(path, "w").write(text)
EOF
    echo "[ok] in-case TWS hold patched (CLOSE_BOX re-injection + auto-shutdown disabled)"
fi

if grep -q -- "-DCHARGER_PLUGINOUT_RESET=0" "${TARGET_MK}"; then
    echo "[ok] ${CHARGE_MARKER}: target.mk already patched"
else
    python3 - "${TARGET_MK}" <<'EOF'
import sys

path = sys.argv[1]
# docs/design-ibrt-transport.md §12.4: KBUILD_CPPFLAGS += is a hard append,
# so overriding the value from the command line would just redefine the
# macro -- target.mk itself has to change. The line sits inside a
# backslash line-continuation, so (unlike the apps.cpp hooks above) no
# marker comment is inserted here: a bare "#" line would end the
# continuation early and break the build.
old = "    -DCHARGER_PLUGINOUT_RESET=1 \\\n"
new = "    -DCHARGER_PLUGINOUT_RESET=0 \\\n"

text = open(path).read()
assert text.count(old) == 1, "CHARGER_PLUGINOUT_RESET=1 not unique; re-check"
text = text.replace(old, new)
open(path, "w").write(text)
EOF
    echo "[ok] pine-buds-cluster charging poweron: CHARGER_PLUGINOUT_RESET 1 -> 0 patched into target.mk"
fi

if grep -q "${BOX_EVENT_MARKER}" "${SEARCH_PAIR_UI}"; then
    echo "[ok] charger box-event patch already present"
else
    python3 - "${SEARCH_PAIR_UI}" <<'EOF'
import sys

path = sys.argv[1]
# docs/design-ibrt-transport.md §12.8: neuter both box-event timer posts in
# app_ibrt_battery_handle_process_normal() (PLUGIN and PLUGOUT branches) so
# a debounced charger PLUGIN can no longer inject IBRT_CLOSE_BOX_EVENT and
# tear down the TWS link mid-run. Box state stays owned solely by the MPI
# glue's forced OUT_BOX (§11.2.3).
old = """      if (app_box_handle_timer != NULL) {
        osTimerStop(app_box_handle_timer);
        osTimerStart(app_box_handle_timer, 500);
      }
"""
new = """      if (0 && app_box_handle_timer != NULL) { /* pine-buds-cluster charger box events */
        osTimerStop(app_box_handle_timer);
        osTimerStart(app_box_handle_timer, 500);
      }
"""

text = open(path).read()
assert text.count(old) == 2, "app_box_handle_timer block not found exactly twice; re-check"
text = text.replace(old, new)
open(path, "w").write(text)
EOF
    echo "[ok] charger box-event injection patched (PLUGIN + PLUGOUT timer posts disabled)"
fi

echo
echo "Now build inside the dev container:"
echo "  cd external/OpenPineBuds && ./start_dev.sh   # then: ./build.sh"
