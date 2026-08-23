# batman_lgu CM11 graphics porting log

## Baseline investigation

- Target: LG Linux 3.4.0, branch `cm-11.0`, baseline `a368e6a7d50`.
- Reference: HTC pyramid Linux 3.4, branch `cm-11.0`, baseline
  `3f45cd2715c`.
- The repositories have no directly usable common Git ancestry. The LG tree's
  graphics baseline is an early CAF 3.4 generation: Android sync sources and
  dma-buf exist, but sync is disabled and KGSL exposes only the genlock-backed
  timestamp event at ioctl `0x31`.
- Required dependency order identified in pyramid:
  `010accf8a0e` (sync framework) -> `fe6c39ce093` (KGSL fence event) ->
  `0b120d0c31f` (timestamp comparison) -> `df5f3c82303` (MDP buffer sync) ->
  `76f0c66f9e9` (display commit).
- A3xx/8960/MDSS changes and HTC board, panel, power and color changes are
  excluded from the initial msm8660/A2xx port.

## Stage A/B1: sync framework and KGSL timestamp fence

Reference commits:

- `fe6c39ce093` - msm: kgsl: Add support for Android's sync point framework
- `0b120d0c31f` - msm: kgsl: implement sync compare callback
- `88b5889050e` - msm: kgsl: Remove extra call to sync_fence_put()

Adaptation notes:

- Kept LG's existing KGSL event list, context ownership check and genlock path.
- Enabled `CONFIG_SYNC` and `CONFIG_SW_SYNC` only for `batman_lgu_defconfig`.
- Preserved ioctl `0x31` as `IOCTL_KGSL_TIMESTAMP_EVENT_OLD`; added the CM11
  read/write ioctl at `0x33` and fence event type 2.
- Recomputed `_IOC_NR(cmd)` after legacy ioctl remapping.
- Moved `sync_fence_install()` to the final success path. This makes all error
  paths operate on an uninstalled fd and avoids the original double-put/invalid
  `put_unused_fd()` sequence.
- Fence callbacks signal the requested timestamp, not a possibly newer retired
  timestamp, and retain the context until callback/cancellation completion.

Validation:

- `git diff --check`: clean.
- `make bootimage -j16`: passed; `kgsl_sync.o`, vmlinux, zImage, modules and
  `out/target/product/batman_lgu/boot.img` built successfully.
- Build configuration still reports duplicate source/vendor display HAL output
  producers. This must be resolved before a full ROM validation.

## Stage D/E1: MDP buffer sync and display commit

Reference commits:

- `df5f3c82303` - msm: display: Buffer sync point support
- `028d03daff2` - msm: display: buffer sync point update
- `182c21fbfa2` - msm: mdp: Release all fences on blank
- `2f3c438ffec` - msm: display: buf sync enhancement
- `76f0c66f9e9` - msm: display: add display commit ioctl
- `309c0ca6eb7` - msm_fb: display: Consolidate commit ioctls

Adaptation notes:

- Added CM11 `MSMFB_BUFFER_SYNC` ioctl 162 with up to ten acquire fences and
  release/retire fence output.
- Added `MSMFB_DISPLAY_COMMIT` ioctl 164 and connected overlay commits to the
  existing LG MDP4 mixer-specific commit implementation.
- Kept the CM10 `MSMFB_OVERLAY_COMMIT` path and distinguished the legacy LG
  metadata ioctl at number 162 by its encoded structure size. Metadata number
  166 is also supported for the CM11-built userspace.
- Fence fds are installed only after both output values are copied successfully,
  so failures can use `put_unused_fd()` without touching an installed file.
- Panel blank drops pending acquire references and advances the timeline so
  release/retire fences cannot remain indefinitely pending.

Validation:

- `make bootimage -j16`: passed; `msm_fb.o`, `mdp4_overlay.o`, vmlinux, zImage,
  modules and boot image built successfully.
