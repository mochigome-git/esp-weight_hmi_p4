# SPIFFS contents

Drop your alert sound WAV files here. They will be packed into the `storage`
partition image at flash time and accessible from C at `/spiffs/...`.

## Required files

| Filename          | Played for           |
|-------------------|----------------------|
| `alert_pass.wav`  | PASS status          |
| `alert_low.wav`   | LOW alert            |
| `alert_high.wav`  | HIGH alert           |
| `click.wav`       | UI confirmation tap  |

## Required format

- **PCM 16-bit signed little-endian**
- **16000 Hz sample rate**
- **Mono (1 channel)**
- Keep each file under ~32KB (i.e. < 1 second duration)

## Quick conversion with ffmpeg

```bash
ffmpeg -i your_download.mp3 \
       -acodec pcm_s16le \
       -ar 16000 \
       -ac 1 \
       alert_pass.wav
```

## Free sound sources

- pixabay.com/sound-effects (CC0)
- mixkit.co/free-sound-effects (CC0)
- freesound.org (mostly CC, account needed)

Suggested search terms:
- PASS  → "confirmation beep", "soft chime"
- LOW   → "warning beep", "caution tone"
- HIGH  → "alarm beep", "urgent alert"
- click → "UI tap", "soft click"

## Updating

After dropping or replacing files here, just run `idf.py flash` - the SPIFFS
image is rebuilt and reflashed automatically.
