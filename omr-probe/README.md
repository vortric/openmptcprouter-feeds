# omr-probe

Active per-carrier measurement for the survey rig. Passive metrics only show
what the mqvpn scheduler chose to send; a coverage map needs every carrier
measured under the same conditions, so `omr-probed` runs directly on each
WAN interface (`SO_BINDTODEVICE`, `curl --interface`) and never through the
tunnel.

| probe | cadence | what it records |
|---|---|---|
| `udp_echo` | 1 Hz per interface | RTT to the server's UDP echo (`rtt_ms`) or `lost:true` after `udp_timeout_ms` |
| `http_down` | every `capacity_period_s` per interface, round-robin | `GET /down?bytes=N`, curl's raw timing/size/speed numbers |
| `http_up` | right after each `http_down` | `POST /up` with N bytes, same curl numbers |

Every result is one JSON event, e.g.

```json
{"seq":135,"session":"drive-01","type":"udp_echo","iface":"usb-au","probe_seq":44,
 "sent_realtime_ns":1789806490784483435,"sent_monotonic_ns":47147339400331,"rtt_ms":54.971}
{"seq":140,"session":"drive-01","type":"http_down","iface":"usb-docomo","bytes":5000000,
 "start_realtime_ns":...,"start_monotonic_ns":...,"end_monotonic_ns":...,"curl_exit":0,
 "curl":{"http_code":"200","time_connect":0.111,"time_starttransfer":0.226,"time_total":0.82,
         "size_download":5000000,"speed_download":6115444,...}}
```

written to `<rawlog_dir>/probe-YYYYMMDD.jsonl` (always) and to
`<survey_base>/<session>/probe.jsonl` while an omr-survey session runs.
`ubus call probe get` returns the latest values per interface (UDP counters,
a 60-sample RTT window with min/avg/max/jitter, last down/up curl results);
omr-survey samples it as the `probe` source. `ubus call probe status` is the
one-line summary.

`tunnel_interface` (default `tun0`) adds the mqvpn tunnel as a fourth probe
target. It measures what the bonded tunnel actually delivers, directly
comparable with the per-carrier numbers, and it is the only thing that puts
real traffic on mqvpn's paths: without it a survey drive leaves the tunnel
idle and its per-path metrics say nothing. Budget it as a fourth carrier;
its traffic also rides on the three WANs.

Data budget: with the defaults (5 MB down + 2 MB up per interface every
120 s, 3 carriers) the capacity probes use about 630 MB/h of mobile data;
the UDP probes are negligible. Tune `capacity_period_s` / `down_bytes` /
`up_bytes` in `/etc/config/omr-probe`. Spatial resolution at 40 km/h: UDP
every 11 m, capacity every 1.3 km per carrier.

## Server side

`server/omr-probe-server.py` (Python 3, no dependencies) provides the UDP
echo and the HTTP `/down`, `/up`, `/ping` endpoints; `server/omr-probe-server.service`
is a hardened systemd unit. On the rig's VPS it runs on UDP 4433 and TCP 443
because those are what the cloud firewall allows. Only for a rig whose
carriers are being surveyed: no auth, no rate limit.
