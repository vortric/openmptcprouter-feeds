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

`RESULT: OK` になれば採集可能。行頭 `X` は要対処、`!` は注意:

| 表示 | 対処 |
|---|---|
| `NO-ADDR ... toggle USB tethering` | その端末で USB テザリングを一度オフ→オン (再起動後に自動復帰しないことがある) |
| `tracker not up yet` | 1〜2 分待つ |
| `radio=false` が 3 回線同時 | `/etc/init.d/omr-radio restart` (60 秒待てば自動でも復帰) |
| `radio=false` が 1 回線だけ | その端末で「OMR Radio」アプリを開く (サービスが再起動する) |
| `no live mqvpn path` | 1 分待つ。直らなければ `/etc/init.d/mqvpn restart` |
| `GNSS ... not connected` | OPPO 側の送信を再開 (宛先 192.168.100.1:8620、TCP) |
| `disk ... MB free` が X | /srv/survey の古いセッションを削除 |

## 3. 採集の開始と停止

    ubus call omr-survey start '{"session":"drive-20260921-01"}'   # 開始 (名前は英数字 . _ -)
    ubus call omr-survey status                                     # 進行状況 (samples が毎秒増える)
    ubus call omr-survey stop                                       # 停止 (ファイルを閉じて meta.json 確定)
    ubus call omr-survey list                                       # セッション一覧

- 起動時の自動採集はしない設定 (autostart=0)。開始は必ず上記コマンド。
- 同時に走るセッションは 1 つ。名前は再利用しない (同名で start は拒否される)。
- 停止せずに電源を切っても、書き込み済みの行は残る (meta.json の running が true のまま残るだけ)。
- 長時間なら分割: `'{"session":"...","rotate_bytes":100000000}'` で survey.jsonl を 100 MB ごとに分割。

## 4. 走行中に見るもの

    ssh -t root@192.168.100.1 omr-survey-check -w

`session` 行の samples / gnss / probe / radio が増え続けていれば正常。`last row: N source(s) failed` が続く場合はそのソースを確認。

## 5. データの回収 (走行後)

    scp -O root@192.168.100.1:/srv/survey/drive-20260921-01/*.jsonl ./drive-20260921-01/
    python3 openmptcprouter-feeds/omr-survey/tools/survey_join.py drive-20260921-01/survey.jsonl --summary > joined.jsonl

ルータの scp は sftp-server が無いので `-O` が必要。

## 6. 電源を切るとき

    ubus call omr-survey stop     # 採集中なら先に停止
    poweroff

ハブとスマホはそのままで可。次回は 1 の順番で投入。

## 7. やってはいけないこと

- 走行中のルータ電源断・再起動 (スマホのテザリングが戻らないことがある)。
- スマホの USB 抜き差し、ハブポートの入れ替え。
- ルータ側でソフト的に USB を再列挙する操作 (unbind/bind、authorized)。
- 採集中の `/etc/init.d/network reload` や WAN 設定変更。

## 8. データ量の目安

- survey.jsonl 約 100 MB/時、gnss.jsonl 約 8 MB/時、probe/radio 各 約 10 MB/時。空き 110 GB。
- 能動計測 (omr-probe) は 3 回線合計で約 630 MB/時のモバイルデータを消費する。走らない日は `/etc/init.d/omr-probe stop`。

## 9. サービス一覧

| サービス | 役割 | 状態確認 |
|---|---|---|
| mqvpn | トンネル (3 path) | `ubus call mqvpn metrics` |
| omr-tracker | WAN 疎通判定 (dns) | `uci get openmptcprouter.wan1.state` |
| omr-gnss | NMEA 受信 (TCP 8620) | `ubus call gnss status` |
| omr-probe | 能動計測 (UDP エコー / HTTP 容量) | `ubus call probe status` |
| omr-radio | スマホ無線テレメトリ受信 (TCP 8630) | `ubus call radio status` |
| omr-survey | 1 Hz 封筒の記録 | `ubus call omr-survey status` |
