# Private pipeline implementation evidence

Implementation-first work is authorized; hardware acceptance remains deferred.
Client wire fields and protocol versions remain unchanged.

Fixed-capture contract/v1 preserves the negotiated even size without alignment,
rotation, scaling or resolution-change events; capture-size changes fail closed.
PyroWave does not admit RAM capture through shared capture policy.
Portable size-predicate coverage only; Windows compilation/runtime pending.

## Delivered slices

- Sunshine branch: `pyrowave/uhd770-private`.
- `1c5bde8`: SDR BT.709/YUV420 signal checks and atomic single-video admission.
  Windows run `40341402933` passed.
- PyroWave fork `ruiwai/pyrowave`, branch `uhd770/fp16-only`, commit
  `76890c5`: mandatory native FP16 compute wavelets, removed precision switches,
  regenerated shaders, and a GPU-free enabled-feature rejection test.
  Sunshine pin: `ac8bc8c`; focused native-test wiring: `03d3835`.
  Windows run `40349371844` passed all three focused CTest entries.
- The actual embedded DWT/IDWT SPIR-V passed validation and contains half
  arithmetic. Two shader-generation runs produced identical artifacts.
- `710ff36`: centered-chroma fused SDR shader and Windows WARP reference tests.
  The predetermined tolerance is one 8-bit code value per sample, with exact
  uniform black/white endpoints. The Windows unit binary includes these tests;
  individual-case output was not retained by the successful CTest invocation.
- `dff0fa6`: two-slot plane ownership; reuse follows the existing release-fence
  wait. Errors invalidate the ring. Refreshes re-encode retained planes, not
  cached packets. Local portable tests: 45 cases plus packetizer probe passed.
  Windows run `40356085453` passed three focused CTest entries and changed
  backend compilation, including `display_vram.cpp`.

## Known failing correctness check

The native 34x30 round-trip validator ran on Linux Intel ADL GT2
(`8086:46a1`), not Windows UHD770. It failed with maximum absolute 8-bit errors
Y=4, Cb=3, Cr=3 against its existing limits Y=1, Cb=2, Cr=2 (exit 5).
Diagnostic changes expose these errors without changing the limits.

Reverted experiments: half-vector fused multiply-add yielded 4/3/2;
initial quantization bits 8 with cap 512 yielded 2/2/2; combining them yielded
2/2/1. None passed. Original shader/encoder sources and generated header were
restored and the original validator rebuilt. No FP32 fallback was introduced.
FP16 numerical correctness is therefore **not accepted** by this check.
The user waived further investigation of this error; it remains a known failure.

## Remaining integration and validation

Production now routes fused conversion into shared R8 planes and rejects
non-borrowed/unsupported sources; Windows validation of this routing is pending.
Capture lifecycle, compatibility deletion and event-driven idle remain pending.
Unreachable native fragment-renderer cleanup remains pending too.
No full Sunshine link, Windows GPU interop runtime, paired-client decode,
UHD770 quality/performance measurements or hardware soak is claimed here.
- Native FP16 storage preflight + diagnostics: static review PASS; Linux build and feature test 1/1 PASS. Windows CI `40369678709` at `9bb052d`: changed objects compiled, including video/display; native features, probe and original WARP color test passed; borrowed-source WARP test failed.

## Current requirement evidence (not hardware acceptance)

- Native `a1235e1`, host preflight pin `c39913a`; production fusion/startup `1ca36e8`.
- Production CI `40365767443` at `1ca36e8` failed Windows compilation on protected capture identity fields; public const noexcept accessors fix the wrapper accesses; changed-object compilation passed in `40369678709`.
- WARP borrowed-source test `6c482b8` failed in `40369678709`; acquire-before-binding ordering fix verification and new source-lease Windows coverage remain **PENDING**. No full Sunshine link/runtime claim; historical passes do not verify this fix.
- Source-lease strict-success/v1 checks require `S_OK` on borrowed release and retain
  pending leases/accounting when the borrowed mutex is missing; Windows checks pending.
