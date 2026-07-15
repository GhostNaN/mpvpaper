#!/bin/bash

# An old proof of concept to auto cycle videos in a playlist based on a timer
# This then formally became the "--slideshow" option

TIMER=15 # In minutes

output="$1"
playlist_dir="$2"

mpvpaper -o "input-ipc-server=/tmp/mpv-socket-$output no-audio loop loop-playlist" $output $playlist_dir &

while pidof mpvpaper >/dev/null; do
    sleep $((TIMER * 60))
    echo 'playlist-next' | socat - /tmp/mpv-socket-$output
done
