#!/usr/bin/env python3
"""Entry quality analyzer — MFE/MAE and pre-entry context."""
import json
import datetime
import glob
from collections import defaultdict


def parse_ts(s):
    return datetime.datetime.fromisoformat(s.replace('Z', '+00:00'))


def main():
    market_by_sym = defaultdict(list)  # sym -> [(ts_obj, price, mom5, bb_pos, ema, adx)]
    fills = []  # (ts_obj, sym, side, entry, size, log_file)
    closes = []  # (ts_obj, sym, side, close_price, realized_pnl)

    for path in sorted(glob.glob('logs/run*.log')):
        try:
            with open(path) as f:
                for line in f:
                    try:
                        e = json.loads(line)
                    except Exception:
                        continue
                    msg = e.get('message', '')
                    ts = e.get('timestamp', '')
                    fields = e.get('fields', {}) or {}
                    sym = fields.get('symbol', '')
                    if not ts or not sym:
                        continue
                    try:
                        ts_obj = parse_ts(ts)
                    except Exception:
                        continue

                    if msg == 'Статус рынка':
                        try:
                            market_by_sym[sym].append((
                                ts_obj,
                                float(fields.get('price', 0)),
                                float(fields.get('mom5', 0)),
                                float(fields.get('bb_pos', 0)),
                                fields.get('ema', ''),
                                float(fields.get('adx', 0)),
                            ))
                        except Exception:
                            pass

                    if msg == 'Открыта позиция':
                        fills.append((ts_obj, sym, fields.get('side', ''),
                                      float(fields.get('entry_price', 0) or 0),
                                      float(fields.get('size', 0) or 0)))

                    if msg == 'Позиция полностью закрыта через reduce (hedge)':
                        closes.append((ts_obj, sym, fields.get('side', ''),
                                       float(fields.get('close_price', 0) or 0),
                                       float(fields.get('realized_pnl', 0) or 0)))
        except FileNotFoundError:
            continue

    # Pair fills with closes
    trades = []
    used = [False] * len(closes)
    for f_ts, f_sym, f_side, entry, size in fills:
        is_long = f_side in ('Buy', 'long', 'Long', 'BUY')
        for i, (c_ts, c_sym, c_side, close, pnl) in enumerate(closes):
            if used[i]:
                continue
            if c_sym != f_sym:
                continue
            c_long = c_side in ('long', 'Buy', 'BUY')
            if c_long != is_long:
                continue
            if c_ts < f_ts:
                continue
            trades.append({
                'open_ts': f_ts, 'close_ts': c_ts, 'sym': f_sym,
                'side': 'long' if is_long else 'short',
                'entry': entry, 'close': close, 'size': size, 'pnl': pnl
            })
            used[i] = True
            break

    # Analyze last 15 trades
    print(f'\nTotal matched trades: {len(trades)}\n')
    print('=== LAST 15 TRADES: MFE/MAE/pre-entry context ===\n')

    for t in trades[-15:]:
        sym = t['sym']
        snaps = market_by_sym.get(sym, [])
        # Snaps before and during trade
        pre_snaps = [s for s in snaps if (t['open_ts'] - s[0]).total_seconds() in range(0, 90)]
        during = [s for s in snaps if t['open_ts'] <= s[0] <= t['close_ts']]
        is_long = t['side'] == 'long'

        mfe = 0.0
        mae = 0.0
        mfe_at = None
        mae_at = None
        for s_ts, px, *_ in during:
            move_pct = (px - t['entry']) / t['entry'] * 100 if is_long \
                       else (t['entry'] - px) / t['entry'] * 100
            if move_pct > mfe:
                mfe = move_pct
                mfe_at = (s_ts - t['open_ts']).total_seconds()
            if move_pct < mae:
                mae = move_pct
                mae_at = (s_ts - t['open_ts']).total_seconds()

        hold_sec = (t['close_ts'] - t['open_ts']).total_seconds()
        move_final = (t['close'] - t['entry']) / t['entry'] * 100 if is_long \
                     else (t['entry'] - t['close']) / t['entry'] * 100

        # Pre-entry context (last snap before entry)
        prior = [s for s in snaps if s[0] < t['open_ts']]
        pre = prior[-1] if prior else None
        pre_str = ""
        if pre:
            pre_ts, pre_px, mom5, bbp, ema, adx = pre
            gap_sec = (t['open_ts'] - pre_ts).total_seconds()
            pre_str = f" pre[{gap_sec:.0f}s]: px={pre_px:.4f} mom5={mom5:+.4f} bb={bbp:+.2f} ema={ema} adx={adx:.0f}"

        timing = "post-impulse" if (is_long and mae_at is not None and mae_at < 30) \
                 else "OK timing"
        print(f'  {t["open_ts"].strftime("%H:%M:%S")} {sym:10s} {t["side"]:5s} entry={t["entry"]:.4f} '
              f'hold={hold_sec:.0f}s move_final={move_final:+.2f}% '
              f'MFE={mfe:+.2f}%@{mfe_at}s MAE={mae:+.2f}%@{mae_at}s pnl=${t["pnl"]:+.4f} [{timing}]')
        if pre_str:
            print(f'    {pre_str}')

    # Aggregate stats
    print('\n=== ENTRY QUALITY STATS (all matched) ===')
    instant_adverse = 0  # MAE within 30s
    chase = 0  # MFE < 0.1% and MAE < -0.3%
    for t in trades:
        sym = t['sym']
        snaps = market_by_sym.get(sym, [])
        during = [s for s in snaps if t['open_ts'] <= s[0] <= t['close_ts']]
        is_long = t['side'] == 'long'
        mae = 0; mfe = 0; mae_at = None
        for s_ts, px, *_ in during:
            move = (px - t['entry']) / t['entry'] * 100 if is_long \
                   else (t['entry'] - px) / t['entry'] * 100
            if move < mae:
                mae = move
                mae_at = (s_ts - t['open_ts']).total_seconds()
            if move > mfe:
                mfe = move
        if mae_at is not None and mae_at <= 30:
            instant_adverse += 1
        if mfe < 0.1 and mae < -0.3:
            chase += 1

    n = len(trades)
    if n > 0:
        print(f'  Total trades: {n}')
        print(f'  Instant adverse (MAE within 30s): {instant_adverse} ({instant_adverse/n*100:.0f}%)')
        print(f'  Chase pattern (MFE<0.1% AND MAE<-0.3%): {chase} ({chase/n*100:.0f}%)')


if __name__ == '__main__':
    main()
