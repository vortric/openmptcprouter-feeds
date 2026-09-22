# OMR 調査リグ 運用手順 (USAGE)

ルータ: N150 / OpenMPTCProuter、LAN 192.168.100.1 (ssh root)。
データ: /srv/survey/<session>/ (survey.jsonl, gnss.jsonl, probe.jsonl, radio.jsonl, meta.json)

## 1. 電源投入の順番

1. USB ハブ (セルフパワー) と ポケット WiFi (eth1、DHCP オフ) に電源。
2. スマホ 3 台をハブの決まったポートに接続:
   - ポート 1 = docomo (usb-docomo / wan1)
   - ポート 2 = au     (usb-au / wan2)
   - ポート 3 = softbank (usb-softbank / wan3)
   画面ロック「なし」、開発者向けオプション「デフォルトの USB 設定 = USB テザリング」のまま。
3. N150 に電源。起動 30 秒 + 回線復帰 約 2 分。
4. GNSS 送信機 (OPPO) をポケット WiFi に接続し、NMEA を TCP 192.168.100.1:8620 へ送信開始。
5. LAN 側の PC (Mac など) をポケット WiFi または eth0 に接続。

## 2. 出発前チェック

    ssh root@192.168.100.1 omr-survey-check        # 1 回
    ssh -t root@192.168.100.1 omr-survey-check -w  # 2 秒ごと更新 (Ctrl-C で終了)

`RESULT: OK` になれば採集可能 (GNSS 送信を始めるまで GNSS だけ赤のことがある)。
行頭 `X` は要対処、`!` は注意:

| 表示 | 対処 |
|---|---|
| `NO-ADDR ... toggle USB tethering` | その端末で USB テザリングを一度オフ→オン (再起動後に自動復帰しないことがある) |
| アドレスは付いているのに `loss` が 100% に近い | その端末で USB テザリングをオフ→オン。Android の RNDIS が応答フレームの宛先 MAC を全ゼロで送る状態に陥ることがあり (2026-09-22 に au で発生)、ICMP だけ通って TCP/UDP が全滅する |
| `tracker not up yet` | 1〜2 分待つ |
| `radio=false` が 3 回線同時 | `/etc/init.d/omr-radio restart` (60 秒待てば自動でも復帰) |
| `radio=false` が 1 回線だけ | その端末で「OMR Radio」アプリを開く (サービスが再起動する) |
| `no live mqvpn path` | 1 分待つ。直らなければ `/etc/init.d/mqvpn restart` |
| `GNSS ... not connected` | OPPO 側の送信を再開 (宛先 192.168.100.1:8620、TCP) |
| `per-path metrics ... absent from get_status` | `/etc/init.d/mqvpn restart` (§4 参照)。放置すると mqvpn の srtt/バイト数がセッション中ずっと欠測になる |
| `tunnel probe ... no probe yet` | トンネルが上がってから 2 分待つ。残るなら `/etc/init.d/omr-probe restart` |
| `syslog capture ... empty` | `/etc/init.d/omr-survey` が古い。apk を入れ直す |
| `disk ... MB free` が X | /srv/survey の古いセッションを削除 |

## 3. 採集開始の直前にやること (重要)

**WAN が 3 本とも `up` になってから mqvpn を再起動し、それから採集を開始する。**

    ssh root@192.168.100.1 /etc/init.d/mqvpn restart
    # 1 分ほど待ってから
    ssh root@192.168.100.1 omr-survey-check

mqvpn は起動時に `up` の WAN しか `Path =` に書かないので、WAN より先に起動すると、どの
キャリアにも紐づかない path が 1 本できる。0920 の走行データには 4 時間ずっと残っていた。
`omr-survey-check` の `unbound mqvpn path` 警告がこれを検出する。

再起動後、次を確認する。

- `per-path metrics  all N live path(s) present in get_status` が緑
- `unbound mqvpn path` の警告が出ていない
- `tunnel probe (tun0)` に RTT が出ている (トンネルに負荷がかかっている印)

## 4. per-path 指標の欠測について (2026-09-22 に解決済み)

0920 の走行では、接続開始から約 6 分後を境に per-path 指標 (srtt / バイト数 / 損失) が
3.5 時間にわたり全欠測した。**真因は特定し修正済み**なので、通常は再発しない。

真因は `mqvpn_client_get_info()` が xquic の統計配列を**作成順のまま先頭 8 件
(MQVPN_MAX_PATHS) で打ち切っていた**こと。走行中は WAN のフラップごとに path が作り直され、
8 本を超えた時点で以降の path が一切報告されなくなっていた。path 自体は正常に通信していた
(検証が通らないわけではなかった)。修正は live な path を先に並べるもので、実機で
path_id 14 まで再生成しても 3 本すべてが報告されることを確認済み (mqvpn r4、2026-09-22 版)。

