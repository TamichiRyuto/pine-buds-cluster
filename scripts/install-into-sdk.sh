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
#   9. Copies the SPP log ring + service (log_ring.{h,cpp}, spp_log_service.{h,cpp},
#      spp_tx_fsm.{h,cpp} for the send state machine) flat into apps/main/
#      alongside the rest (same as step 4), and patches
#      services/bt_app/besmain.cpp to call spp_log_service_init() on
#      BesbtThread, just before the event loop -- the same spot the SDK's
#      own app_tota_init() runs from (docs/design-ibrt-transport.md §13.8)
#  10. Patches apps/battery/app_battery.cpp so a full battery no longer
#      calls app_shutdown() while docked: the buds only run in the case
#      (charging power-on), and a full charge killed every run 15-60 s
#      after boot -- too short for the PC to open the SPP link. The
#      FULLCHARGE indication is kept; only the shutdown is guarded out with
#      the same `if (0 && ...)` style as steps 6 and 8. Hardware charge
#      termination is unaffected (the PMU charger cuts off on its own)
#  11. Adds an opt-in to apps/main/Makefile: `make ... SPP_LOG_PAIRING=1`
#      defines SPP_LOG_ACCESS_MODE=BTIF_BAM_GENERAL_ACCESSIBLE so the SPP
#      log re-arm (§13.4.3) also turns inquiry scan on, letting Windows
#      discover the buds for a fresh pairing (§13.7 step 0). Off by default
#  12. Copies run_trigger.{h,cpp} (design §14) and patches
#      apps/main/key_handler.cpp so a 5-tap (APP_KEY_EVENT_RAMPAGECLICK,
#      unbound in the fork's key table) calls mpi_ibrt_trigger_run(),
#      re-running GEMM-MPI on both buds. Single tap stays play/pause
#  13. OpenMP Stage 2 on the second core (docs/design-ibrt-transport.md §15):
#      copies omp_adapter.h + omp_cp_port.{h,cpp}, compiles ONLY
#      gemm_mpi_omp.o with -fopenmp (CFLAGS_<obj>.o hook, scripts/lib.mk:138)
#      so GCC outlines the bench's `#pragma omp parallel for` into a call to
#      our GOMP_parallel, and patches scripts/link/best1000.lds.S to place
#      gemm_mpi_omp.o's code in .cp_text_sram next to the SDK's own
#      `*:cp_queue.o(.text*)` rule -- the CP's MPU denies flash, so the
#      outlined loop body must live in CP RAM
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${REPO_ROOT}/external/OpenPineBuds"
DST="${SDK_DIR}/apps/main"
APPS_CPP="${DST}/apps.cpp"
TARGET_MK="${SDK_DIR}/config/open_source/target.mk"
SEARCH_PAIR_UI="${SDK_DIR}/services/app_ibrt/src/app_ibrt_search_pair_ui.cpp"
BESMAIN_CPP="${SDK_DIR}/services/bt_app/besmain.cpp"
BATTERY_CPP="${SDK_DIR}/apps/battery/app_battery.cpp"
KEY_HANDLER_CPP="${DST}/key_handler.cpp"
MARKER="pine-buds-cluster compute hook"
FULL_CHARGE_MARKER="pine-buds-cluster full-charge keep-alive"
PAIRING_MARKER="pine-buds-cluster SPP pairing build"
TAP_MARKER="pine-buds-cluster 5-tap run"
MPI_MARKER="pine-buds-cluster MPI/IBRT hook"
TWS_HOLD_MARKER="pine-buds-cluster in-case TWS hold"
CHARGE_MARKER="pine-buds-cluster charging poweron"
BOX_EVENT_MARKER="pine-buds-cluster charger box events"
SPP_LOG_MARKER="pine-buds-cluster SPP log"
OMP_MARKER="pine-buds-cluster OpenMP CP text"
LDS_FILE="${SDK_DIR}/scripts/link/best1000.lds.S"

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
   "${REPO_ROOT}"/adapters/omp/omp.h "${REPO_ROOT}"/adapters/omp/omp_adapter.h \
   "${REPO_ROOT}"/adapters/omp/omp_runtime.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/omp_cp_port.h \
   "${REPO_ROOT}"/firmware/pinebuds_compute/omp_cp_port.cpp \
   "${REPO_ROOT}"/bench/gemm_mpi_omp.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/mpi_ibrt_glue.h \
   "${REPO_ROOT}"/firmware/pinebuds_compute/mpi_ibrt_glue.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/log_ring.h \
   "${REPO_ROOT}"/firmware/pinebuds_compute/log_ring.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/spp_log_service.h \
   "${REPO_ROOT}"/firmware/pinebuds_compute/spp_log_service.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/run_trigger.h \
   "${REPO_ROOT}"/firmware/pinebuds_compute/run_trigger.cpp \
   "${REPO_ROOT}"/firmware/pinebuds_compute/spp_tx_fsm.h \
   "${REPO_ROOT}"/firmware/pinebuds_compute/spp_tx_fsm.cpp \
   "${DST}/"
