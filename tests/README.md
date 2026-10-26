# Hardware regression tests

Run on the supported Iris board. These are hardware integration tests, not host
unit tests. C/C++ files include build commands in their headers; FFmpeg tests
also require `ffprobe` and software H.264/HEVC decoding.

`meson test -C build` also checks CAPTURE buffer budgeting without hardware.

| Test | Coverage |
| --- | --- |
| `decoder/main10.cpp` | HEVC Main10 / AV1 8/10-bit capabilities, config validation, surface enumeration, and context bit-depth selection. |
| `decoder/av1.py` | FFmpeg/GStreamer AV1 pixels against dav1d: 720p–4K, 8/10-bit, hidden references, tiles, lossless, still pictures, super-resolution and global motion. |
| `decoder/prime-import.cpp` | PRIME1/PRIME2 NV12/P010 imports, malformed descriptors, shared storage, and caller FD ownership. |
| `decoder/error-recovery.py` | Corrupt HEVC, queue errors, neutral NV12/P010 pixels, B frames, consecutive errors, DRM mapping, and persistent/imported storage. |
| `encoder/ffmpeg-smoke.py` | Profiles, rate control, dimensions, frame counts, keyframes, and decoded quality. |
| `encoder/api-smoke.cpp` | Uploads, direct/staged input, rejected requests, coded buffers, and resource cleanup. |
| `encoder/async.cpp` | Queued versus serial output, QP/IDR changes, non-IDR I requests, synchronization, timeouts, and pending resource destruction. |
| `encoder/writable-export.cpp` | GPU-written input, stable DMA-BUF exports, and GPU fence synchronization across non-IDR GOPs. |

To select an uninstalled driver before running a test:

```sh
export LIBVA_DRIVER_NAME=qcom LIBVA_DRIVERS_PATH="$PWD/build"
```

The PRIME import test requires `/dev/dma_heap/system`. It checks padded layouts
and nonzero offsets, and verifies that importing and exporting retain the same
DMA-BUF rather than copying its pixels.

Run AV1 on a board exposing the V4L2 AV1 format, with the NOSHOW kernel patch:
`python3 tests/decoder/av1.py tmp/av1 --driver-dir build --gstreamer --extended`.
Requires FFmpeg with libaom/dav1d; `--gstreamer` also needs the `va` plugin and
GStreamer app/video development libraries. `--extended` adds coding-tool cases
and requires `aomenc`. Passing decoded files are removed; logs and streams remain.

Run error recovery with `python3 tests/decoder/error-recovery.py tmp/recovery
--driver-dir build` on a kernel with the error-timestamp patch. It generates
HEVC samples with libx265 and builds the adjacent preload shims. Besides real
corruption, it checks that injected errors replace only the affected frames,
exports retain their DMA-BUF, and failed fills or missing timestamps do not
report successful recovery.

The FFmpeg, async, and writable-export tests accept `--hevc` to exercise HEVC
Main instead of H.264. The HEVC async test also checks rejected picture types,
tiles, partial slices, invalid QP/filter values, and long-term references.

Place generated streams and logs under `tmp/`. The async and writable-export
programs take an existing output directory as their first argument.

Optional failure injection helpers (build commands are in their headers):

- `reject-coherent.c`: load with `LD_PRELOAD` to test the CPU/MMAP fallback on
  hardware that normally supports coherent GEM. Works with all encoder tests.
- `delay-completion.c`: load with `LD_PRELOAD` and set
  `IRIS_TEST_DELAY_COMPLETION=1` when running `async` to test caller timeouts and
  the hardware wait watchdog.
- `decoder/error-completion.c`: load with `LD_PRELOAD` and set
  `IRIS_TEST_ERROR_QUEUE=capture`, `output`, or `both` to inject errors at
  decode token 46. `IRIS_TEST_ERROR_FIRST` and `IRIS_TEST_ERROR_COUNT` select
  another frame or consecutive errors. Errors must complete without a sync
  or context destruction timeout, including late CAPTURE after OUTPUT errors.

For staged async input, set `IRIS_TEST_STAGING=1`; add `IRIS_VAAPI_COPY=cpu` to
exercise CPU staging. Without that override, staging uses the build's default
copy path.

## Build options

`python3 tests/build/gpu-copy.py tmp/build-options` runs without Iris hardware.
Install all GPU-copy development dependencies first. It masks each dependency
in turn and checks `auto`, `enabled`, and `disabled`, source selection, linked
libraries, and driver loading. Logs and builds remain in the output directory.

On the board, also run the decoder recovery and encoder staging tests against
`-Dgpu_copy=disabled` without setting `IRIS_VAAPI_COPY`, to exercise the default
CPU path. Explicit `IRIS_VAAPI_COPY=gpu` must fail only when a copy is needed;
DMA-BUF zero-copy remains usable.

## CDSP VPP

`meson test -C build --print-errorlogs` checks MSM allocation cache modes,
skeleton selection and HVX worker limits without hardware. Build with CDSP
enabled, then run on the target (upstream FastRPC kernel ABI):

```sh
c++ -O2 -std=c++17 -rdynamic tests/vpp/scaling.cpp -o build/vpp-scaling \
  $(pkg-config --cflags --libs libva libva-drm libdrm vulkan) -ldl \
  -Lbuild/src/cdsp -liris_vpp
export LIBVA_DRIVER_NAME=qcom LIBVA_DRIVERS_PATH="$PWD/build"
export LD_LIBRARY_PATH="$PWD/build/src/cdsp${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export ADSP_LIBRARY_PATH="$PWD/build/src/cdsp${ADSP_LIBRARY_PATH:+;$ADSP_LIBRARY_PATH}"
./build/vpp-scaling
```

Checks NV12/P010 scaling, conversion and rotation, rounding, crop/output layouts,
DMA-BUF identity, persistent mapping, context/buffer lifetime and rotation with
five live contexts sharing scratch resources. Also run
against a `-Dfastrpc=bundled` build. Pass `vulkan` to run the same VAAPI cases
and check persistent Vulkan imports instead of FastRPC mappings. For a build
without CDSP, add `-DVPP_NO_CDSP` and omit `-Lbuild/src/cdsp -liris_vpp`.
`--bench` measures 100 synchronous submissions after a one-second warmup,
excluding uploads and reference generation.

`vpp/tonemap.cpp` checks Vulkan HDR10 → SDR against a double-precision reference,
including metadata changes, padded/cropped layouts, DMA-BUF identity, persistent
imports and context lifetime. Build with the command in its header and run with
the same driver environment; `--geometry` also checks combined HDR, scaling and
rotation with the selected VPP backend. It reports warmed VAAPI call times
for 1080p and 4K.

`vpp/overlay.cpp` checks both overlay backends against a double-precision
reference, including RGB upload/PRIME round trips, clipping, scaling, alpha,
multiple layers, rejected submissions and parameter lifetimes. Build with the
command in its header. `--geometry` adds combined base scaling; `--bench`
measures warmed VAAPI submissions including the base copy, excluding uploads
and encoding. Pass `cdsp` or `vulkan` to test a single backend.

Optional ABI audit against SDK 6.7 (all six architectures):

```sh
python3 tests/vpp/check-abi.py /path/to/19.0.02/Tools /path/to/HexagonSDK6.7
```
