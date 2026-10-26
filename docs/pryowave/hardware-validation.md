# Deferred UHD770 hardware acceptance

All checks below are **UNCHECKED / deferred**; this is a follow-up contract,
not evidence of a Windows run, full Sunshine link, hardware acceptance or completion.
See [implementation evidence](implementation-status.md) and [design](README.md).

## Freeze evidence and acceptance before measuring

- [ ] Record exact host/native/client commits, dirty state, build options/artifact hashes,
  Windows build, D3D/Vulkan driver versions and paired-client build/capability evidence.
- [ ] Record UHD770 PCI/device identity and matching capture/D3D11/Vulkan LUIDs;
  enumerate enabled native FP16, storage/image/queue features and actual R8 plane,
  required R16 wavelet-format and external-fence import results, including failures.
- [ ] Agree numerical quality, bitrate, performance/no-regression and bounded lifecycle
  thresholds BEFORE future measurements; do not choose limits after seeing results.
- [ ] Freeze matched current-PyroWave and same-workload QSV baselines: identical scene,
  1920x1080 at 120 Hz, signal/range/siting, bitrate/quality settings, network and client,
  RAM clocks/configuration, power plan, clocks and thermal conditions; retain configs.

## Trace and quality evidence

- [ ] Keep frame-correlated p50/p95/p99 capture, conversion, encode, bitstream readback,
  network and end-to-end timings; distinguish GPU timestamps from CPU time/waits.
- [ ] Record cadence, drops, graphics/media engines, power, bandwidth when available,
  host/GPU memory, resource and handle counts; mark unavailable counters, not zeros.
- [ ] GPU/API traces must prove borrowed-source direct conversion into R8 planes:
  no NV12 intermediate/split, capture blit, conversion scratch roundtrip, CPU frame
  readback or fallback. Encoded-bitstream readback remains; producer-internal copies
  are unverified. This is NOT end-to-end no-copy or a speed claim.
- [ ] Check limited/full reference endpoints, chroma siting, gradients, text, motion
  and video, plus actual paired-client decode; use agreed per-plane tolerances and
  quality/bitrate criteria, finite/range/overflow checks and retained reference images.
- [ ] Carry the known native 34x30 Linux ADL GT2 failure: Y/Cb/Cr errors 4/3/3
  versus unchanged limits 1/2/2 (exit 5). Further investigation was user-waived;
  this is NOT a pass or a new request to investigate now. See implementation evidence.

## Ownership, client and retained contracts

- [ ] Stress busy borrow and backpressure without fallback or overwriting in-flight
  planes; prove source GPU-read completion and release-fence-gated plane reuse.
- [ ] Verify second-session rejection leaves the first intact; failed-startup admission
  release, stop, reconnect, generation/resize rejection and device-loss teardown must
  meet agreed bounds with no late/stale frames, deadlocks, leaks or growing handles.
- [ ] Require explicit cursor-hidden mode unless negotiated paired-client cursor
  support is verified; no host cursor-compositing fallback or assumed capability.
- [ ] Verify static idle suppression, refresh, changed-frame/input and stop response
  against agreed deadlines with actual client capability evidence; invent no protocol
  fields or stale cached-packet replay to satisfy this gate.
- [ ] Preserve and exercise audio, microphone, input, pairing/authentication,
  encryption, packet bounds, FEC and error isolation; video specialization is no waiver.
- [ ] Run at least 30 minutes of paired UHD770 1080p120 streaming/decoding with
  matched baselines; retain thermal/clock histories, stage tails, drops, resources
  and GPU/API traces against the pre-agreed thresholds.
- [ ] Gate optional UMA/readback or sparse-RDO tuning on NEW correctness checks and
  matched measurements; assume no wins from logical-byte estimates or host visibility.
