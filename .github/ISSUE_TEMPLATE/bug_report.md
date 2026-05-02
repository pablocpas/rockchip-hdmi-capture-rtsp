---
name: Bug report
about: Report a capture, encoding, audio, or RTSP issue
title: ""
labels: bug
assignees: ""
---

## Problem

Describe what happens and what you expected.

## Hardware

- Board:
- SoC:
- OS image:
- Kernel:
- Capture dongle:
- USB port/hub:

## rk-hdmi-streamer command

```bash

```

## Logs

```text

```

## Video device formats

Output of:

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
```

```text

```

## Audio devices

Output of:

```bash
arecord -l
```

```text

```

## Rockchip MPP

Output of:

```bash
pkg-config --modversion rockchip_mpp
ldconfig -p | grep -E 'rockchip|mpp'
```

```text

```
