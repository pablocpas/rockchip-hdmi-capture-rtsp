# Bash completion for rk-hdmi-streamer
_rk_hdmi_streamer() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "$prev" in
        -c|--config|-o|--output|--rga-library)
            COMPREPLY=( $(compgen -f -- "$cur") )
            return 0
            ;;
        -d|--device)
            COMPREPLY=( $(compgen -f -- "${cur:-/dev/v4l/by-id/}") )
            return 0
            ;;
        --video-codec)
            COMPREPLY=( $(compgen -W "h264 h265 hevc" -- "$cur") )
            return 0
            ;;
        --rc-mode)
            COMPREPLY=( $(compgen -W "vbr cbr" -- "$cur") )
            return 0
            ;;
        --stream-profile)
            COMPREPLY=( $(compgen -W "mjpeg rga yuyv-libyuv" -- "$cur") )
            return 0
            ;;
        --input-format)
            COMPREPLY=( $(compgen -W "mjpeg yuyv" -- "$cur") )
            return 0
            ;;
        --yuyv-converter)
            COMPREPLY=( $(compgen -W "rga libyuv" -- "$cur") )
            return 0
            ;;
        --video-range)
            COMPREPLY=( $(compgen -W "limited full unspecified" -- "$cur") )
            return 0
            ;;
        --audio-codec)
            COMPREPLY=( $(compgen -W "opus l16" -- "$cur") )
            return 0
            ;;
        --audio-frame-ms)
            COMPREPLY=( $(compgen -W "2.5 5 10 20 40 60" -- "$cur") )
            return 0
            ;;
        -s|--size)
            COMPREPLY=( $(compgen -W "1920x1080 1280x720 640x360" -- "$cur") )
            return 0
            ;;
        -r|--fps)
            COMPREPLY=( $(compgen -W "25 30 50 60" -- "$cur") )
            return 0
            ;;
        -b|--bitrate)
            COMPREPLY=( $(compgen -W "2M 2.5M 4M 6M 8M 12M 15M 18M" -- "$cur") )
            return 0
            ;;
    esac

    local opts="
        -c --config --check-config --print-config -q --quiet -v --verbose
        --stats-interval -V --version -h --help
        -d --device --stream-profile --input-format --yuyv-converter --rga-library
        --v4l2-dmabuf --no-v4l2-dmabuf --mppjpeg-copy-output --no-mppjpeg-copy-output
        -s --size --width --height -r --fps --video-range
        --video --no-video --video-codec --rc-mode -b --bitrate --gop
        --audio --no-audio --audio-device --audio-codec --audio-gain --audio-frame-ms
        -o --output -l --listen-rtsp --rtsp-path --rtsp-stream --rtsp-profile
        --rtp-payload --max-clients --rtsp-idle-grace-ms --rtsp-send-timeout-ms
        --rtsp-debug --no-rtsp-debug --frames
    "
    COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
}
complete -F _rk_hdmi_streamer rk-hdmi-streamer
