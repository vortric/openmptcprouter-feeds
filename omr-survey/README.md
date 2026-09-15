# omr-survey

Minimum telemetry pipeline for an OpenMPTCProuter-based mobile connectivity
survey. The router only *records*; all interpretation happens off-router.

## What it records

`omr-surveyd` samples once per second (scheduled on `CLOCK_MONOTONIC`, so the
cadence does not drift) and appends one line per sample to
`/tmp/omr-survey/<session>/survey.jsonl`:

```json
{"seq":12,"session":"drive-01",
 "clock_realtime_ns":1789500000123456789,"clock_monotonic_ns":81234567890,
 "clock_monotonic_end_ns":81290123456,"missed":0,
 "omr":   <raw output of `ubus -S call metrics get_all`>,
 "mqvpn": <raw output of `ubus -S call mqvpn metrics`>,
 "rc":{"omr":0,"mqvpn":0}}
```

* `omr` is omr-tracker's per-WAN record set (`interfaces[]`, each with
  `interface`, `device`, latency/loss, modem signal, throughput, ...).
* `mqvpn` bundles mqvpn's control API: `status` (`get_status`: per-path
  transport metrics keyed by xquic `path_id`) and `paths` (`list_paths`:
  `paths[]` interface names plus `path_info[]` = iface + handle + `path_id`).
* Payloads are embedded byte-for-byte as printed by `ubus -S`. A source that
  fails or prints something that is not one JSON line is stored as `null`,
  its exit status kept in `rc`, and the raw bytes appended to `errors.log`.
* `clock_monotonic_end_ns` is taken after the last ubus call, so the
  sampling latency of each row is known. `missed` counts 1 s slots skipped
  because the previous sample overran.

`meta.json` in the session directory is written at start and rewritten at
stop (`stop_reason`: `signal`, `max_samples` or `duration`; sample count;
both clocks).

## Control

```sh
ubus call omr-survey start '{"session":"drive-01"}'    # session id optional
ubus call omr-survey status
ubus call omr-survey stop
ubus call omr-survey list
```

`start` accepts `interval_ms`, `max_samples`, `duration_s`, `rotate_bytes`
(0 = unbounded / no rotation). One session runs at a time. `stop` sends
SIGTERM through procd; the daemon finishes the sample in flight, rewrites
`meta.json`, fsyncs and closes the file. `/etc/init.d/omr-survey reload`
(SIGHUP) rotates `survey.jsonl` to `survey.NNN.jsonl` without ending the
session.

Defaults live in `/etc/config/omr-survey`; `autostart '1'` starts a session
at boot without a request.

## Post-processing (off-router)

`tools/survey_join.py` (Python 3, not installed on the router) joins, per
sample, each `status.clients[].paths[]` entry to its WAN interface via
`paths.path_info[]` (`path_id` -> `iface`, live entries only) and then to
the omr-tracker record whose `device` is that interface:

```sh
scp root@router:/tmp/omr-survey/drive-01/survey.jsonl .
tools/survey_join.py survey.jsonl --summary > joined.jsonl
tools/survey_join.py survey.jsonl --csv joined.csv
```

`path_id` is volatile (it changes when a path is recreated or the tunnel
reconnects), which is why the join is done per sample and never across rows.

## Requirements

* mqvpn with `list_paths` `path_info` support (control API ≥ v0.16.3) and
  the control API enabled (`mqvpn.control.control_port`).
* `omr-metrics` (provides `ubus call metrics get_all`).
