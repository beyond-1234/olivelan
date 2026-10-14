# Golden datasets (Phase 6 / patch 0029)

Immutable regression baselines recorded **before** the Phase 7 / Phase 8 surgery,
so the rewrite of the underlying code can be asserted against the pre-rewrite
behaviour.

## 1. `pixel_convert/` — pixel-conversion matrix

One binary snapshot per `Frame::convert(src -> dst)` combination:

```
pixel_convert/convert_<SRC>_to_<DST>_c<CHANNELS>.bin
```

* `SRC` / `DST` ∈ `{U8, U16, F16, F32}`, `CHANNELS` ∈ `{1, 2, 3, 4}` (48 files).
* Only the pixel-bearing bytes of each row are stored; the 32-byte row-alignment
  padding produced by `Frame::allocate()` is uninitialised and deliberately
  excluded.
* The input frame is generated deterministically (per-channel `(idx * prime) %
  251` ramps, see `Sample()` in `tests/codec/color-golden-tests.cpp`), so the
  snapshot depends only on the conversion code.

Produced by `Frame::convert()`. The snapshots were originally recorded while that
function was still OpenImageIO-backed, and patch 0031 rewrote it without OIIO
against exactly these values, so they remain bit-identical and now pin the
in-tree implementation. Consumed by `tests/codec/color-golden-tests.cpp`:

```bash
# verify (default, run by ctest)
./color-golden-tests

# re-record (ONLY when intentionally rebasing the baseline)
OLIVE_GOLDEN_RECORD=1 ./color-golden-tests
```

Assertion rules (from the Phase 6 spec):

* integer destinations (U8 / U16): **bit-exact, zero error**;
* float destinations: `|Δ| < 1e-5` for F32, `|Δ| < 1e-3` for F16 (binary16 has
  ~11 significant bits, so 1e-5 is not attainable).

This is the gate for **patch 0031** (`Frame::convert()` rewrite without OIIO).

## 2. Colour-equivalence set — NOT YET RECORDED (runtime-gated)

The second half of the 0029 spec records Golden frames from the **live OCIO
render pipeline**:

| case | path |
|---|---|
| Rec.709 → Linear → sRGB Display | standard video clip through the colour manager |
| sRGB still image load | `.png` ingest |
| standard `.cube` 3D LUT node | LUT generator node render |

Unlike the pixel matrix, this half needs a real GL/OCIO runtime (the colour
manager builds GLSL from the OCIO config) and must be captured **before** the
OCIO pipeline is replaced in Phase 8. It therefore has to be recorded on a GPU
session, not offline:

1. Build with the colour pipeline intact (`OLIVE_ENABLE_OCIO=ON` once patch
   0037/0038 land — today: unmodified build).
2. Import the reference clip still + the reference `.cube` file.
3. Render each case at a fixed timeline position and dump the viewer output as
   a lossless PNG/EXR under `tests/golden/color_equivalence/`.
4. Commit the dumps; Phase 8's scoped gate (patch 0044) compares against them
   with the narrowed-domain criteria (mean ΔE₀₀ < 1.0, PSNR > 45 dB).

Recording this set is the precondition for the Phase 8 replacement patches; it
is tracked here rather than faked.
