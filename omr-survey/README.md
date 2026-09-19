# omr-survey

Minimum telemetry pipeline for an OpenMPTCProuter-based mobile connectivity
survey. The router only *records*; all interpretation happens off-router.

## What it records

`omr-surveyd` samples once per second (scheduled on `CLOCK_MONOTONIC`, so the
cadence does not drift) and appends one line per sample to
`/srv/survey/<session>/survey.jsonl` (envelope format `omr-survey/2`):

```json
{"seq":12,"session":"drive-01",
 "time":{"realtime_ns":1789500000123456789,"monotonic_ns":81234567890,
         "end_monotonic_ns":81290123456},
 "missed":0,
 "sources":{
   "omr":    {"ok":true,"collected_at_monotonic_ns":81234570000,"collect_ms":7.1,
              "data": <raw output of `ubus -S call metrics get_all`>},
   "mqvpn":  {"ok":true,"collected_at_monotonic_ns":81241700000,"collect_ms":5.0,
              "data": <raw output of `ubus -S call mqvpn metrics`>},
   "network":{"ok":true, ..., "data": <raw `ubus -S call network.interface dump`>},
   "gnss":   {"ok":false,"collected_at_monotonic_ns":81250100000,"collect_ms":2.0,
              "rc":1,"error":"ubus exit 1"}}}
```

Router responsibility stops at: receive, timestamp, wrap raw, append. The
envelope carries only what the collector itself knows (sequence, session,
sample start/end, per-source query time, success or failure). Everything
under `sources.*.data` is embedded byte-for-byte as printed by `ubus -S`,
so "the value was 0" and "it was not collected" stay distinguishable and
upstream schema changes never touch the collector.

* `omr` is omr-tracker's per-WAN record set (`interfaces[]`, each with
  `interface`, `device`, latency/loss, modem signal, throughput, ...).
* `mqvpn` bundles mqvpn's control API: `status` (`get_status`: per-path
  transport metrics keyed by xquic `path_id`) and `paths` (`list_paths`:
  `paths[]` interface names plus `path_info[]` = iface + handle + `path_id`).
* `network` is netifd's interface dump (up/down, addresses, l3 device).
* `gnss` (with the `omr-gnss` package) is the latest-sentence snapshot; the
  full-rate stream lives in `gnss.jsonl` (below).
* `probe` (with the `omr-probe` package) is the latest active-measurement
  state per carrier (UDP RTT window, last HTTP capacity results); every
  individual probe result is an event in `probe.jsonl` (below).
* Sources are configurable: `omr-survey.settings.sources` is a
  space-separated `key=ubus_object.method` list. A failing source keeps its
  row with `ok:false`; its raw bytes go to `errors.log`.
* `end_monotonic_ns` is taken after the last ubus call; `missed` counts 1 s
  slots skipped because the previous sample overran.

`gnss.jsonl` in the same directory is written by omr-gnssd at the receiver's
own rate (5 Hz RMC etc.), one JSON line per NMEA sentence:

```json
{"seq":3819,"session":"drive-01","recv_realtime_ns":1789500000123456789,
 "recv_monotonic_ns":81234756123,"nmea":"$GNRMC,...*3E"}
```

`probe.jsonl`, likewise, holds one event per active probe result from
omr-probed (`udp_echo` at 1 Hz per carrier, `http_down`/`http_up` per
capacity cycle), so the per-carrier RTT/loss/capacity series are independent
of what the tunnel scheduler chose to send.

Nothing is decoded on the router; parse, normalize, join and resample
happen off-router (`tools/survey_join.py`). `meta.json` is written at start
and rewritten at stop (`stop_reason`: `signal`, `max_samples` or `duration`;
sample count; both clocks).

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

Each joined row also carries the decoded GNSS fix when present:
`gnss_lat`, `gnss_lon`, `gnss_speed_kmh`, `gnss_course_deg`, `gnss_utc`,
`gnss_fix_quality`, `gnss_sats`, `gnss_hdop`, `gnss_alt_m`,
`gnss_err_lat_m` / `gnss_err_lon_m` / `gnss_err_alt_m` (from GST) and
`gnss_fix_age_ms` (age of the newest RMC at sampling time).

## Requirements

* mqvpn with `list_paths` `path_info` support (control API ≥ v0.16.3) and
  the control API enabled (`mqvpn.control.control_port`).
* `omr-metrics` (provides `ubus call metrics get_all`).

## Stable WAN names for USB-tethered phones (rig example)

RNDIS interfaces enumerate as `usb0`, `usb1`, ... in plug-in order and their
MAC changes on every tethering session, so `wan1`..`wan3` would silently swap
carriers after a reboot. `examples/hotplug-net-05-usb-tether-rename` (install
as `/etc/hotplug.d/net/05-usb-tether-rename`, adjust the hub port paths)
renames each interface by the USB hub port it sits on, e.g. `usb-docomo`,
`usb-au`, `usb-softbank`, and `network.wanN.device` points at those names.
The name then appears as `iface` in `list_paths` and as `device` in the
omr-tracker records, so joined rows carry the carrier directly.

Two rig lessons: carriers that drop ICMP need `omr-tracker.wanN.type=dns`
(the default ping probe keeps the WAN "down" and mqvpn never adds the path),
and never re-enumerate the phones from software (unbind/bind, `authorized`
toggle): Android stops answering DHCP until the cable is physically
re-plugged or tethering is toggled.
