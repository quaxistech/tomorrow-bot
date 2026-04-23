#!/bin/bash
set -euo pipefail
cd /home/quaxis/projects/tomorrow-bot
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

BINARY="${TOMORROW_BOT_BINARY:-./build-release/src/app/tomorrow-bot}"
CONFIG="configs/production.yaml"
RESTART_DELAY_SEC="${BOT_RESTART_DELAY_SEC:-5}"
MAX_RUNTIME_SEC="${BOT_MAX_RUNTIME_SEC:-0}"

if [[ ! -x "$BINARY" ]]; then
    echo "FATAL: Production binary not found or not executable: $BINARY" >&2
    echo "Build a release binary first: ./scripts/build_release.sh" >&2
    exit 1
fi

while true; do
    start_ts="$(date -Is)"
    echo "[$start_ts] starting production bot" | tee -a logs/prod_supervisor.log

    set +e
    if [[ "$MAX_RUNTIME_SEC" != "0" ]]; then
        timeout "$MAX_RUNTIME_SEC" "$BINARY" --config "$CONFIG" >> logs/prod_stdout.log 2>&1
        exit_code=$?
    else
        "$BINARY" --config "$CONFIG" >> logs/prod_stdout.log 2>&1
        exit_code=$?
    fi
    set -e

    echo "$exit_code" > logs/prod_exit_code.txt
    date -Is > logs/prod_done.flag
    echo "[$(date -Is)] bot exited with code=$exit_code" | tee -a logs/prod_supervisor.log

    # Clean exit or timeout: stop the supervisor loop.
    if [[ $exit_code -eq 0 || $exit_code -eq 124 ]]; then
        exit $exit_code
    fi

    echo "[$(date -Is)] restarting in ${RESTART_DELAY_SEC}s" | tee -a logs/prod_supervisor.log
    sleep "$RESTART_DELAY_SEC"
done
