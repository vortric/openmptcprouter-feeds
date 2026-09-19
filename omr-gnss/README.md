# omr-gnss

NMEA-over-TCP receiver for the survey rig. A GNSS receiver or phone app
pushes NMEA 0183 (tested: NMEA 4.1, GN/GP/GA/GB talkers, RMC/VTG/GGA at
5 Hz plus GSA/GSV/GST) to the router on TCP port 8620.

* `omr-gnssd` keeps the latest sentence per key (`GNRMC`, `GNGGA`, `GPGSV-2`,
  `GNGSA-3`, ...) with CLOCK_REALTIME / CLOCK_MONOTONIC receive stamps and
  writes `/tmp/gnss/state.json` (at most every 200 ms). One sender at a
  time; a new connection replaces the old one. Checksums are verified,
  nothing else is parsed on the router.
* `ubus call gnss get` returns that state verbatim; `ubus call gnss status`
  a summary (connected, peer, counts, fix age).
* With `rawlog_dir` set (default `/srv/survey/gnss`), every sentence is also
  appended to `nmea-YYYYMMDD.log` as `<realtime_ns> <monotonic_ns> <sentence>`
  so the full receiver rate is kept next to the 1 Hz survey samples.

omr-survey samples it as the `gnss` source (`gnss=gnss.get` in
`omr-survey.settings.sources`); `tools/survey_join.py` decodes lat/lon,
speed, fix quality, satellites, HDOP, altitude and GST errors per row.