- CI `40372952762` was blocked before build/tests by DualSense download HTTP 504.
  Replacement `40375057787` at `5c56d4e` built changed objects and three test binaries.
  Original WARP color/reference and all five source-lease tests passed; borrowed WARP
  failed with 540 pixel failures matching the producer's later white overwrite.
  Explicit source-completion correction now waits on D3D11 EVENT queries before handoff;
  Windows compilation/runtime validation remains **PENDING**; this is not resolved.

| Requirement | Code / tests; remaining evidence |
| --- | --- |
| Preflight | [native feature guards](../../third-party/pyrowave/pyrowave_features.hpp), [feature test](../../third-party/pyrowave/pyrowave_features_test.cpp); real Windows imports pending. |
| Admission | [video_admission_t](../../src/pyrowave/video_admission.h), [PyrowaveVideoAdmissionTest](../../tests/unit/test_pyrowave.cpp); hardware lifecycle pending. |
| Pipeline | [convert_direct](../../src/platform/windows/display_vram.cpp), [gpu_encoder.cpp](../../src/platform/windows/pyrowave/gpu_encoder.cpp); [WarpBorrowedSourceHandoffPreservesConvertedPlanes](../../tests/unit/platform/windows/test_pyrowave_conversion.cpp) pending Windows run. |
| FP16 | [pyrowave_common.cpp](../../third-party/pyrowave/pyrowave_common.cpp) feature rejection and wavelet policy; known numerical failure above remains unaccepted. |
| Simplification | [video.cpp](../../src/video.cpp) still has shared-capture/fallback orchestration; compatibility deletion pending, not proven by fused routing. |
| Idle | [video.cpp](../../src/video.cpp) has VRR/cached-plane refresh paths; dedicated event-driven policy and paired-client acceptance pending. |

All hardware gates remain deferred in the [hardware checklist](hardware-validation.md).

## Restricted-adapter preflight/v1

GPU-only DXGI Intel/UHD770 exact-identity preflight now precedes conversion planes
and native allocation. Identity is not feature/format/sync support; native and LUID
checks remain. CPU fallback is removed; PyroWave requires GPU capture and shuts down
on backend failure. Windows compilation of this deletion and the gate is pending.

Per-frame release verification/v1 checks D3D completion after each successful Vulkan
CPU wait, before ring release; failures retain ring poisoning and the 1000 ms event wait.
Portable coverage tests only the completion predicate; Windows compilation/runtime pending.
Lifecycle is **not complete**: native `321909d` bounds five encoder readback APIs with a 1 s `wait_timeout`, failing before mapping on timeout.
Helper fixture and native build/feature test (1/1) passed; context/device teardown and decoder waits remain unbounded.
GPU-stall behavior and an end-to-end bound remain unverified.
Windows run `40378882947` at `8377e82` **PASSED** changed-object compilation, three test-binary links, WARP 2/2, source-lease 8/8, 81 unit cases and 3/3 CTest entries.
It predates this CPU deletion, native `321909d` and new host-stage stats; their Windows validation remains **PENDING**, not full-app/hardware acceptance.

Host-stage baseline observations/v1: constructor logs ring slots, shared plane images, logical R8 plane bytes (excluding padding/driver allocation, not measured VRAM), and maximum bitstream buffer bytes.
Existing encoded-frame verbose logs add session identity and integer steady-clock microseconds for native encode call, return-to-checked-release/ring completion, and release-to-readback/packet flatten completion (before transport publication).
These host phases include CPU waits/work, not GPU-only timestamps or measured performance claims; existing conversion begin/end traces remain unchanged for later matched baselines. Windows compilation/hardware validation remains unverified.

SDR backend dead HDR work removed; existing rejection guards and SDR wire unchanged.
Windows compilation/runtime not yet verified; no speedup claim.

Packetizer reuses only temporary scratch; returned wire storage/spans remain independently owned.
Windows transport uses a thread-local instance; no benchmark or Windows compilation claim.
Packetized output transfers its full wire vector to transport without a full-wire copy only when uniquely owned; aliased output is copied without changing other owners.
This removes neither encoded-bitstream readback nor other network copies; it is not an end-to-end zero-copy or speed claim.
Focused Windows CI skips only unrelated Web UI/Node.js and DualSense sidecar/.NET steps.
Normal/default-false runs are unchanged; the new Windows workflow run remains pending.
