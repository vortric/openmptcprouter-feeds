#!/usr/bin/env python3
"""Post-process an omr-survey session off the router.

For every sample row in survey.jsonl, decode the GNSS fix (if the row has an
omr-gnss `gnss` payload) and join mqvpn's per-path transport metrics
(`mqvpn.status.clients[].paths[]`, keyed by xquic path_id) with the
interface table (`mqvpn.paths.path_info[]`, iface <-> path_id) and, when the
same netdev appears as `device` in the OMR metrics, with that WAN's
omr-tracker record. One output row per (sample, live path).

The router stores raw payloads only; every reshaping step lives here.

Usage:
    survey_join.py survey.jsonl                 # JSONL on stdout
    survey_join.py survey.jsonl --csv out.csv   # flat CSV
    survey_join.py survey.jsonl --summary       # per-iface summary to stderr
"""

import argparse
import csv
import json
import statistics
import sys


def load_rows(path):
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                yield lineno, json.loads(line)
            except json.JSONDecodeError as e:
                print(f"{path}:{lineno}: skipping unparsable row: {e}", file=sys.stderr)


def path_id_to_iface(mq):
    """iface keyed by xquic path_id, live entries only (see control-api.md §5.10)."""
    out = {}
    paths = (mq or {}).get("paths") or {}
    for e in paths.get("path_info") or []:
        if e.get("live") and e.get("path_id") is not None:
            out[int(e["path_id"])] = e
    return out


def omr_by_device(omr):
    out = {}
    for rec in (omr or {}).get("interfaces") or []:
        dev = rec.get("device")
        if dev:
            out[dev] = rec
    return out


def flatten_omr(rec):
    """Pick a stable subset of omr-tracker fields; keep the raw record too."""
    if not rec:
        return {}
    sig = rec.get("signal") or {}
    return {
        "omr_interface": rec.get("interface"),
        "omr_status": rec.get("status"),
        "omr_latency": rec.get("latency"),
        "omr_loss": rec.get("loss"),
        "omr_signal_type": sig.get("type"),
        "omr_rssi": sig.get("rssi"),
        "omr_rsrp": sig.get("rsrp"),
        "omr_rsrq": sig.get("rsrq"),
        "omr_sinr": sig.get("sinr"),
        "omr_rx_bps": rec.get("rx_bps"),
        "omr_tx_bps": rec.get("tx_bps"),
    }


def _nmea_fields(sent):
    """'$GNRMC,...*hh' -> list of fields (talker/type first), or None."""
    if not sent or not sent.startswith("$"):
        return None
    body = sent[1:].split("*", 1)[0]
    return body.split(",")


