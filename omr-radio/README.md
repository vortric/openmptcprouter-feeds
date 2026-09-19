# omr-radio (router side)

Pulls the radio telemetry streamed by the `omr-radio` Android app
(vortric/omr-radio-android) on each USB-tethered phone. `omr-radiod` connects
through every WAN interface (`SO_BINDTODEVICE`) to that interface's default
gateway, which is the phone itself, on TCP 8630, so the carrier of each line
is fixed by the interface. The address is taken from netifd
(`inactive.route` nexthop, then `route`, then `data.dhcpserver`, since OMR
keeps WAN default routes in per-interface tables) and re-resolved every
10 s while disconnected.

Each received line is stored verbatim:

```json
{"seq":812,"session":"drive-01","iface":"usb-au","peer":"172.25.74.2:8630",
 "recv_realtime_ns":...,"recv_monotonic_ns":...,"line":{"v":1,"trigger":"periodic",
 "t":{...},"device":{...},"service":{...},"display":...,"signal":{...},
 "cells":[{"registered":true,"type":"LTE","identity":{"pci":265,"tac":39445,
 "earfcn":5925,"bands":[18],...},"signal":{"rsrp":-107,"rsrq":-15,...}},...],
 "data":{...},"power":{"battery_temp_dC":320,"thermal_status":0,...}}}
```

into `<rawlog_dir>/radio-YYYYMMDD.jsonl` and `<survey_base>/<session>/radio.jsonl`;
connect / disconnect are logged as `{"event":...}` lines. `ubus call radio get`
returns the latest line per interface (omr-survey samples it as the `radio`
source); `ubus call radio status` is the summary.

Phone setup (all three SOG09 phones on the rig): `tools/install-phone.sh`
in the app repo installs the APK and grants READ_PHONE_STATE, fine/coarse/
background location and notifications over adb, exempts the app from
battery optimization and starts it; the service restarts on boot. Measured
fields: LTE/NR registered and neighbour cells (PCI, TAC, cell id, ARFCN,
bands, bandwidth, RSRP/RSRQ/RSSNR or SS-RSRP/RSRQ/SINR), data network type,
NetworkRegistrationInfo, TelephonyDisplayInfo when it changes, mobile byte
counters, battery temperature and thermal status. CQI and timing advance
come back as Android's "unavailable" sentinel (2147483647) on SOG09.
