#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════════
# promotion_gate.sh — Pre-deployment gate: все обязательные тест-паки
#
# Блокирует продакшн-деплой если хоть один обязательный пакет тестов не проходит.
# Запуск: ./scripts/promotion_gate.sh [build_dir]
#
# Exit codes:
#   0 — все паки зелёные, деплой разрешён
#   1 — хотя бы один пакет тестов красный
# ═══════════════════════════════════════════════════════════════════════════════
set -euo pipefail

BUILD_DIR="${1:-build}"
RESULTS=()
FAILED=0

log_header() {
    echo ""
    echo "════════════════════════════════════════════════════════════════════"
    echo "  PROMOTION GATE: $1"
    echo "════════════════════════════════════════════════════════════════════"
}

run_pack() {
    local pack_name="$1"
    local filter="$2"

    log_header "$pack_name"

    if cd "$BUILD_DIR" && ctest --output-on-failure -j"$(nproc)" -R "$filter" 2>&1; then
        echo "  ✓ $pack_name: PASSED"
        RESULTS+=("✓ $pack_name")
    else
        echo "  ✗ $pack_name: FAILED"
        RESULTS+=("✗ $pack_name")
        FAILED=1
    fi
    cd - > /dev/null
}

echo "╔═══════════════════════════════════════════════════════════════════╗"
echo "║  Tomorrow Bot — Promotion Gate                                   ║"
echo "║  $(date '+%Y-%m-%d %H:%M:%S')                                              ║"
echo "╚═══════════════════════════════════════════════════════════════════╝"

# ─── Pack 1: Exit & Hedge Core ─────────────────────────────────────────────
run_pack "Exit Orchestrator" "ExitOrchestrator"
run_pack "Hedge Pair Manager" "HedgePairManager"
run_pack "Pair Economics" "PairEconomics"

# ─── Pack 2: Risk & Resilience ─────────────────────────────────────────────
run_pack "Risk Engine" "test_risk"
run_pack "Operational Guard" "OperationalGuard"
run_pack "Circuit Breaker" "circuit_breaker"
run_pack "Self-Diagnosis" "SelfCheck|self_check"

# ─── Pack 3: Execution Quality ────────────────────────────────────────────
run_pack "Execution Engine" "test_execution"
run_pack "Order Mapping" "OrderMapping"

# ─── Pack 4: Reconciliation ───────────────────────────────────────────────
run_pack "Reconciliation" "test_reconciliation"
run_pack "Recovery" "test_recovery"

# ─── Pack 5: Fault Injection ──────────────────────────────────────────────
run_pack "Fault Injection" "FaultInjection"

# ─── Pack 6: Scenario Integration ──────────────────────────────────────────
run_pack "Scenario Integration" "test_scenario"

# ─── Summary ───────────────────────────────────────────────────────────────
echo ""
echo "╔═══════════════════════════════════════════════════════════════════╗"
echo "║  SUMMARY                                                         ║"
echo "╠═══════════════════════════════════════════════════════════════════╣"
for r in "${RESULTS[@]}"; do
    printf "║  %-63s║\n" "$r"
done
echo "╠═══════════════════════════════════════════════════════════════════╣"

if [[ $FAILED -eq 0 ]]; then
    echo "║  VERDICT: ✓ ALL PACKS PASSED — deployment authorized             ║"
    echo "╚═══════════════════════════════════════════════════════════════════╝"
    exit 0
else
    echo "║  VERDICT: ✗ GATE FAILED — deployment BLOCKED                     ║"
    echo "╚═══════════════════════════════════════════════════════════════════╝"
    exit 1
fi