echo "[ok] MPI/OpenMP adapters + bench + IBRT glue + SPP log service copied into apps/main/"

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

if ! grep -q "${PAIRING_MARKER}" "${DST}/Makefile"; then
    # Command-line make variables reach sub-makes through MAKEFLAGS, so
    # `make T=open_source ... SPP_LOG_PAIRING=1` is visible here.
    printf '\n# %s (docs/design-ibrt-transport.md §13.7 step 0)\nifeq ($(SPP_LOG_PAIRING),1)\nccflags-y += -DSPP_LOG_ACCESS_MODE=BTIF_BAM_GENERAL_ACCESSIBLE\nendif\n' \
        "${PAIRING_MARKER}" >> "${DST}/Makefile"
    echo "[ok] SPP_LOG_PAIRING=1 opt-in added to apps/main/Makefile"
else
    echo "[ok] apps/main/Makefile already has the SPP pairing opt-in"
fi

if ! grep -q "${OMP_MARKER}" "${DST}/Makefile"; then
    # Per-object flag (scripts/lib.mk:138). Compile-only: -fopenmp never
    # reaches the link line, so no -lgomp is requested; GOMP_parallel comes
    # from omp_runtime.cpp (design §15.2 items 1-2).
    printf '\n# %s (docs/design-ibrt-transport.md §15)\nCFLAGS_gemm_mpi_omp.o += -fopenmp\n' \
        "${OMP_MARKER}" >> "${DST}/Makefile"
    echo "[ok] -fopenmp for gemm_mpi_omp.o added to apps/main/Makefile"
else
    echo "[ok] apps/main/Makefile already compiles gemm_mpi_omp.o with -fopenmp"
fi

if grep -q "${OMP_MARKER}" "${LDS_FILE}"; then
    echo "[ok] linker script already places gemm_mpi_omp.o in .cp_text_sram"
else
    python3 - "${LDS_FILE}" <<'EOF'
import sys

path = sys.argv[1]
# docs/design-ibrt-transport.md §15.2 item 4: the CP cannot execute from
# flash, so the whole bench object (gemm_bench_run and the outlined
# gemm_bench_run._omp_fn.0 GCC emits for -fopenmp) goes to CP RAM, using the
# SDK's own object-level idiom right next to it.
anchor = "\t\t*:cp_queue.o(.text*)\n"
addition = anchor + "\t\t*:gemm_mpi_omp.o(.text*) /* pine-buds-cluster OpenMP CP text */\n"

text = open(path).read()
assert text.count(anchor) == 1, "cp_queue.o .cp_text_sram rule not unique; re-check"
text = text.replace(anchor, addition)
open(path, "w").write(text)
EOF
    echo "[ok] gemm_mpi_omp.o placed in .cp_text_sram (scripts/link/best1000.lds.S)"
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

if grep -q "${SPP_LOG_MARKER}" "${BESMAIN_CPP}"; then
    echo "[ok] SPP log service hook already present"
else
    python3 - "${BESMAIN_CPP}" <<'EOF'
import sys

