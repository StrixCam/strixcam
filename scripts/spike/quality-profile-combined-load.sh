#!/usr/bin/env bash
# Record+stream quality-rework spike (ON-METAL ONLY, plan 2026-07-09-001 U6).
# Measures whether the NEW quality profile — record + RTMP both 1080p30 with
# tune=zerolatency DROPPED, bframes=3 b-adapt=1 rc-lookahead=20, 14 Mbps, a 3s
# time-deepened leaky pre-encode queue, at speed-preset=$PRESET — sustains >=1x
# realtime alongside live preview + the 2 synthesized 480p15 training proxies,
# WITHOUT starving Argus capture (R9).
#
# Real ISP source: nvarguscamerasrc (sensor-id=0), so total CPU includes the real
# capture path and the Argus-starvation interaction is genuine. Stop the firmware
# service first (it holds Argus). Run per-preset:  PRESET=veryfast ./this.sh
#
# Verdict: SUSTAINED iff recorded record.mp4 duration ~= wall-clock (ratio>=0.98)
# AND total CPU stays clear of the ~249% Argus-starvation threshold (no
# INVALID_SETTINGS in the argus log). Pick the SLOWEST preset that holds.
set -euo pipefail

PRESET="${PRESET:-superfast}"
DUR_S="${DUR_S:-40}"
BFRAMES="${BFRAMES:-3}"
RC_LOOKAHEAD="${RC_LOOKAHEAD:-20}"
BITRATE="${BITRATE:-14000}"
QUEUE_NS="${QUEUE_NS:-3000000000}"   # 3s deepened leaky pre-encode queue
RTMP_URL="${RTMP_URL:-rtmp://10.10.1.96/live/spike}"
OUT_DIR="${OUT_DIR:-/tmp/quality-spike}"
rm -rf "$OUT_DIR"; mkdir -p "$OUT_DIR"

QQ="queue leaky=downstream max-size-buffers=30 max-size-time=${QUEUE_NS} max-size-bytes=0"
QUAL="x264enc speed-preset=${PRESET} bframes=${BFRAMES} b-adapt=1 rc-lookahead=${RC_LOOKAHEAD} bitrate=${BITRATE} key-int-max=60"
# The IMX477 negotiates 1080p60 (sensor mode 1); the firmware feeds 30fps frames,
# so EVERY branch downrates to its target fps in software (videorate) — otherwise
# record/rtmp encode at 60fps and double-count CPU. RATE30 caps to 30fps.
RATE30="videorate ! video/x-raw,framerate=30/1"
SRC_FPS="${SRC_FPS:-60}"
NUMBUF="$((DUR_S * SRC_FPS))"   # natural EOS after ~DUR_S so mp4mux finalizes a valid moov

echo "== quality-profile combined-load spike =="
echo "preset=${PRESET} bframes=${BFRAMES} rc-lookahead=${RC_LOOKAHEAD} bitrate=${BITRATE} queue=${QUEUE_NS}ns dur=${DUR_S}s rtmp=${RTMP_URL}"

cpu_snapshot() { awk '/^cpu /{t=0;for(i=2;i<=NF;i++)t+=$i;print t, $5}' /proc/stat; }

# Full concurrent load in one gst-launch: real Argus source teed to
# (1) 1080p30 quality record, (2) 1080p30 quality RTMP + silent AAC,
# (3) low-latency preview (fakesink), (4)+(5) two 480p15 ultrafast proxies.
# num-buffers gives a natural EOS so mp4mux writes a valid moov (clean finalize).
gst-launch-1.0 -e \
  nvarguscamerasrc sensor-id=0 num-buffers="$NUMBUF" \
    ! "video/x-raw(memory:NVMM),width=1920,height=1080,format=NV12" \
    ! tee name=t \
  flvmux name=mux streamable=true ! rtmp2sink location="$RTMP_URL" sync=false \
  audiotestsrc is-live=true wave=silence num-buffers="$((DUR_S * 44100 / 1024))" ! audioconvert ! voaacenc ! aacparse ! queue ! mux.audio \
  t. ! queue ! nvvidconv ! video/x-raw,format=I420 ! $RATE30 \
       ! $QQ ! $QUAL ! h264parse ! mp4mux ! filesink location="$OUT_DIR/record.mp4" \
  t. ! queue ! nvvidconv ! video/x-raw,format=I420,width=1920,height=1080 ! $RATE30 \
       ! $QQ ! $QUAL ! h264parse ! queue ! mux.video \
  t. ! queue ! nvvidconv ! video/x-raw,format=I420 ! $RATE30 \
       ! queue leaky=downstream max-size-buffers=2 \
       ! x264enc speed-preset=ultrafast tune=zerolatency bitrate=4000 key-int-max=60 ! fakesink sync=false \
  t. ! queue ! nvvidconv ! video/x-raw,format=I420,width=854,height=480 ! videorate ! video/x-raw,framerate=15/1 \
       ! queue leaky=downstream max-size-buffers=30 \
       ! x264enc speed-preset=ultrafast tune=zerolatency bitrate=1500 key-int-max=30 ! h264parse ! mp4mux ! filesink location="$OUT_DIR/proxy0.mp4" \
  t. ! queue ! nvvidconv ! video/x-raw,format=I420,width=854,height=480 ! videorate ! video/x-raw,framerate=15/1 \
       ! queue leaky=downstream max-size-buffers=30 \
       ! x264enc speed-preset=ultrafast tune=zerolatency bitrate=1500 key-int-max=30 ! h264parse ! mp4mux ! filesink location="$OUT_DIR/proxy1.mp4" \
  >"$OUT_DIR/gst.log" 2>&1 &
GST_PID=$!

sleep 6  # warmup (Argus start + pipeline PLAYING)
read C0 I0 < <(cpu_snapshot)
sleep "$DUR_S"
read C1 I1 < <(cpu_snapshot)
# Safety cap: give clean EOS/finalize up to 30s, then force-stop if wedged.
( sleep 30; kill -9 "$GST_PID" 2>/dev/null ) &
WATCH=$!
wait "$GST_PID" 2>/dev/null || true
kill "$WATCH" 2>/dev/null || true

NCPU=$(nproc)
BUSY=$(awk -v c0="$C0" -v i0="$I0" -v c1="$C1" -v i1="$I1" -v n="$NCPU" \
  'BEGIN{dt=c1-c0; di=i1-i0; if(dt<=0){print "NA"; exit} printf "%.0f", 100.0*n*(dt-di)/dt}')

echo
echo "== results (preset=${PRESET}) =="
echo "  total CPU busy over window: ${BUSY}%  (of ${NCPU}00% max; ~249% = Argus-starvation risk)"
REC_DUR=$(gst-discoverer-1.0 "$OUT_DIR/record.mp4" 2>/dev/null | awk -F': ' '/Duration/{print $2; exit}')
echo "  record.mp4 duration: ${REC_DUR:-<NONE — did not sustain / no valid moov>}  (target ~${DUR_S}s)"
echo "  proxy0: $(gst-discoverer-1.0 "$OUT_DIR/proxy0.mp4" 2>/dev/null | awk -F': ' '/Duration/{print $2; exit}' || echo none)"
echo "  argus errors: $(grep -c -i 'invalid_settings\|error' "$OUT_DIR/gst.log" 2>/dev/null || echo 0) (see $OUT_DIR/gst.log)"
echo "  rtmp egress check: run 'gst-discoverer-1.0 ${RTMP_URL}' from a third box"
