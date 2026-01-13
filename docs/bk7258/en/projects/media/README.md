# BK7258 Audio Playback Documentation

## 📄 English Documentation

### [SD Card Audio Playback Guide](./audio_play_sdcard_mp3_music/index.md)

Comprehensive guide covering:
- ✅ Complete system architecture explanation
- ✅ Quick start guide for MP3 playback demo
- ✅ API reference (Audio Interface, MP3 Decoder, FatFS)
- ✅ Detailed code walkthrough
- ✅ Configuration and customization guide
- ✅ Extending to other audio formats (WAV, AAC, FLAC)
- ✅ Fast development guide
- ✅ Troubleshooting and optimization tips

**Read the guide:** [index.md](./audio_play_sdcard_mp3_music/index.md)

---

## Overview

This documentation helps you understand and implement audio playback from SD card on BK7258:

```
SD Card (FatFS) → Audio Decoder → Audio Interface (I2S/DAC) → Speaker
```

**Key Features:**
- Play audio files from SD card
- Support for multiple formats (MP3, WAV, AAC, FLAC)
- I2S/DAC output to speaker
- Dual-core architecture (CPU0 + CPU1)
- Easy to extend and customize

**Target Audience:** Embedded developers working with BK7258 chip who need to implement audio playback functionality.

---

## Quick Links

- **Demo Project:** `projects/media/audio_play_sdcard_mp3_music/`
- **API Headers:** 
  - `components/multimedia/include/aud_intf.h`
  - `bk_idk/include/modules/mp3dec.h`

---

## Getting Started

1. Read the [Audio Playback Guide](./audio_play_sdcard_mp3_music/index.md)
2. Build the demo project: `make bk7258 PROJECT=media/audio_play_sdcard_mp3_music`
3. Prepare your SD card with MP3 files
4. Flash and test using CLI commands

For detailed instructions, see the main documentation.