path = sys.argv[1]
# docs/design-ibrt-transport.md §13.8: register the SPP log service on
# BesbtThread, just before the event loop -- the same spot the SDK's own
# app_tota_init() runs from (besmain.cpp:448-449). extern "C" is a linkage
# specification: it must live at file scope, not inside a block, so declare
# above besmain() and call inside it (same pattern as the compute_main hook
# above).
decl_anchor = "int besmain(void) {"
decl = 'extern "C" void spp_log_service_init(void); /* pine-buds-cluster SPP log */\n\n'
call_anchor = "  osapi_notify_evm();"
call = """  /* pine-buds-cluster SPP log */
  spp_log_service_init();
"""

text = open(path).read()
assert text.count(decl_anchor) == 1, "besmain definition not unique; re-check"
assert text.count(call_anchor) == 1, "osapi_notify_evm anchor not unique; re-check"
text = text.replace(decl_anchor, decl + decl_anchor)
text = text.replace(call_anchor, call + call_anchor)
open(path, "w").write(text)
EOF
    echo "[ok] spp_log_service_init() hooked into besmain (before the event loop)"
fi

if grep -q "${FULL_CHARGE_MARKER}" "${BATTERY_CPP}"; then
    echo "[ok] full-charge keep-alive patch already present"
else
    python3 - "${BATTERY_CPP}" <<'EOF'
import sys

path = sys.argv[1]
# app_battery_handle_process_charging(): once the charger reports full,
# the SDK sets the FULLCHARGE indication and calls app_shutdown(). Keep the
# indication (it also guards this block from re-running), drop the
# shutdown. Same `if (0 && ...)` guard style as the apps.cpp patches.
old = """      app_status_indication_set(APP_STATUS_INDICATION_FULLCHARGE);
      app_shutdown();
"""
new = """      app_status_indication_set(APP_STATUS_INDICATION_FULLCHARGE);
      if (0) app_shutdown(); /* pine-buds-cluster full-charge keep-alive */
"""

text = open(path).read()
assert text.count(old) == 1, "FULLCHARGE app_shutdown block not unique; re-check"
text = text.replace(old, new)
open(path, "w").write(text)
EOF
    echo "[ok] full-charge app_shutdown() disabled in apps/battery/app_battery.cpp"
fi

if grep -q "${TAP_MARKER}" "${KEY_HANDLER_CPP}"; then
    echo "[ok] 5-tap run hook already present"
else
    python3 - "${KEY_HANDLER_CPP}" <<'EOF'
import sys

path = sys.argv[1]
# docs/design-ibrt-transport.md §14: bind the unused 5-tap gesture to the
# MPI re-run. app_key_handle_registration() overwrites an existing
# (code, event) slot instead of appending, so a new (PWR, RAMPAGECLICK)
# entry is the only way to add a gesture without stealing play/pause.
# The handler runs on app_thread; mpi_ibrt_trigger_run() never blocks.
decl_anchor = "void app_key_init(void) {"
decl = """extern "C" void mpi_ibrt_trigger_run(void); /* pine-buds-cluster 5-tap run */

void app_key_compute_run(APP_KEY_STATUS *status, void *param) {
  TRACE(2, "%s event %d", __func__, status->event);
  mpi_ibrt_trigger_run();
}

"""
entry_anchor = """      {{APP_KEY_CODE_PWR, APP_KEY_EVENT_LONGPRESS},
       "",
       app_key_long_press_down,
       NULL},
"""
entry = entry_anchor + """      {{APP_KEY_CODE_PWR, APP_KEY_EVENT_RAMPAGECLICK}, /* pine-buds-cluster 5-tap run */
       "",
       app_key_compute_run,
       NULL},
"""

text = open(path).read()
assert text.count(decl_anchor) == 1, "app_key_init definition not unique; re-check"
assert text.count(entry_anchor) == 1, "LONGPRESS key_cfg entry not unique; re-check"
text = text.replace(decl_anchor, decl + decl_anchor)
text = text.replace(entry_anchor, entry)
open(path, "w").write(text)
EOF
    echo "[ok] 5-tap (RAMPAGECLICK) -> mpi_ibrt_trigger_run() added to apps/main/key_handler.cpp"
fi

echo
echo "Now build inside the dev container:"
echo "  cd external/OpenPineBuds && ./start_dev.sh   # then: ./build.sh"
echo "For a one-off Windows pairing build add SPP_LOG_PAIRING=1 to make (step 11)."
