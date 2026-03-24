# Проектирование защиты от враждебных рыночных условий

## Обзор

Модуль `AdversarialMarketDefense` обнаруживает и реагирует на 6 типов
рыночных угроз, защищая систему от потерь в экстремальных условиях.

---

## 1. Типы угроз

### 1.1 UnstableOrderBook — Нестабильный стакан

- **Признаки**: высокий `book_instability` (> 0.7), невалидный стакан
- **Действие**: `VetoTrade` — запрет торговли
- **Серьёзность**: пропорциональна `book_instability`

### 1.2 SpreadExplosion — Взрыв спреда

- **Признаки**: `spread_bps` > порога (по умолчанию 100 bps)
- **Действие**: `VetoTrade` при критическом спреде, `RaiseThreshold` при повышенном
- **Серьёзность**: `(spread - normal) / (threshold - normal)`

### 1.3 LiquidityVacuum — Вакуум ликвидности

- **Признаки**: `bid_depth` или `ask_depth` < минимальной глубины (50)
- **Действие**: `VetoTrade`
- **Серьёзность**: обратно пропорциональна глубине

### 1.4 ToxicFlow — Токсичный поток ордеров

- **Признаки**: `buy_sell_ratio` > порога (0.85) при дисбалансе стакана
- **Действие**: `ReduceConfidence`
- **Серьёзность**: пропорциональна отклонению от нормы

### 1.5 BadBreakoutTrap — Ловушка ложного пробоя

- **Признаки**: комбинация высокого спреда, дисбаланса и нестабильности
- **Действие**: `RaiseThreshold`
- **Серьёзность**: средняя

### 1.6 PostShockCooldown — Пост-шоковое охлаждение

- **Признаки**: предыдущий шок зарегистрирован, cooldown не истёк
- **Действие**: `Cooldown` — ожидание нормализации
- **Длительность**: 30-60 секунд (настраиваемо)

---

## 2. Логика обнаружения

```
assess(MarketCondition) → DefenseAssessment
  1. Проверить cooldown (PostShockCooldown)
  2. Обнаружить SpreadExplosion
  3. Обнаружить LiquidityVacuum
  4. Обнаружить UnstableBook
  5. Обнаружить ToxicFlow
  6. Обнаружить BadBreakoutTrap
  7. Агрегировать угрозы → выбрать максимальное действие
  8. Рассчитать confidence_multiplier и threshold_multiplier
```

### Приоритет действий

```
VetoTrade > Cooldown > AlertOperator > RaiseThreshold > ReduceConfidence > NoAction
```

---

## 3. Рекомендации по эксплуатации

- Мониторить `tb_threats_detected` по типам
- Настроить алерты на `VetoTrade` > 10 за 5 минут
- После шторма угроз — проверить лог и скорректировать пороги
- Cooldown не отключать — это критический механизм безопасности

---

## 4. Конфигурация

```yaml
adversarial_defense:
  spread_explosion_threshold_bps: 100.0
  spread_normal_bps: 20.0
  min_liquidity_depth: 50.0
  book_imbalance_threshold: 0.8
  book_instability_threshold: 0.7
  toxic_flow_threshold: 0.85
  cooldown_duration_ms: 30000
  post_shock_cooldown_ms: 60000
```

### Настройка порогов

- **Консервативный** профиль: снизить пороги на 20%
- **Агрессивный** профиль: повысить пороги на 30%
- **Рекомендация**: начинать с консервативного профиля

---

## 5. Интеграция

- Вызывается перед каждым торговым решением
- Результат (`DefenseAssessment`) учитывается в `DecisionEngine`
- Шоки регистрируются для cooldown
- Все угрозы логируются для анализа
