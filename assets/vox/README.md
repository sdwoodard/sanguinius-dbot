# Vox fallback acceptance gate

Stephen auditioned and approved the three `tts-1`/`onyx` fallback clips on
2026-08-25. The checked-in PCM files are the exact normalized candidates he
approved. No provider WAV is retained.

An approved asset set consists of `entrance.pcm`, `error.pcm`,
`farewell.pcm`, and `fallbacks-v1.json`. The manifest must set
`approved` to `true` and record the exact text, provider, model, voice,
generation date, `s16le/48000/stereo` target, filename, and SHA-256 for every
clip. Vox-enabled configuration and startup fail closed until that exact set is
installed in `SANGUINIUS_TTS_FALLBACK_DIRECTORY`.
