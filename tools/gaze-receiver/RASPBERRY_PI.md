# Gaze Stream on Raspberry Pi 5

This document covers hardware video decoding on Raspberry Pi 5 and the encoder optimizations made to support it.

## Pi 5 Hardware Decoder Capabilities

| Codec | Hardware Support | Notes |
|-------|------------------|-------|
| **HEVC/H.265** | ✅ Yes | V4L2 stateless decoder |
| **HEVC Main10 (10-bit)** | ❌ No | Hardware cannot decode 10-bit |
| **H.264/AVC** | ❌ No | **Removed in Pi 5** - CPU decode only |

### Critical Difference from Pi 4

The Raspberry Pi 5 **removed** the H.264 hardware decoder that was present in Pi 4. The design decision was made because the Pi 5's CPU cores can decode more streams than the old hardware block could.

This means:
- **HEVC is the correct choice** for Pi 5 hardware acceleration
- **H.264 will use 100% CPU** on Pi 5 (no hardware assist)
- Pi 4 users may prefer H.264 (has both H.264 and HEVC hardware decoders)

## V4L2 Stateless Decoder

Pi 5 uses the V4L2 stateless decoder API, which is different from Pi 4's v4l2m2m.

### Device Setup

Add to `/boot/config.txt`:
```
dtoverlay=rpivid-v4l2
```

This creates `/dev/video19` for the stateless decoder.

### Verify Devices
```bash
v4l2-ctl --list-devices
# Look for rpi-hevc-dec with video19 and media devices
```

### FFmpeg Hardware Acceleration

**Important**: Pi 5 uses `-hwaccel drm`, NOT `-hwaccel v4l2m2m`

```bash
# Correct for Pi 5
ffmpeg -hwaccel drm -i input.hevc -f null -

# This will show in output:
# [hevc @ ...] Hwaccel V4L2 HEVC stateless V4; devices: /dev/media2,/dev/video19
```

### Required Permissions

Ensure the user is in the `video` group:
```bash
sudo usermod -aG video $USER
# Logout and login for changes to take effect
```

## Encoder Settings for Pi 5 Compatibility

The Gaze Stream filter explicitly sets **HEVC Main profile (8-bit)** to ensure Pi 5 hardware decode compatibility.

### Settings Applied (all hardware encoders)

| Setting | Value | Reason |
|---------|-------|--------|
| `profile` | `main` | 8-bit only (Pi 5 cannot decode Main10) |
| `tier` | `main` | Standard tier for streaming |
| `max_b_frames` | `0` | No B-frames for low latency |
| `preset` | Ultra low latency | Minimal encoder delay |
| `rc` | `cbr` | Constant bitrate for predictable streaming |

### Code Location

See `src/gaze/gaze-encoder.cpp`:
- `configure_nvenc()` - NVIDIA encoder
- `configure_amf()` - AMD encoder
- `configure_qsv()` - Intel encoder
- `configure_software()` - x265 fallback

## Python Receiver on Pi 5

### Install Dependencies

```bash
# Raspberry Pi OS should have FFmpeg with V4L2 support
sudo apt update
sudo apt install ffmpeg python3-pip

# Install receiver dependencies
cd tools/gaze-receiver
pip install -e .
# or with uv:
uv sync
```

### PyAV Hardware Decode

PyAV can use FFmpeg's hardware acceleration. The receiver should automatically use hardware decode if available.

For explicit hardware decode in custom code:
```python
import av

container = av.open(input_stream)
# PyAV will use FFmpeg's hwaccel if configured in system FFmpeg
```

### Docker/Container Usage

If running in Docker, expose the required devices:
```yaml
devices:
  - /dev/video19
  - /dev/media0
  - /dev/media1
  - /dev/media2
```

## Performance Tips

### Resolution

Lower resolution = less decode work. For minimal CPU usage:
- 720p is very light on Pi 5
- 1080p is comfortable
- 4K works but uses more resources

### Bitrate

CBR ensures predictable decode load. Recommended:
- 720p: 2-5 Mbps
- 1080p: 5-15 Mbps
- 4K: 15-30 Mbps

### Frame Rate

Higher frame rates mean more frames to decode:
- 30 fps: Half the decode work of 60 fps
- 60 fps: Recommended maximum for smooth operation

## Troubleshooting

### "No hardware decoder found"

1. Check `/dev/video19` exists
2. Verify user is in `video` group
3. Check `dtoverlay=rpivid-v4l2` in config.txt
4. Reboot after config changes

### High CPU usage during decode

1. Verify HEVC codec (not H.264)
2. Check stream is Main profile (not Main10)
3. Use `htop` to verify hardware decode is active
4. Check FFmpeg output for "Hwaccel V4L2 HEVC stateless"

### Decode errors / corruption

1. Verify Main profile (8-bit) - Main10 will fail
2. Check network for packet loss (FEC should help)
3. Ensure stream has proper keyframes

## References

- [Pi Forums - HEVC decode](https://forums.raspberrypi.com/viewtopic.php?t=361671)
- [Pi Forums - V4L2 stateless](https://forums.raspberrypi.com/viewtopic.php?t=381601)
- [Frigate Pi 5 HEVC Discussion](https://github.com/blakeblackshear/frigate/discussions/18431)
- [OpenCV Pi 5 V4L2 PR](https://github.com/opencv/opencv/pull/27453)
- [Phoronix - Pi HEVC Driver](https://www.phoronix.com/news/Raspberry-Pi-HEVC-H265-Decode)
