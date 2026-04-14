#!/bin/bash
set -euo pipefail
cd /home/quaxis/projects/tomorrow-bot
rm -f logs/production.log
mkdir -p logs

# Загрузка секретов из файла (НЕ хранить в скрипте!)
# Файл должен содержать: BITGET_API_KEY, BITGET_API_SECRET, BITGET_PASSPHRASE
SECRETS_FILE="${SECRETS_FILE:-/home/quaxis/projects/tomorrow-bot/.secrets/secrets.env}"
if [[ ! -f "$SECRETS_FILE" ]]; then
    echo "FATAL: Secrets file not found: $SECRETS_FILE" >&2
    echo "Create it with: BITGET_API_KEY=xxx / BITGET_API_SECRET=xxx / BITGET_PASSPHRASE=xxx" >&2
    exit 1
fi
set -a
source "$SECRETS_FILE"
set +a

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

timeout 36000 ./build/src/app/tomorrow-bot --config configs/production.yaml > logs/prod_stdout.log 2>&1
echo $? > logs/prod_exit_code.txt
date > logs/prod_done.flag
