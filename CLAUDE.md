# CLAUDE.md

Project-specific instructions for Claude Code when working in this repository.

## Commits

Never add a `Co-Authored-By: Claude` or `Claude-Session:` trailer to commit messages in this repo, and never push such a trailer to any remote. Commits should be attributed to the human author only.

## Shader compilation (Cg / cgcomp)

This Mac is arm64 (Apple Silicon). PSL1GHT's shader compiler `cgcomp` (`/usr/local/ps3dev/bin/cgcomp`) is a native arm64 binary, but NVIDIA's Cg Toolkit (which `cgcomp` needs to compile actual `.cg` source) was discontinued in 2012 and only ever shipped for ppc/i386/x86_64 — no arm64 build exists or ever will.

Two ways to compile shaders here:

- **`cgcomp -a` (assembly input)** — works natively on arm64, no Cg Toolkit needed at all. `InitCompiler()`/`dlopen` is skipped entirely when `-a` is passed; it goes straight to cgcomp's own NV30/40 assembler. Use this if you don't need to write shaders in the Cg language.
- **`cgcomp-x86_64` (real `.cg` source)** — a custom x86_64 build of `cgcomp`, at `/usr/local/ps3dev/bin/cgcomp-x86_64`, run via Rosetta:
  ```bash
  arch -x86_64 /usr/local/ps3dev/bin/cgcomp-x86_64 -v shader.vp.cg shader.vpo   # vertex
  arch -x86_64 /usr/local/ps3dev/bin/cgcomp-x86_64 -f shader.fp.cg shader.fpo   # fragment
  ```
  This needed two things installed system-wide (both already done on this machine, not part of the repo):
  1. NVIDIA Cg Toolkit 3.1 (April 2012, Mac dmg) — only `Library/Frameworks/Cg.framework` was installed, to `/Library/Frameworks/Cg.framework`.
  2. `cgcomp` rebuilt from [ps3dev/PSL1GHT `tools/cgcomp`](https://github.com/ps3dev/PSL1GHT/tree/master/tools/cgcomp) source with `-arch x86_64`, with two source patches in `main.cpp`:
     - `InitCompiler()`: `dlopen("Cg", RTLD_LAZY)` → `dlopen("/Library/Frameworks/Cg.framework/Cg", RTLD_LAZY)`. The bare `"Cg"` form relied on an implicit framework-search fallback that modern macOS's dyld no longer does, so the original source fails on any current macOS regardless of architecture.
     - `cgDefArgs[]`: `"-fastprecision"` was removed from the default Cg compiler args during debugging. This was almost certainly **not** necessary (see below) -- it changes `MOVH`/`h0` to `MOVR`/`r0` in the dumped assembly, which is harmless, but it was not the cause of the black screen. Left in place because it's benign; re-adding it is fine if ever needed.

  If this ever needs rebuilding (e.g. on another machine): pull the same `tools/cgcomp` source, compile with `g++ -arch x86_64 -std=c++11 -O2 -I include source/*.cpp -o cgcomp-x86_64`, apply the `dlopen` patch above, and make sure `/Library/Frameworks/Cg.framework` exists (from the Cg Toolkit 3.1 Mac dmg, `developer.download.nvidia.com/cg/Cg_3.1/Cg-3.1_April2012.dmg`).

### Getting a shader-drawn triangle to actually render (what went wrong, what fixed it)

The first shader-drawn triangle in this project showed a **black screen for a very long time**, across dozens of attempts. Two things sent the debugging badly astray and are worth knowing so nobody repeats them:

1. **`RSX: ROP reads from r0 without writing to it. Final value will be gathered.` in the RPCS3 log is a red herring.** RPCS3 emits this for *every* fragment program it loads, including ones that render perfectly (verified in `shader-triangle-test/rsxtest-bisect`, which logs it and renders fine). Do **not** use its presence/absence as a pass/fail signal. Only the actual rendered image counts.
2. **The shader and `cgcomp` were never the problem.** A lot of effort went into suspecting a cgcomp encoder bug on "trivial" fragment shaders. That theory was wrong: the exact known-good `diffuse_specular_shader` also rendered black in Cell-8 until the RSX *draw state* was fixed.

The actual cause was **missing / wrong RSX pipeline state in `source/triangle/triangle.cpp` and `source/rsxutil.cpp`**. All of the following were applied together (from the LOCAL verified copy at `shader-triangle-test/rsxtest-bisect/`, *not* a GitHub fork -- the bucanero fork uses an older PSL1GHT API and differs in exactly these details), after which the triangle rendered:

- Depth surface `GCM_SURFACE_ZETA_Z16` → `GCM_SURFACE_ZETA_Z24S8`, depth clear `0xffff` → `0xffffff00` (`rsxutil.cpp` `initRenderTarget()` + `triangle.cpp`).
- `rsxSetUserClipPlaneControl(ctx, DISABLE ×6)` before the draw -- never called anywhere in this project before.
- Vertex attributes bound to hardware slots `GCM_VERTEX_ATTRIB_POS` / `_NORMAL` / `_TEX0` (0/2/8), not the index from `rsxVertexProgramGetAttrib(...)->index`.
- `rsxSetZMinMaxControl(ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE)` (was `0,1,1`).
- `rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE)` before texture setup, and an explicit `gcmTexture.remap` (was left 0).
- Plus, from earlier in the same investigation: `setRenderTarget(curr_fb)` explicitly before the first frame (Cell-8's `initRenderTarget()` only fills the struct, it never calls `rsxSetSurface`), GPU-side clear via `rsxClearSurface` instead of a CPU `memset` into the bound render target (that memset caused RPCS3's "Cannot invalidate a currently bound render target!" + flicker), viewport with **negated Y scale** (`h * -0.5f`), `rsxSetColorMaskMrt(ctx, 0)`, depth test **on** with `GCM_LESS`, `rsxSetShadeModel(SMOOTH)`, `rsxSetFrontFace(CCW)`, and the unused MRT color slots 1-3 set to offset `0` (they were aliased onto `color_offset[curr_fb]` with a bogus pitch).

Which subset of those was strictly necessary was not bisected -- they were applied as a batch to stop the churn. If you ever want to trim, bisect against the working state, one change at a time, checking the screen (not the log).

The Makefile also gained `-O2`. `-Os` (which the upstream sample uses) triggers a GCC internal compiler error (`rs6000_savres_routine_name`) on `triangle.cpp` with this toolchain; `-O1`/`-O2` build fine. `-mcpu=cell` was tried and removed again (it ICEs together with `-Os`); it is not currently set.

When porting RSX code from a PSL1GHT sample, **use `shader-triangle-test/rsxtest-bisect/` (or `rsxtest-stock/`) on this machine as the reference** -- it is built against the same PSL1GHT headers as Cell-8 and is confirmed to render here.
