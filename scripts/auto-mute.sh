#!/bin/bash

# A script that allows for playing audio.
# Then muting automatically if there are other audio streams running

output="$1"
media="$2"

mpvpaper -o "input-ipc-server=/tmp/mpv-socket-$output loop" $output $media &

while pidof mpvpaper >/dev/null; do

    other_audio=$(pw-dump |
        jq -r '
            .[]
            | select(.type=="PipeWire:Interface:Node")
            | select(.info.props."media.class"=="Stream/Output/Audio")
            | select(.info.state=="running")
            | select(.info.props."application.name" != "mpv")
        '
    )

    mute=$(
        echo '{ "command": ["get_property", "mute"] }' |
        socat - /tmp/mpv-socket-$output |
        jq -r '.data'
    )

    if [[ -n "$other_audio" ]]; then
        if [[ "$mute" == "false" ]]; then
            echo 'set mute yes' | socat - "/tmp/mpv-socket-$output"
        fi
    else
        if [[ "$mute" == "true" ]]; then
            echo 'set mute no' | socat - "/tmp/mpv-socket-$output"
        fi
    fi

    sleep 1
done
