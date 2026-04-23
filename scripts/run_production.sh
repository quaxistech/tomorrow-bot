#!/usr/bin/env bash
# run_production.sh — Запуск Tomorrow Bot в production-режиме
# ВНИМАНИЕ: этот скрипт запускает РЕАЛЬНУЮ торговлю!
# A7 fix: унифицирован с корневым run_prod.sh — те же guardrails.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

# Загрузка секретов из файла (НЕ хранить в .env рядом с кодом!)
SECRETS_FILE="${SECRETS_FILE:-/home/quaxis/projects/tomorrow-bot/.secrets/secrets.env}"
if [[ ! -f "$SECRETS_FILE" ]]; then
    echo "FATAL: Secrets file not found: $SECRETS_FILE" >&2
    echo "Create it with: BITGET_API_KEY=xxx / BITGET_API_SECRET=xxx / BITGET_PASSPHRASE=xxx" >&2
    exit 1
fi
set -a
source "$SECRETS_FILE"
set +a
echo "✓ Секреты загружены из $SECRETS_FILE"

# Проверка обязательных переменных
for var in BITGET_API_KEY BITGET_API_SECRET BITGET_PASSPHRASE; do
    if [[ -z "${!var:-}" ]]; then
        echo "FATAL: $var not set in $SECRETS_FILE" >&2
        exit 1
    fi
done

# Production guard: оператор должен подтвердить ТОЧНЫМ токеном
if [[ "${TOMORROW_BOT_PRODUCTION_CONFIRM:-}" != "I_UNDERSTAND_LIVE_TRADING" ]]; then
    echo "═══════════════════════════════════════════════════════════════════"
    echo "ВНИМАНИЕ: Запуск PRODUCTION режима с РЕАЛЬНЫМИ ДЕНЬГАМИ!"
    echo "Для подтверждения установите переменную окружения:"
    echo "  export TOMORROW_BOT_PRODUCTION_CONFIRM=I_UNDERSTAND_LIVE_TRADING"
    echo "═══════════════════════════════════════════════════════════════════"
    exit 1
fi

# Production запускается только на release binary.
BINARY="${TOMORROW_BOT_BINARY:-build-release/src/app/tomorrow-bot}"
if [[ ! -x "$BINARY" ]]; then
    echo "✗ ОШИБКА: Release-бинарник не найден: $BINARY"
    echo "  Соберите проект: ./scripts/build_release.sh"
    exit 1
fi
echo "✓ Используется Release-сборка: $BINARY"

# Создание директории для логов
mkdir -p logs

CONFIG="configs/production.yaml"

echo ""
echo "╔══════════════════════════════════════════╗"
echo "║  ⚠  PRODUCTION MODE — РЕАЛЬНЫЕ ДЕНЬГИ   ║"
echo "╠══════════════════════════════════════════╣"
echo "║  Конфиг: $CONFIG"
echo "║  Бинарник: $BINARY"
echo "║  Логи: logs/production.log"
echo "╚══════════════════════════════════════════╝"
echo ""
echo "Запуск через 3 секунды... (Ctrl+C для отмены)"
sleep 3

exec "$BINARY" --config "$CONFIG"
