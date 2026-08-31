# Vox fallback asset contract

The three checked-in PCM clips are the exact owner-approved normalized
tts-1/onyx fallbacks. No provider WAV or received voice audio is retained.

The required set is:

- entrance.pcm
- error.pcm
- farewell.pcm
- fallbacks-v1.json

The manifest must remain approved and record the exact public text, provider,
model, voice, generation date, s16le/48000/stereo target, filename, and SHA-256
for every clip. Vox-enabled configuration fails closed unless the exact set
validates in SANGUINIUS_TTS_FALLBACK_DIRECTORY.

Replacing a clip requires a new audition, normalized deterministic output,
updated checksums/provenance, media validation, and the full Vox fallback,
cache, provider-failure, queue, and reconnect regression suite.
