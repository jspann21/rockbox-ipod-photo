# A1099 MPEG CPU/COP render-overlap A/B test

PR #16 keeps MPEG decoding on the COP. In accelerated mode, exact 220x176
YUV420 display frames are copied into two staging slots and rendered to LCD by
a CPU worker while the COP resumes MPEG parsing.

This is one build and one test session.

## Files
Use the two clips in `mpeg16_corpus.zip`:
- `mpeg_220x176_15fps.mpg`
- `mpeg_220x176_24fps.mpg`

Both contain MP2 stereo audio. Listen for clicks, pauses, underruns, or pitch
problems during both passes.

Set MPEGPlayer:
- Limit FPS: Yes
- Skip Frames: Yes

## Reference pass
1. Delete `.rockbox/mpeg16.csv`.
2. Create `.rockbox/mpeg16.enabled`.
3. Create `.rockbox/mpeg16.reference`.
4. Play the 15 fps clip completely.
5. Play the 24 fps clip completely.

## Accelerated pass
1. Delete only `.rockbox/mpeg16.reference`.
2. Keep `.rockbox/mpeg16.enabled`.
3. Play the 15 fps clip completely.
4. Play the 24 fps clip completely.

Check:
- correct image and colors;
- no tearing/stale frames;
- continuous audio;
- no crash/hang;
- accelerated playback should not look less smooth.

Copy `.rockbox/mpeg16.csv` and run:
`python3 check_log.py mpeg16.csv`

If the CPU-render worker cannot keep up or hurts audio/decode, the experiment
will be rejected rather than shipped.