万一 `omr-survey-check` で `per-path metrics` が赤になったら、停車時に次を行う。

    ubus call omr-survey stop
    /etc/init.d/mqvpn restart
    # 1 分待って omr-survey-check が緑になってから
    ubus call omr-survey start '{"session":"drive-YYYYMMDD-02"}'

セッションの途中で mqvpn を再起動しないこと。1 つのセッションが 2 つの QUIC 接続にまたがると、
後処理で接続の切れ目が分からなくなる。なお per-path 指標が落ちても、**キャリア単体の計測
(probe)、電波 (radio)、位置 (GNSS) は影響を受けない**。

## 5. 採集の開始と停止

    ubus call omr-survey start '{"session":"drive-20260921-01"}'   # 開始 (名前は英数字 . _ -)
    ubus call omr-survey status                                     # 進行状況 (samples が毎秒増える)
    ubus call omr-survey stop                                       # 停止 (ファイルを閉じて meta.json 確定)
    ubus call omr-survey list                                       # セッション一覧

- 起動時の自動採集はしない設定 (autostart=0)。開始は必ず上記コマンド。
- 同時に走るセッションは 1 つ。名前は再利用しない (同名で start は拒否される)。
- 停止せずに電源を切っても、書き込み済みの行は残る (meta.json の running が true のまま残るだけ)。
- 長時間なら分割: `'{"session":"...","rotate_bytes":100000000}'` で survey.jsonl を 100 MB ごとに分割。

## 6. 走行中に見るもの

    ssh -t root@192.168.100.1 omr-survey-check -w

`session` 行の samples / gnss / probe / radio が増え続けていれば正常。`last row: N source(s) failed` が続く場合はそのソースを確認。

## 7. セッションに残るもの

| ファイル | 中身 |
|---|---|
| `survey.jsonl` | 1 秒ごとの封筒 (omr / mqvpn / network / gnss / probe / radio の生データ) |
| `gnss.jsonl` | NMEA を受信レートそのままで 1 文 1 行 |
| `probe.jsonl` | 能動計測のイベント (UDP エコー、容量)。tun0 = トンネル経由の計測も含む |
| `radio.jsonl` | スマホの電波テレメトリ |
| `syslog-start.log` | 採集開始時点のログリング全体 |
| `syslog.log` | 採集中のログ (mqvpn などの挙動を後から追うため) |
| `meta.json` | 開始・停止時刻、サンプル数、停止理由 |

## 8. データの回収 (走行後)

    scp -O root@192.168.100.1:/srv/survey/drive-20260921-01/*.jsonl ./drive-20260921-01/
    python3 openmptcprouter-feeds/omr-survey/tools/survey_join.py drive-20260921-01/survey.jsonl --summary > joined.jsonl

ルータの scp は sftp-server が無いので `-O` が必要。

## 9. 電源を切るとき

    ubus call omr-survey stop     # 採集中なら先に停止
    poweroff

ハブとスマホはそのままで可。次回は 1 の順番で投入。

## 10. やってはいけないこと

- 走行中のルータ電源断・再起動 (スマホのテザリングが戻らないことがある)。
- スマホの USB 抜き差し、ハブポートの入れ替え。
- ルータ側でソフト的に USB を再列挙する操作 (unbind/bind、authorized)。
- 採集中の `/etc/init.d/network reload` や WAN 設定変更。
- **セッションの途中で mqvpn を再起動する**こと。直す必要があるときは §4 のとおり、
  いったんセッションを止めてから再起動し、新しいセッションを始める。

## 11. データ量の目安

- survey.jsonl 約 100 MB/時、gnss.jsonl 約 8 MB/時、probe/radio 各 約 10 MB/時。空き 110 GB。
- 能動計測 (omr-probe) は 3 回線 + トンネルで約 840 MB/時のモバイルデータを消費する (トンネル分は 3 回線に分散して乗る)。走らない日は `/etc/init.d/omr-probe stop`。
- トンネル経由の計測 (tun0) は mqvpn のパスに実トラフィックを乗せる唯一の手段。これが無いと mqvpn の per-path 指標は無負荷のままで意味を持たない。

## 12. サービス一覧

| サービス | 役割 | 状態確認 |
|---|---|---|
| mqvpn | トンネル (3 path) | `ubus call mqvpn metrics` |
| omr-tracker | WAN 疎通判定 (dns) | `uci get openmptcprouter.wan1.state` |
| omr-gnss | NMEA 受信 (TCP 8620) | `ubus call gnss status` |
| omr-probe | 能動計測 (UDP エコー / HTTP 容量) | `ubus call probe status` |
| omr-radio | スマホ無線テレメトリ受信 (TCP 8630) | `ubus call radio status` |
| omr-survey | 1 Hz 封筒の記録 | `ubus call omr-survey status` |
