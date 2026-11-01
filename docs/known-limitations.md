# Known limitations

SNDS is an alpha for technical testers. These are known and intended limits, not bugs, unless
the behavior differs from what is written here.

## Platform and hardware

- **Windows 11 x64 only.** Other systems are not tested and the scripts assume Windows PowerShell 5.1.
- **NVIDIA CUDA is the only accelerator.** Other GPUs use the CPU. The CPU path always works but is
  slower (see [measured times](testing.md#measured-baselines)).
- **Graphics fall back, by design.** The reactive artwork needs OpenGL 3.2. If it is missing or
  fails, SNDS switches to a CPU renderer and then to static artwork, shows a notice, and keeps
  playing. Expect fewer frames per second on the CPU renderer and on integrated GPUs. A hung graphics
  driver is moved aside and the next renderer takes over, without freezing the window. There is no reduced-motion mode
  beyond turning **Effects** off.

## Songs and files

- **WAV and MP3 only.** WAV must be 16, 24 or 32-bit PCM or 32-bit float; mono or stereo;
  8 to 192 kHz. Other containers, codecs, and surround files are refused with a reason.
- **One song and one preparation at a time.**
- **The original file is not copied.** SNDS keeps the path, size and content hash. If the file
  moves, **Stem Mix** still plays from the cache, the **Stem Mix** and **Original** buttons are
  greyed out, and **Locate Original...** restores it, but only for the same file (same size and hash). If you edit the file,
  SNDS treats it as a new song and prepares it again.
- **MP3 timing.** MP3 encoders add a short silence at the start. SNDS removes it, using the
  encoder-delay tag (the Xing/LAME header), when it lines **Original** up with the stems, and
  then measures the result. If the original does not line up exactly (for example an MP3 with no
  tag whose decoder delay differs), **Original** is turned off for that song with the reason.
  Stem Mix is unaffected.
- **Separation is a model, not a recording.** `htdemucs` leaves audible artifacts: some bleed
  between stems, softened transients, and a stem sum that differs from the original (about -28 to
  -35 dB on the owner track). **Original** is the real mix, and the difference is expected.

## Mixing

- **One global tempo.** SNDS detects one BPM. Live recordings or songs with tempo changes drift
  against the tempo-synced delay and the visuals. Set the tempo by hand, or use **×½** and **×2**.
  There is no tap tempo and no tempo map. Playback speed never changes.
- **The mix is ephemeral.** Gains, mutes, solos, pans, filters and effects are not saved. A song
  opens with the default mix every time. A tempo you set is the one exception: it is stored with
  the prepared song and used the next time it opens. There are no presets, no export, and no
  recording.
- Four fixed stems: drums, bass, other, vocals.

## Storage

- Each prepared song uses about 5 GB per hour of 44.1 kHz audio (four float32 stereo stems;
  more at higher sample rates). SNDS never deletes songs on its own; use **Song cache...**.
- A cache entry made by another SNDS version is marked incompatible and has to be prepared again.

## Legal

- JUCE and the model checkpoints have licence terms that matter before any wider sharing. See
  `THIRD_PARTY_NOTICES.md` in the repository root.
