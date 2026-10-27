// ---- the aged foam's scratch scale (interface version 6): Wave Prepare (Ninja_WavePrepare) hands F2 to Wave Step (Ninja_WaveStep) through the
// 16-bit foam target as hi = F2 rounded to a half float (.y) and the remainder lo = F2 - hi stored times FoamAgedLoScale (.z); Wave Step
// reads F2 = .y + .z * (1 / FoamAgedLoScale). Both modules include this one constant, so they cannot disagree.
// Why the scale: lo is at most half a half-float ulp of hi (2^-12 to 2^-11 of F2), so unscaled it is a half-float denormal (below 2^-14)
// whenever F2 is below 0.25, about half the time below 0.5 and a quarter of the time below 1. A GPU that flushes fp16 denormals on the
// typed UAV store or load reads it as 0: F2's slow decay is rounded away again and the aged foam stalls (a 50 s half-float simulation with
// lo flushed: 0.2 ends at 0.183 instead of 0.074, 0.1 at 0.092 instead of 0.037, a permanent faint trail). Times 2^11, |lo| is at most the
// bottom of hi's binade (about hi, never near the half-float limit) and stays a normal half float down to lo = 2^-25 (F2 about 1e-4,
// invisible); a power of two and its inverse scale exactly, so the scale adds no rounding of its own. A missing .z (an RG target) still
// reads 0: lo is dropped and F2 keeps 16-bit precision.
static const float FoamAgedLoScale = 2048.0;
