# PyroWave server implementation checklist

Scope: server-side requirements of `pyrowave_codec_roadmap.md`. The baseline
already implements the codec integration; this audit adds exception isolation
for CPU frame encoding and makes GPU diagnostic failures non-fatal too.
Code presence is not proof of Windows/GPU runtime behavior.

## Requirement-to-evidence map

| Requirement | Implementation | Existing verification |
| --- | --- | --- |
| Opt-in Windows server; preserve traditional codec defaults | `src/pyrowave/runtime.cpp`, PyroWave branches in `src/rtsp.cpp` and `src/video.cpp` | `PyrowaveRuntimeTest`; legacy-client integration still needs runtime validation |
| Static core/C API, pinned dependency, API 101.0.0 | `cmake/dependencies/Pyrowave_Sunshine.cmake`, gitlink `third-party/pyrowave`, `src/pyrowave/config.h` | `PyrowaveApiTest`, exact-version capability tests; Windows link verification required |
| Default context 0, synchronous frame delivery | Native `pyrowave_encoder_t` context initialization; CPU/GPU adapters use synchronous encode and packetize; GPU waits on returned fence | Static inspection; real encoder cadence unverified |
| SDR 8-bit BT.709, PQ/HLG 10-bit BT.2020, 4:2:0, limited/full range | `color_metadata.h`, `video_colorspace.cpp`, Windows display conversion shaders | `PyrowaveColorMetadataTest`; GPU pixel output unverified |
| Do not relabel SDR as HDR | Resolved colorspace rejection in `video.cpp` and `color_metadata_for` | Invalid colorspace/depth tests; display/enhancement integration unverified |
| Fixed negotiated-frame-rate budget | `rate_control.h`, both Windows encoder adapters | `PyrowaveRateControlTest`, `PyrowaveUsesHostCapWithoutLegacyRtpFecConversion` |
| Low-bpp luminance detail priority | Pinned native `pyrowave_encoder.cpp`: `get_quant_rdo_distortion_scale`, `update_low_rate_detail_profile` | Static inspection only; comparative image quality unverified |
| Independent PYRF versions 2/2/3, fixed header and packet kinds | Pinned common-c `PyrowaveProtocol.h/.c`; RTSP capability negotiation | Packetizer round-trip tests and protocol validation; ANNOUNCE integration unverified |
| Negotiated inner/outer packet limits and one packet per RTP block | `packet.cpp`, `rtsp.cpp`, `video.cpp`, `stream.cpp` | `NegotiatedWireLimitAlsoBoundsOuterRtpGeometry`, `HonorsSmallerNegotiatedWireLimitWithoutSplittingInnerPackets` |
| Protected TLVs and codec bytes participate in block-aware FEC | `packetizer.cpp`, Windows `transport.cpp`, common-c reassembly | Loss/reorder/duplicate/metadata-boundary tests in `test_pyrowave.cpp` |
| Optional Runtime TLV; static HDR stays on control channel | `transport.cpp`, `stream.cpp::send_hdr_mode` | Runtime/no-metadata transport tests; HLG presentation/control delivery unverified |
| Explicit negotiation; reject incompatible modes without codec retry | `rtsp.cpp` PyroWave ANNOUNCE validation, `video.cpp` failure paths | Capability/selection unit tests; direct RTSP regression coverage absent |
| Shared capture, pacing, stop and error boundaries | `video.cpp` encoder dispatch and capture loop; Windows display conversion | Static integration inspection; unaffected audio/input/USB/clipboard runtime checks absent |
| Dynamic HDR selection and luminance-analysis gate | `dynamic_hdr_selection.cpp`, `rtsp.cpp`, `display_vram.cpp` | `PyrowaveMappingSelectsOnlyTheRequestedBaseCompatibleFormat`; client preflight outside server scope |
| Retain valid scene on missing/invalid samples; reset on rebuild; no invented default | `src/pyrowave/dynamic_hdr.cpp`, GPU startup timeout | Producer gap/invalid/reconfigure/black-scene tests |
| Full required dynamic metadata in each protected frame | `dynamic_hdr.cpp`, `transport.cpp` | `CachedSceneMetadataStillTravelsInEveryProtectedFrame`, same-frame transport tests |
| Vivid statistics/UWA subset and PQ/HLG codes; HDR10+/DV supported profiles | `video_hdr_metadata.h/.cpp`, `dynamic_hdr.cpp` | Profile fixtures, Vivid syntax/base-curve tests, independent Dolby RPU parser tests |
| CPU recovery only for exclusive unenhanced SDR capture | `capture_policy.h`, shared-capture policy in `video.cpp` | Capture-policy admission/fallback tests; concurrent-stream runtime verification absent |
| Terminal backend failures do not escape into the process | `video.cpp` failure handler; CPU/GPU frame entry points use `encode_guard.h` | Four portable guard tests cover original errors, allocation/unknown exceptions and throwing diagnostics |
| Display switches, device loss, reconnect and long-running sessions | Existing shared capture lifecycle | Requires Windows hardware; not established by pure session-state tests |

## Validation record

- Baseline: `b052a5fc88fe90bd9b849c45b3fda2024e0676a5`.
- Dependency pins inspected: PyroWave `cc91cbf6363721d19d135d05ca8e3d8665ea6afd`,
  common-c `ddd52e20252ac2e27b4137b6db24530cf96fdfbb`.
- Linux temporary standalone CMake harness uses the repository's existing
  portable test/probe source lists and pinned Boost 1.92.0 FetchContent source.
  Debug assertions remain enabled. It is not a full Sunshine build.
- Baseline: 29 portable unit cases and packetizer probe passed.
- After exception-isolation changes: 33 portable unit cases passed; CTest
  passed both the unit target and packetizer probe (2/2 entries).
- Independent static review of the exception-isolation change passed.
- Focused Windows [Actions run 40329815631](https://github.com/ruiwai/foundation-sunshine/actions/runs/40329815631)
  passed at `eca8a1b04f76e85c4215582d64ef87b48800be76`. Logs inspected with
  `gh run view --log` confirm both changed CPU/GPU encoder objects compiled
  and `pyrowave_unit_tests` plus `pyrowave_packetizer_probe` passed (2/2 CTest
  entries). Successful-test output does not expose individual Windows case counts.
- The focused dispatch input `pyrowave_tests_only=true` skips the full suite
  and packaging; normal CI defaults are unchanged. The earlier full run was
  cancelled in favor of this scope at the user's request, not reported as passed.
- Windows-only tests were executed in CI, not locally. Hardware-dependent
  capture/encoding/presentation and a full Sunshine link remain unverified.

## Boundaries and outstanding runtime evidence

This checklist does not claim a client implementation, native Vulkan Vivid
presentation, certified HDR10+/Dolby output, Linux server support, or broader
device/multi-client guarantees. Shared dependencies were inspected at their
existing pins, not changed. Native `yuv2rgb.frag` uses 8-bit-form range constants;
bit-depth-aware client decode/display behavior is not established by this
server audit and requires a separate client-side check.

Before claiming end-to-end validation, exercise supported SDR/PQ/HLG range
combinations, dynamic metadata delivery, GPU failure/CPU recovery, device loss,
display changes, reconnect and soak scenarios on paired Windows/Android hardware.
Compare latency, bandwidth and quality only under identical hardware/network
conditions. Unit tests and CI builds cannot substitute for those observations.
