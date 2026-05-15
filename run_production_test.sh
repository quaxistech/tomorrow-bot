#!/bin/bash
set -euo pipefail

cd /home/quaxis/projects/tomorrow-bot
rm -f logs/production.log
mkdir -p logs

# Загрузка секретов из файла (НЕ хранить в скрипте!)
SECRETS_FILE="${SECRETS_FILE:-/home/quaxis/projects/tomorrow-bot/.secrets/secrets.env}"
if [[ ! -f "$SECRETS_FILE" ]]; then
    echo "FATAL: Secrets file not found: $SECRETS_FILE" >&2
    echo "Create it with: BITGET_API_KEY=xxx / BITGET_API_SECRET=xxx / BITGET_PASSPHRASE=xxx" >&2
    exit 1
fi
set -a
source "$SECRETS_FILE"
set +a

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

BINARY="${TOMORROW_BOT_BINARY:-./build-release/src/app/tomorrow-bot}"
if [[ ! -x "$BINARY" ]]; then
    echo "FATAL: Release binary not found or not executable: $BINARY" >&2
    echo "Build a release binary first: ./scripts/build_release.sh" >&2
    exit 1
fi

timeout 600 "$BINARY" --config configs/production.yaml
echo "BOT_EXIT_CODE=$?"
