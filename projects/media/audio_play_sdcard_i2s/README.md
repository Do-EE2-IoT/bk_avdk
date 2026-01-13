## Project：audio

## Life Cycle：2023-07-15 ~~ 2024-01-15

## Application：media audio_play_sdcard_i2s

## Description:
This project plays MP3 audio files from SD card through I2S output to an external DAC.
Unlike the DAC version, this uses direct I2S hardware interface for better audio quality.

## Hardware Requirements:
- BK7258 board with PSRAM
- SD card with MP3 files  
- External I2S DAC (e.g., PCM5102, MAX98357A, etc.)
- Speaker connected to I2S DAC

## I2S GPIO Configuration (GROUP_0):
- GPIO6: MCLK (Master Clock)
- GPIO7: BCLK (Bit Clock)
- GPIO8: LRCK (Left/Right Clock)
- GPIO9: DOUT (Data Output to DAC)

## Special Macro Configuration Description:
1、CONFIG_I2S                           // Enable I2S driver
2、CONFIG_ASDF       				    // CONFIG AUDIO Software Development Framework
3、CONFIG_ASDF_FATFS_STREAM			    // CONFIG Fatfs stream
4、CONFIG_ASDF_MP3_DECODER			    // CONFIG mp3 decoder

## Complie Command:
1、make bk7256 PROJECT=media/audio_play_sdcard_i2s
2、make bk7258 PROJECT=media/audio_play_sdcard_i2s

## CPU:
1、BK7256: CPU0
2、BK7258：CPU0 + CPU1

## Media: AUDIO
