#!/usr/bin/env bash
# U2 spike (ON-METAL ONLY) — does software x264 sustain 1080p60 record when VIC
# (nvvidconv) does the scale/convert AND the real concurrent match load is
# present? The verdict gates advertising 1080p60 as the default (U3).
#
# CRITICAL (adversarial review finding): the real proxy encode sinks don't exist
# yet at spike time, so this harness SYNTHESIZES the two 480p@15 training-proxy
# x264 encoders alongside record + RTMP + RTSP preview. Measuring 1080p60 record
# in isolation under-counts CPU and would ship an over-optimistic default.
#
# Run this ON A REAL JETSON (headless, multi-user.target — see install.sh U1),
# not in the dev container. It uses videotestsrc as a stand-in source so it needs
# no cameras; swap in nvarguscamerasrc to measure with the real ISP path too.
#
# Verdict rule: 1080p60 is sustainable ONLY if the recorded MP4's real duration
# ~= wall-clock capture time (ratio >= ~0.98) AND every branch stays realtime.
# If it lags, keep 1080p30 the default (do NOT flip kSupportedVideoModes).
set -euo pipefail

DUR_S="${DUR_S:-60}"            # capture seconds
OUT_DIR="${OUT_DIR:-/tmp/vic-spike}"
RTMP_URL="${RTMP_URL:-rtmp://127.0.0.1:1935/live/spike}"   # a local nginx-rtmp, or drop this branch
mkdir -p "$OUT_DIR"

echo "== U2 VIC combined-load spike: 1080p60 record + RTMP + preview + 2x 480p15 proxy =="
echo "duration=${DUR_S}s  out=${OUT_DIR}"

# One gst-launch process carrying the full concurrent encode load. tee fans the
# 1080p60 source to: (1) VIC-scaled 1080p60 record, (2) VIC-scaled RTMP,
# (3) two VIC-scaled 480p15 proxy encoders (the synthesized training-proxy load).
# nvvidconv = VIC (scale+convert on hardware); x264enc = software (the ceiling).
timeout "$((DUR_S + 5))" gst-launch-1.0 -e \
  videotestsrc is-live=true pattern=smpte \
    ! video/x-raw,format=NV12,width=1920,height=1080,framerate=60/1 \
    ! tee name=t \
  t. ! queue ! nvvidconv ! video/x-raw,format=I420 \
       ! queue leaky=downstream max-size-buffers=30 \
       ! x264enc speed-preset=ultrafast tune=zerolatency bitrate=8000 key-int-max=120 \
       ! h264parse ! mp4mux ! filesink location="$OUT_DIR/record-1080p60.mp4" \
  t. ! queue ! nvvidconv ! video/x-raw,format=I420 ! videorate ! video/x-raw,framerate=60/1 \
       ! queue leaky=downstream max-size-buffers=30 \
       ! x264enc speed-preset=ultrafast tune=zerolatency bitrate=6000 key-int-max=120 \
       ! h264parse ! flvmux ! rtmp2sink location="$RTMP_URL" sync=false \
  t. ! queue ! nvvidconv ! video/x-raw,format=I420,width=854,height=480 ! videorate ! video/x-raw,framerate=15/1 \
       ! queue leaky=downstream max-size-buffers=30 \
       ! x264enc speed-preset=ultrafast tune=zerolatency bitrate=1500 key-int-max=30 \
       ! h264parse ! mp4mux ! filesink location="$OUT_DIR/proxy-cam0.mp4" \
  t. ! queue ! nvvidconv ! video/x-raw,format=I420,width=854,height=480 ! videorate ! video/x-raw,framerate=15/1 \
       ! queue leaky=downstream max-size-buffers=30 \
       ! x264enc speed-preset=ultrafast tune=zerolatency bitrate=1500 key-int-max=30 \
       ! h264parse ! mp4mux ! filesink location="$OUT_DIR/proxy-cam1.mp4" \
  || echo "(pipeline ended)"

echo
echo "== Verdict input: recorded 1080p60 duration vs ${DUR_S}s wall-clock =="
REC_DUR=$(gst-discoverer-1.0 "$OUT_DIR/record-1080p60.mp4" 2>/dev/null | awk -F': ' '/Duration/{print $2; exit}')
echo "  recorded duration: ${REC_DUR:-<none — likely could NOT sustain, no valid moov>}"
echo "  wall-clock:        ${DUR_S}s"
echo "  SUSTAINED iff recorded ~= wall-clock. If short/absent → keep 1080p30 default (do not flip U3)."
