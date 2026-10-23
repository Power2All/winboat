# DirectX fixes: to-do list

The failures from the DirectX test (`tools/dx-test`) in the WinBoat VM (run of 2026-10-14, gpu.35: 367 passed, 27 failed).
Each item is fixed in the spike first, checked with the DirectX test there, then everything goes to the WinBoat VM together.

Legend: `[ ]` open · `[~]` in progress · `[x]` done

## Direct3D 9 (driver: Mesa d3d9umd + gallium nine)

- [x] Fixed-function lighting crashes the app: Windows' fixed-function → vertex shader conversion emits `ifc` in
      `vs_2_0` shaders (2_x shaders carry the 2.0 version token); nine refused it and its `endif` underflowed.
      Nine now checks 2.0 shaders against the 2_x instruction set.
- [x] Texture format A8L8 not offered: the host has no L8A8; kept as R8G8 with an `rrrg` sampler swizzle.
- [x] Volume (3D) textures: system memory volume textures (managed pool) and the `VolBlt` DDI (UpdateTexture).
- [x] Occlusion and event queries: "not ready yet" must be `S_FALSE`; the runtime treats `WASSTILLDRAWING` as a lost
      device and returns `E_FAIL`.
- [x] (extra) Timestamp queries offered (TIMESTAMP, TIMESTAMPDISJOINT, TIMESTAMPFREQ).

## Direct3D 10/11 (driver: Mesa d3d10umd + virgl)

- [x] Stream output + DrawAuto crash:
  - [x] stream output from a vertex shader (no GS): the "empty" GS went to virgl with no code (crash); the VS now
        carries the stream output declaration
  - [x] buffers with several bind flags (vertex + stream output, vertex + index): virglrenderer refused them and the
        context died; virgl adds the sampler-view flag the host needs
  - [x] DrawAuto passed no draw range (crash in virgl)
  - [x] DrawAuto drew the whole buffer: the host now draws with `glDrawTransformFeedback`
        (virglrenderer patch 0003, image 6.05-6)
  - [x] unbinding stream output (NULL targets) made the host begin an empty transform feedback object at the next
        draw (GL_INVALID_OPERATION, the context's rendering stopped); virgl drops trailing NULL targets
  - [x] the device's stream output target references
- [x] sRGB render targets: clears weren't encoded (the host clears on a framebuffer of its own); virgl clears a
      linear view with the encoded color.
- [x] Blending on sRGB render targets (same cause).
- [x] Pipeline statistics returned zeros: D3D11's query types (with HS/DS/CS counts, per-stream SO queries) weren't
      known; virgl returned one statistic per query (the host's queries are per statistic) and now makes all of them.
- [x] (extra) Timestamp-disjoint queries made a host context error (virglrenderer doesn't know them): emulated in
      the guest. SO statistics queries: made of the host's primitives emitted/generated queries.

## Only in the WinBoat VM (not in the spike)

- [x] Bit-block transfer (bitblt) swap chains: the window read black (D3D9 present, DXGI DISCARD/SEQUENTIAL, 4x MSAA
      swap chain). Gone with the image update (6.05-4 → 6.05-6).
- [x] Presenting with vsync ran at ~7 frames per second: 64 now, with the image update.

## Wrapping up

- [x] DirectX test in the spike: 395 passed, 1 failed (no D3D12 driver), 24 optional; desktop fine after a reboot
- [x] Export the Mesa patch (0012) and the virglrenderer patch (0003), the patch series reproduces the sources
- [x] Full driver package build in the spike (driver 100.6.101.58749)
- [x] WinBoat gpu.36 (image 6.05-6) installed, driver installed in the WinBoat VM, container on image 6.05-6;
      DirectX test there: 395 passed, 1 failed (no D3D12 driver), 24 optional, 10 skipped
- [x] docs/hardware-acceleration.md and the notes updated

## Not in this round (new features, not bugs)

- Direct3D 12: needs a whole new driver (D3D12 user-mode driver, most likely on Vulkan/Venus, which is parked)
- Optional D3D11 features reported as ⚠️: feature level 11_1, doubles in shaders, 11.1 logic ops, tiled resources,
  11.3 features, video decoding, optional formats (B4G4R4A4, R32G32B32 rendering, NV12)
