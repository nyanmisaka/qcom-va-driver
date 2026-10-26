# CDSP VPP

NV12/P010 scaling, bit-depth conversion and 90°/180°/270° rotation using
128-byte HVX vectors. Scaling uses area reduction and Catmull–Rom enlargement,
with dedicated 2× paths.
See [supported inputs and usage](../../README.md).

## Build and runtime

- Linux ARM64 build machine; GHC with `syb` (`ghc haskell-syb` on Arch,
  `ghc libghc-syb-dev` on Debian). No full Hexagon SDK required.
- Meson downloads pinned QAIC and Hexagon Open Access **19.0.02** archives.
  The toolchain is about 855 MiB compressed and needs several GiB extracted.
- FastRPC defaults to the system library, falling back to pinned source.
  The fallback needs libyaml/libbsd development files. Use `-Dfastrpc=system`
  or `bundled` to force either; `-Dfastrpc_include_dir=...` overrides headers.
- Offline builds: prefill `subprojects/packagecache/` using filenames and
  hashes from `subprojects/*.wrap`, then pass `--wrap-mode=nodownload`.
- Target: CDSP firmware/runtime libraries, unsigned FastRPC support,
  `/dev/fastrpc-cdsp` and board FastRPC configuration. Buffer allocation uses
  `/dev/dma_heap/system` with coherent MSM GEM as a fallback.

Every build produces v68/v69/v73/v75/v79/v81 skeletons. The helper detects the
architecture and loads the exact match; unsupported architectures fail.
The install path is tried first unless `ADSP_LIBRARY_PATH` is set and nonempty.
Default installation with `/usr` prefix:

- Skeletons: `/usr/lib/dsp/cdsp/qcom-va-driver/vpp_v<arch>_skel.so`
  (`-Dcdsp_firmware_dir=...` to override).
- Host helper and optional private FastRPC library: `libdir/qcom-va-driver`.

For uninstalled builds, set `LD_LIBRARY_PATH` and `ADSP_LIBRARY_PATH` to
`build/src/cdsp`; see [test commands](../../tests/README.md#cdsp-vpp).

## Maintenance

- `dsp.c` / `filter.h`: Q7 area and Q14 cubic filters, cached per context.
  P010 operations retain signed Q4 intermediates; NV12 output rounds and
  saturates after filtering. Conversion preserves color range and matrix.
  Rotation transposes pixel groups (keeping U/V paired); 90°/270° HVX tiles
  write 128-byte spans using 32–128 rows. Strips and transpose workspace
  prefer VTCM, falling back to aligned memory when unavailable. Scratch is
  sized for active workers and released when no longer needed or reconfigured.
  Active workers follow the HVX count (1–8); each context currently allocates
  eight DSP threads. Multiple contexts therefore share a finite thread budget.
  Contexts request maximum core/bus performance.
- `client.cpp`: persistent DMA-BUF mappings, retired before the last RPC
  context closes and remapped lazily if surfaces survive. Imports retain
  their FDs; submissions and mapping/session changes are synchronized.
- `qurt_abi.h`: minimal firmware declarations, not a QuRT implementation.
  Layouts/constants are audited against SDK 6.7; keep the short-enum ABI.
  All six architectures build and pass the ABI audit; hardware tests cover v68.

Run [scaling, mapping-lifetime and ABI checks](../../tests/README.md#cdsp-vpp)
when changing these interfaces.