def _dm_to_deg(v, hemi):
    """NMEA ddmm.mmmm / dddmm.mmmm -> signed decimal degrees."""
    if not v:
        return None
    try:
        f = float(v)
    except ValueError:
        return None
    deg = int(f // 100)
    minutes = f - deg * 100
    d = deg + minutes / 60.0
    return -d if hemi in ("S", "W") else d


def _num(v):
    try:
        return float(v) if v not in (None, "") else None
    except ValueError:
        return None


def decode_gnss(g, row_mono_ns=None):
    """Latest-fix summary from omr-gnssd's raw sentence table (survey row 'gnss').

    Decodes RMC (time/date, status, speed, course), GGA (fix quality, sats,
    HDOP, altitude), GST (1-sigma lat/lon/alt error) and VTG (km/h). Also
    reports how old the newest RMC was when the survey row was sampled.
    """
    out = {"gnss_connected": None}
    if not g:
        return out
    out["gnss_connected"] = g.get("connected")
    sents = g.get("sentences") or {}

    def pick(typ):
        for talker in ("GN", "GP", "GA", "GB", "GL", "GQ"):
            e = sents.get(talker + typ)
            if e:
                return e
        return None

    rmc = pick("RMC")
    if rmc:
        f = _nmea_fields(rmc["nmea"])
        if f and len(f) >= 10:
            out["gnss_rmc_status"] = f[2]
            out["gnss_lat"] = _dm_to_deg(f[3], f[4])
            out["gnss_lon"] = _dm_to_deg(f[5], f[6])
            kn = _num(f[7])
            out["gnss_speed_kmh"] = round(kn * 1.852, 3) if kn is not None else None
            out["gnss_course_deg"] = _num(f[8])
            t, d = f[1], f[9]
            if len(t) >= 6 and len(d) == 6:
                out["gnss_utc"] = f"20{d[4:6]}-{d[2:4]}-{d[0:2]}T{t[0:2]}:{t[2:4]}:{t[4:]}Z"
            if len(f) >= 13:
                out["gnss_rmc_mode"] = f[12]
        if row_mono_ns is not None and rmc.get("rx_monotonic_ns"):
            out["gnss_fix_age_ms"] = round((row_mono_ns - rmc["rx_monotonic_ns"]) / 1e6, 1)
    gga = pick("GGA")
    if gga:
        f = _nmea_fields(gga["nmea"])
        if f and len(f) >= 10:
            out["gnss_fix_quality"] = int(f[6]) if f[6].isdigit() else None
            out["gnss_sats"] = int(f[7]) if f[7].isdigit() else None
            out["gnss_hdop"] = _num(f[8])
            out["gnss_alt_m"] = _num(f[9])
    gst = pick("GST")
    if gst:
        f = _nmea_fields(gst["nmea"])
        if f and len(f) >= 9:
            out["gnss_err_lat_m"] = _num(f[6])
            out["gnss_err_lon_m"] = _num(f[7])
            out["gnss_err_alt_m"] = _num(f[8])
    return out


def join_rows(rows, keep_raw=False):
    for lineno, row in rows:
        mq = row.get("mqvpn")
        omr = row.get("omr")
        base = {
            "seq": row.get("seq"),
            "session": row.get("session"),
            "clock_realtime_ns": row.get("clock_realtime_ns"),
            "clock_monotonic_ns": row.get("clock_monotonic_ns"),
            "missed": row.get("missed"),
        }
        base.update(decode_gnss(row.get("gnss"), row.get("clock_monotonic_ns")))
        if not mq or not (mq.get("status") or {}).get("clients"):
            yield dict(base, path_id=None, iface=None, join="no_mqvpn_status")
            continue
        ifaces = path_id_to_iface(mq)
        devices = omr_by_device(omr)
        for client in mq["status"]["clients"]:
            for p in client.get("paths") or []:
                pid = p.get("path_id")
                info = ifaces.get(int(pid)) if pid is not None else None
                iface = info.get("iface") if info else None
                out = dict(base)
                out["path_id"] = pid
                out["iface"] = iface
                out["join"] = "ok" if iface else "path_id_not_in_list_paths"
                out["path_status"] = info.get("status") if info else None
                for k in ("srtt_ms", "min_rtt_ms", "cwnd", "in_flight", "bytes_tx",
                          "bytes_rx", "pkt_sent", "pkt_recv", "pkt_lost",
                          "state_label", "reinject_tx_bytes"):
                    out["mq_" + k] = p.get(k)
                out.update(flatten_omr(devices.get(iface)))
                if keep_raw:
                    out["mq_path_raw"] = p
                    out["omr_raw"] = devices.get(iface)
                yield out


def summarize(joined):
    by_iface = {}
    for r in joined:
        key = r.get("iface") or f"(unjoined path_id={r.get('path_id')})"
        d = by_iface.setdefault(key, {"n": 0, "srtt": [], "omr_if": set(), "join": set()})
        d["n"] += 1
        d["join"].add(r.get("join"))
        if r.get("mq_srtt_ms") is not None:
            d["srtt"].append(r["mq_srtt_ms"])
        if r.get("omr_interface"):
            d["omr_if"].add(r["omr_interface"])
    print("iface           samples  omr_interface  srtt_ms(median)  join", file=sys.stderr)
    for k, d in sorted(by_iface.items()):
        med = statistics.median(d["srtt"]) if d["srtt"] else "-"
        print(f"{k:<15} {d['n']:>7}  {','.join(sorted(d['omr_if'])) or '-':<13}  "
              f"{med!s:>15}  {','.join(sorted(x for x in d['join'] if x))}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("jsonl")
    ap.add_argument("--csv", help="write flat CSV here instead of JSONL on stdout")
    ap.add_argument("--summary", action="store_true", help="print per-iface summary to stderr")
    ap.add_argument("--raw", action="store_true", help="embed the raw per-path / OMR records")
    args = ap.parse_args()

    joined = list(join_rows(load_rows(args.jsonl), keep_raw=args.raw))
    if args.csv:
        keys = []
        for r in joined:
            for k in r:
                if k not in keys:
                    keys.append(k)
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            for r in joined:
                w.writerow({k: (json.dumps(v) if isinstance(v, (dict, list)) else v) for k, v in r.items()})
    else:
        for r in joined:
            sys.stdout.write(json.dumps(r, separators=(",", ":")) + "\n")
    if args.summary:
        summarize(joined)
    return 0


if __name__ == "__main__":
    sys.exit(main())
