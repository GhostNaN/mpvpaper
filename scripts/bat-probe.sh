#!/bin/bash

# A script to automatically pause mpvpaper if a battery is draining

output="$1"
media="$2"

mpvpaper -o "input-ipc-server=/tmp/mpv-socket-$output" $output $media &

while pidof mpvpaper >/dev/null; do
	bat_state=$(upower -i /org/freedesktop/UPower/devices/battery_BAT0 | grep state | awk '{print $2}')

	if [ "$bat_state" = "fully-charged" ] || [ "$bat_state" = "charging" ]; then
		echo 'set pause no' | socat - /tmp/mpv-socket-$output
	elif [ "$bat_state" = "discharging" ]; then
		echo 'set pause yes' | socat - /tmp/mpv-socket-$output
	fi

	sleep 5
done
