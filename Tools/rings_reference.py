"""Independent reference for FluidNinja Buoyancy's analytic ring layer: the sub-grid ripples a water contact's surface pressure
impulse sends out, which the surface material draws per pixel from a response table (the ring LUT) because the wave grid cannot
carry them. It recomputes that response from the linear Cauchy-Poisson theory with its own quadrature and Bessel functions, so the
plugin's LUT (and what fnb.Waves.RingsDump logs) can be checked against it, and it answers how big and how visible a contact's
rings are.

The physics (units cm, s, kg; impulses in kg*cm/s, + presses the water down):

    omega(k)^2   = (g k + (sigma/rho) k^3) tanh(k d)                  g = 981, sigma/rho = 72.8, tanh argument clamped at 20
    eta_hat(k,t) = -(I / rho) F(k) (k tanh(k d) / omega) sin(omega t) exp(-2 nu k^2 t)          rho = 0.001
    eta(r,t)     =  1/(2 pi) Integral eta_hat(k,t) J0(k r) k dk
    deta/dr(r,t) = -1/(2 pi) Integral eta_hat(k,t) J1(k r) k^2 dk
    F(k)         = exp(-k^2 sigma_b^2 / 2) - exp(-k^2 sigma_g^2 / 2)    (the ring band: the source minus what the grid carries)

sigma_b is the band's source sigma, sigma_g the Gaussian the same impulse goes into the wave grid with (2 wave cells, at least
sigma_b), d the simulation's depth and nu the ring viscosity. --full drops the grid term (F = the whole source spectrum: what a
grid fine enough for every wave would show).

The integrals use the midpoint rule on a fine k grid (default dk 2e-4 rad/cm, up to k sigma_b = 7.5, where the source spectrum is
below 1e-12) and scipy.special's Bessel functions, or, without scipy, this file's own series (x <= 12) and Hankel asymptotic
expansion (x > 12), accurate to about 1e-10. It prints:

    1. the response table for an impulse of 100 kg*cm/s (the LUT's unit) for the three bands at the radii and times
       fnb.Waves.RingsDump logs: slope, height, the stationary-phase wavelength (the LUT's B channel), the error of the LUT's own
       quadrature (dk = pi/1200 up to min(6/sigma_b, 3)) and of a bilinear read from a 512 x 512 LUT (r = 600 u^2, 8 s);
    2. checks: the sign (a downward impulse opens a depression at the centre first), the slope against a finite difference of the
       height, the volume (zero: the rings move water, they add none) and the k grid's convergence;
    3. the example contacts: a 3.5 kg plunge-diving bird (body band 6 cm, I = 3.5 kg x 1250 cm/s) and its head dip (band 2.5 cm,
       I = 0.3 kg x 150 cm/s): peak height and slope at 0.3, 0.6, 1 and 2 m over 0-6 s, visible crests (slope above 0.005) within
       1.5 m, crest spacing near 0.5 m, and the Wave Ring Strength range that keeps them visible without breaking.

Run it with any Python 3 that has numpy (scipy optional), from anywhere; it needs no Unreal:

    py rings_reference.py
    py rings_reference.py --sigma-g 23.44 --depth 335 --nu 0.05 --impulse-scale 0.35 --ring-strength 1
    py rings_reference.py --skip table,checks --ring-strength 0.06       (the example contacts only, at another strength)

The example contacts also print the linear field's energy against the moving mass's kinetic energy and the speed the surface under
the contact starts down at: a linear impulse response cannot know that most of an impact's energy goes into the cavity, the spray
and the deep flow, so where it puts as much energy into waves as the body had, its rings are too big by about the square root of
the real share. It ends with "RINGS REFERENCE OK" (or "RINGS REFERENCE CHECKS FAILED" when a check is out of tolerance).
"""
import argparse
import math
import sys
import time

import numpy as np

try:
    from scipy import special as _scipy_special
except ImportError:
    _scipy_special = None

RHO = 0.001                     # kg/cm^3
GRAVITY = 981.0                 # cm/s^2
TENSION = 72.8                  # sigma / rho, cm^3/s^2
LUT_UNIT_IMPULSE = 100.0        # kg*cm/s (1 N*s): the LUT's impulse
LUT_RADIUS_CM = 600.0
LUT_SECONDS = 8.0
LUT_NR = 512                    # radius texels
LUT_M = 512                     # time rows per band
BANDS_CM = (2.5, 6.0, 14.0)
DUMP_R_CM = (25.0, 50.0, 100.0, 200.0)
DUMP_T_S = (0.5, 1.0, 2.0, 4.0)
NO_WAVELENGTH = 10000.0         # the LUT's B where no wave arrives
NEGLIGIBLE_F = 1e-3             # F below this fraction of its peak counts as negligible for B (the contract leaves it open)
VISIBLE_SLOPE = 0.005
HARD_TO_SEE_SLOPE = 0.003
BREAKING_SLOPE = 0.3

USE_SCIPY = _scipy_special is not None


# ---------------------------------------------------------------------------------------------------------------- Bessel functions

def _bessel_own(n, x):
    """J0 (n = 0) or J1 (n = 1): power series up to x = 12, Hankel's asymptotic expansion beyond (24 terms; ~1e-10)."""
    x = np.asarray(x, dtype=np.float64)
    out = np.empty_like(x)
    small = np.abs(x) <= 12.0
    xs = x[small]
    if xs.size:
        h = 0.5 * xs
        term = np.ones_like(xs) if n == 0 else h.copy()
        total = term.copy()
        hh = h * h
        for m in range(1, 64):
            term = term * (-hh / (m * (m + n)))
            total += term
        out[small] = total
    xl = x[~small]
    if xl.size:
        mu = 4.0 * n * n
        inv = 1.0 / xl
        p = np.zeros_like(xl)
        q = np.zeros_like(xl)
        power = np.ones_like(xl)
        a = 1.0
        for k in range(24):
            if k:
                a *= (mu - (2 * k - 1) ** 2) / (8.0 * k)
                power = power * inv
            term = a * power
            if k % 2 == 0:
                p += term if (k // 2) % 2 == 0 else -term
            else:
                q += term if ((k - 1) // 2) % 2 == 0 else -term
        chi = xl - (0.5 * n + 0.25) * math.pi
        out[~small] = np.sqrt(2.0 / (math.pi * xl)) * (p * np.cos(chi) - q * np.sin(chi))
    return out


def j0(x):
    return _scipy_special.j0(x) if USE_SCIPY else _bessel_own(0, x)


def j1(x):
    return _scipy_special.j1(x) if USE_SCIPY else _bessel_own(1, x)


# ---------------------------------------------------------------------------------------------------------------------- dispersion

def tanh_kd(k, depth):
    return np.tanh(np.minimum(k * depth, 20.0))


def omega(k, depth):
    return np.sqrt((GRAVITY * k + TENSION * k ** 3) * tanh_kd(k, depth))


def group_speed(k, depth):
    th = tanh_kd(k, depth)
    sech2 = np.where(k * depth < 20.0, 1.0 - th * th, 0.0)
    return ((GRAVITY + 3.0 * TENSION * k * k) * th + (GRAVITY * k + TENSION * k ** 3) * depth * sech2) / (2.0 * omega(k, depth))


def band_filter(k, sigma_b, sigma_g, full=False):
    f = np.exp(-0.5 * (k * sigma_b) ** 2)
    return f if full else f - np.exp(-0.5 * (k * sigma_g) ** 2)


# ------------------------------------------------------------------------------------------------------------------- the response

class Response:
    """eta(r, t) and deta/dr(r, t) of one impulse, by the midpoint rule on k = (i + 0.5) dk, i < ceil(k_max / dk)."""

    def __init__(self, sigma_b, sigma_g, depth, nu, impulse=LUT_UNIT_IMPULSE, full=False, dk=2e-4, k_max=None):
        self.sigma_b, self.sigma_g, self.depth, self.nu, self.impulse, self.full = sigma_b, sigma_g, depth, nu, impulse, full
        self.k_max = 7.5 / sigma_b if k_max is None else k_max
        count = int(math.ceil(self.k_max / dk - 1e-9))
        self.dk = dk
        self.k = (np.arange(count) + 0.5) * dk
        self.w = omega(self.k, depth)
        self.F = band_filter(self.k, sigma_b, sigma_g, full)
        base = (impulse / RHO) * self.F * self.k * tanh_kd(self.k, depth) / self.w * dk / (2.0 * math.pi)
        self.a_eta = -base * self.k             # eta   =  sum a_eta   sin(w t) exp(-2 nu k^2 t) J0(k r)
        self.a_slope = base * self.k ** 2       # slope =  sum a_slope sin(w t) exp(-2 nu k^2 t) J1(k r)

    def _time_weights(self, t):
        return np.sin(np.outer(t, self.w)) * np.exp(-2.0 * self.nu * np.outer(t, self.k ** 2))

    def field(self, r, t, chunk=192):
        """Returns (eta [T, R] in cm, slope [T, R] in cm/cm) for radii r (cm) and times t (s)."""
        r = np.atleast_1d(np.asarray(r, dtype=np.float64))
        t = np.atleast_1d(np.asarray(t, dtype=np.float64))
        eta = np.empty((t.size, r.size))
        slope = np.empty((t.size, r.size))
        if t.size <= r.size:
            weights = self._time_weights(t)
            for r0 in range(0, r.size, chunk):
                kr = np.outer(self.k, r[r0:r0 + chunk])
                eta[:, r0:r0 + chunk] = weights @ (self.a_eta[:, None] * j0(kr))
                slope[:, r0:r0 + chunk] = weights @ (self.a_slope[:, None] * j1(kr))
        else:
            kr = np.outer(self.k, r)
            table0 = self.a_eta[:, None] * j0(kr)
            table1 = self.a_slope[:, None] * j1(kr)
            for t0 in range(0, t.size, chunk):
                weights = self._time_weights(t[t0:t0 + chunk])
                eta[t0:t0 + chunk] = weights @ table0
                slope[t0:t0 + chunk] = weights @ table1
        return eta, slope

    def dominant_wavelength(self, r, t):
        """The LUT's B channel: 2 pi / k where the group speed is r / t; of two roots the one with the larger |F k^2 / omega|;
        NO_WAVELENGTH where there is none or F there is below NEGLIGIBLE_F of its peak."""
        if t <= 0.0 or r <= 0.0:
            return NO_WAVELENGTH, None
        target = r / t
        k = np.geomspace(1e-5, 60.0, 20000)
        diff = group_speed(k, self.depth) - target
        roots = []
        for i in np.nonzero(np.sign(diff[:-1]) != np.sign(diff[1:]))[0]:
            lo, hi = k[i], k[i + 1]
            for _ in range(60):
                mid = 0.5 * (lo + hi)
                if (group_speed(np.array([mid]), self.depth)[0] - target) * diff[i] > 0.0:
                    lo = mid
                else:
                    hi = mid
            roots.append(0.5 * (lo + hi))
        if not roots:
            return NO_WAVELENGTH, None
        roots = np.array(roots)
        f = band_filter(roots, self.sigma_b, self.sigma_g, self.full)
        weight = np.abs(f * roots ** 2 / omega(roots, self.depth))
        best = roots[int(np.argmax(weight))]
        k_fine = np.linspace(1e-4, self.k_max, 20000)
        peak_f = np.max(np.abs(band_filter(k_fine, self.sigma_b, self.sigma_g, self.full)))
        if abs(band_filter(np.array([best]), self.sigma_b, self.sigma_g, self.full)[0]) < NEGLIGIBLE_F * peak_f:
            return NO_WAVELENGTH, best
        return 2.0 * math.pi / best, best


def lut_read(response, r, t):
    """What a bilinear read of a 512 x 512 LUT built from this (exact) response returns at (r, t), the shader's mapping:
    u = sqrt(r / R) on texels (i + 0.5) / Nr, time row clamp(t / T M - 0.5, 0, M - 1)."""
    u = math.sqrt(min(r, LUT_RADIUS_CM) / LUT_RADIUS_CM)
    x = min(max(u * LUT_NR - 0.5, 0.0), LUT_NR - 1.0)
    i0 = int(math.floor(x))
    i1 = min(i0 + 1, LUT_NR - 1)
    fx = x - i0
    y = min(max(t / LUT_SECONDS * LUT_M - 0.5, 0.0), LUT_M - 1.0)
    j0_ = int(math.floor(y))
    j1_ = min(j0_ + 1, LUT_M - 1)
    fy = y - j0_
    radii = [LUT_RADIUS_CM * ((i + 0.5) / LUT_NR) ** 2 for i in (i0, i1)]
    times = [LUT_SECONDS * (j + 0.5) / LUT_M for j in (j0_, j1_)]
    eta, slope = response.field(radii, times)

    def lerp(a):
        return (a[0, 0] * (1 - fx) + a[0, 1] * fx) * (1 - fy) + (a[1, 0] * (1 - fx) + a[1, 1] * fx) * fy

    texel_dr = LUT_RADIUS_CM * (((i1 + 0.5) / LUT_NR) ** 2 - ((i0 + 0.5) / LUT_NR) ** 2)
    return lerp(eta), lerp(slope), texel_dr


# ------------------------------------------------------------------------------------------------------------------------ crests

def find_crests(r, eta, slope, threshold):
    """Crests = local maxima of eta along r (the centre counts when it is a maximum). Each gets the largest |slope| between its
    neighbouring troughs; returns [(r, eta, flank slope)] for those whose flank slope is at least threshold."""
    n = eta.size
    maxima = [i for i in range(1, n - 1) if eta[i] > eta[i - 1] and eta[i] >= eta[i + 1]]
    if n > 1 and eta[0] > eta[1]:
        maxima.insert(0, 0)
    minima = [i for i in range(1, n - 1) if eta[i] < eta[i - 1] and eta[i] <= eta[i + 1]]
    crests = []
    for i in maxima:
        lower = max([m for m in minima if m < i], default=0)
        upper = min([m for m in minima if m > i], default=n - 1)
        flank = float(np.max(np.abs(slope[lower:upper + 1])))
        if flank >= threshold:
            crests.append((float(r[i]), float(eta[i]), flank))
    return crests


def spacing_near(crests, r_target):
    """The spacing of the neighbouring crests whose interval holds r_target (else the pair whose middle is nearest), with the
    pair's radii; None with fewer than two crests."""
    radii = [c[0] for c in crests]
    pairs = list(zip(radii[:-1], radii[1:]))
    if not pairs:
        return None
    holding = [p for p in pairs if p[0] <= r_target < p[1]]
    pair = holding[0] if holding else min(pairs, key=lambda p: abs(0.5 * (p[0] + p[1]) - r_target))
    return pair[1] - pair[0], pair


# --------------------------------------------------------------------------------------------------------------------- the report

def fmt(value, width=10):
    return f'{value:{width}.3e}'


def response_table(args):
    print(f'== 1. Response to {LUT_UNIT_IMPULSE:g} kg*cm/s (the LUT unit): sigma_g {args.sigma_g:g} cm, depth {args.depth:g} cm, '
          f'nu {args.nu:g} cm^2/s, dk {args.dk:g} rad/cm')
    print('   "LUT quad" = relative error of the LUT\'s own quadrature (dk pi/1200 from dk/2 to min(6/sigma_b, 3)); "LUT read" = '
          'bilinear read of an exact 512x512 LUT')
    print('   (both relative to the band\'s largest |slope| / |eta| at these points); smp/wl = radius texels per wavelength, '
          'smp/T = time rows per wave period')
    worst = {}
    for sigma_b in BANDS_CM:
        fine = Response(sigma_b, args.sigma_g, args.depth, args.nu, dk=args.dk)
        contract = Response(sigma_b, args.sigma_g, args.depth, args.nu, dk=math.pi / (2.0 * LUT_RADIUS_CM),
                            k_max=min(6.0 / sigma_b, 3.0))
        eta, slope = fine.field(DUMP_R_CM, DUMP_T_S)
        eta_c, slope_c = contract.field(DUMP_R_CM, DUMP_T_S)
        scale_s = np.max(np.abs(slope))
        scale_e = np.max(np.abs(eta))
        print(f'-- band sigma_b {sigma_b:g} cm: k_max {fine.k_max:.3f} rad/cm ({fine.k.size} k), LUT k_max {contract.k_max:.3f} '
              f'({contract.k.size} k)')
        print('    r cm  t s   deta/dr     eta cm  lambda cm  LUT quad s/e        LUT read s/e      smp/wl smp/T')
        q_s = q_e = l_s = l_e = 0.0
        for ti, t in enumerate(DUMP_T_S):
            for ri, r in enumerate(DUMP_R_CM):
                wavelength, k_dom = fine.dominant_wavelength(r, t)
                lut_eta, lut_slope, texel_dr = lut_read(fine, r, t)
                qs = abs(slope_c[ti, ri] - slope[ti, ri]) / scale_s
                qe = abs(eta_c[ti, ri] - eta[ti, ri]) / scale_e
                ls = abs(lut_slope - slope[ti, ri]) / scale_s
                le = abs(lut_eta - eta[ti, ri]) / scale_e
                q_s, q_e, l_s, l_e = max(q_s, qs), max(q_e, qe), max(l_s, ls), max(l_e, le)
                if k_dom is not None and wavelength < NO_WAVELENGTH:
                    per_wl = f'{wavelength / texel_dr:6.1f}'
                    per_t = f'{(2.0 * math.pi / omega(np.array([k_dom]), args.depth)[0]) / (LUT_SECONDS / LUT_M):5.1f}'
                else:
                    per_wl, per_t = '     -', '    -'
                print(f'  {r:6.0f} {t:4.1f} {fmt(slope[ti, ri])} {fmt(eta[ti, ri])} {wavelength:9.2f}  {qs:7.1e}/{qe:7.1e}  '
                      f'{ls:7.1e}/{le:7.1e}  {per_wl} {per_t}')
        worst[sigma_b] = (q_s, q_e, l_s, l_e)
    print()
    return worst


def checks(args):
    print('== 2. Checks')
    ok = True
    # Sign: a downward impulse opens a depression at the centre first.
    for full in (False, True):
        for sigma_b in BANDS_CM:
            response = Response(sigma_b, args.sigma_g, args.depth, args.nu, full=full, dk=args.dk)
            times = np.array([0.005, 0.02, 0.05, 0.1])
            eta, _ = response.field([0.0], times)
            first = eta[:, 0]
            good = bool(np.all(first < 0.0))
            ok &= good
            label = 'full' if full else 'band'
            print(f'   sign  {label} {sigma_b:4.1f}: eta(0, t = 0.005/0.02/0.05/0.1 s) = '
                  + ' '.join(fmt(v, 10) for v in first) + f' cm  -> {"depression first" if good else "WRONG SIGN"}')
    # Slope against a centred difference of the height.
    worst_fd = 0.0
    for sigma_b in BANDS_CM:
        response = Response(sigma_b, args.sigma_g, args.depth, args.nu, dk=args.dk)
        h = 0.01
        radii = np.array([r + s for r in DUMP_R_CM for s in (-h, 0.0, h)])
        eta, slope = response.field(radii, DUMP_T_S)
        eta = eta.reshape(len(DUMP_T_S), len(DUMP_R_CM), 3)
        slope = slope.reshape(len(DUMP_T_S), len(DUMP_R_CM), 3)[:, :, 1]
        fd = (eta[:, :, 2] - eta[:, :, 0]) / (2.0 * h)
        worst_fd = max(worst_fd, float(np.max(np.abs(fd - slope)) / np.max(np.abs(slope))))
    ok &= worst_fd < 1e-4
    print(f'   slope = d eta / dr (centred difference, h 0.01 cm), worst relative to the band\'s peak: {worst_fd:.1e}')
    # Volume: the rings displace no water.
    for sigma_b in (6.0, 2.5):
        response = Response(sigma_b, args.sigma_g, args.depth, args.nu, dk=args.dk)
        dr = 0.25
        radii = (np.arange(int(1800.0 / dr)) + 0.5) * dr
        eta, _ = response.field(radii, [1.0])
        volume = float(np.sum(eta[0] * 2.0 * math.pi * radii * dr))
        magnitude = float(np.sum(np.abs(eta[0]) * 2.0 * math.pi * radii * dr))
        ok &= abs(volume) < 1e-3 * magnitude
        print(f'   volume band {sigma_b:g} at 1 s: Integral eta 2 pi r dr = {volume:.2e} cm^3 against Integral |eta| = {magnitude:.2e}')
    # k grid convergence.
    worst_dk = 0.0
    for sigma_b in BANDS_CM:
        a = Response(sigma_b, args.sigma_g, args.depth, args.nu, dk=args.dk).field(DUMP_R_CM, DUMP_T_S)
        b = Response(sigma_b, args.sigma_g, args.depth, args.nu, dk=args.dk / 2).field(DUMP_R_CM, DUMP_T_S)
        for x, y in zip(a, b):
            worst_dk = max(worst_dk, float(np.max(np.abs(x - y)) / np.max(np.abs(y))))
    ok &= worst_dk < 1e-4
    print(f'   dk {args.dk:g} against dk/2: worst relative change {worst_dk:.1e}')
    # Bessel functions.
    if _scipy_special is not None:
        x = np.linspace(0.0, 5000.0, 400001)
        error = max(float(np.max(np.abs(_bessel_own(0, x) - _scipy_special.j0(x)))),
                    float(np.max(np.abs(_bessel_own(1, x) - _scipy_special.j1(x)))))
        print(f'   own J0/J1 (the no-scipy fallback) against scipy over 0-5000: worst {error:.1e}')
        ok &= error < 1e-8
    print(f'   Bessel functions in use: {"scipy.special" if USE_SCIPY else "own series + Hankel asymptotic"}')
    print()
    return ok


def contact_report(args, title, sigma_b, impulse, mass, speed, emitted=None, pull_after=None):
    print(f'-- {title}: band sigma_b {sigma_b:g} cm, I = {impulse:g} kg*cm/s'
          + (f' (the component would emit {emitted:g}: x{emitted / impulse:.3f})' if emitted else ''))
    times = np.arange(0.0, 6.0 + 1e-9, 0.005)
    radii = (30.0, 60.0, 100.0, 200.0)
    scale = args.impulse_scale * args.ring_strength
    if not pull_after:
        # Energy and the start: the linear field's energy is its initial kinetic energy, I^2 / (2 rho) 1/(2 pi) Integral F^2 k tanh(kd) k dk;
        # the surface under the centre starts at w0 = -(I / rho) 1/(2 pi) Integral F k tanh(kd) k dk.
        whole = Response(sigma_b, args.sigma_g, args.depth, args.nu, impulse=impulse, full=True, dk=args.dk)
        band = Response(sigma_b, args.sigma_g, args.depth, args.nu, impulse=impulse, dk=args.dk)
        energy = [impulse ** 2 / (2.0 * RHO) * float(np.sum(x.F ** 2 * x.k ** 2 * tanh_kd(x.k, args.depth))) * x.dk / (2.0 * math.pi)
                  for x in (band, whole)]
        w0 = (impulse / RHO) * float(np.sum(whole.F * whole.k ** 2 * tanh_kd(whole.k, args.depth))) * whole.dk / (2.0 * math.pi)
        kinetic = 0.5 * mass * speed ** 2
        print(f'   linear wave energy {energy[0] * 1e-4:.3g} J (band), {energy[1] * 1e-4:.3g} J (whole spectrum) against the '
              f'{mass:g} kg x {speed:g} cm/s kinetic energy {kinetic * 1e-4:.3g} J (x{energy[1] / kinetic:.2f}); the surface under the '
              f'centre starts down at {w0:.0f} cm/s (x{w0 / speed:.2f} the speed)')
    rows = {}
    for full in (False, True):
        response = Response(sigma_b, args.sigma_g, args.depth, args.nu, impulse=impulse, full=full, dk=args.dk)
        eta, slope = response.field(radii, times)
        if pull_after:
            shift = int(round(pull_after / 0.005))
            eta = eta - np.vstack([np.zeros((shift, len(radii))), eta[:-shift]])
            slope = slope - np.vstack([np.zeros((shift, len(radii))), slope[:-shift]])
        rows[full] = (eta, slope)
    print(f'   r m   ring layer: peak|eta| mm (t s)  peak|slope| (t s)  in game slope (x{scale:.4g}) eta mm  |  whole spectrum: '
          f'peak|eta| mm  peak|slope|')
    peaks = {}
    for ri, r in enumerate(radii):
        eta, slope = rows[False]
        e_i = int(np.argmax(np.abs(eta[:, ri])))
        s_i = int(np.argmax(np.abs(slope[:, ri])))
        eta_f, slope_f = rows[True]
        peaks[r] = float(abs(slope[s_i, ri]))
        print(f'   {r / 100:4.1f}  {abs(eta[e_i, ri]) * 10:10.3f} ({times[e_i]:4.2f})  {abs(slope[s_i, ri]):10.4f} ({times[s_i]:4.2f})'
              f'  {abs(slope[s_i, ri]) * scale:13.4f} {abs(eta[e_i, ri]) * 10 * scale:9.3f}  |  {np.max(np.abs(eta_f[:, ri])) * 10:10.3f}'
              f'    {np.max(np.abs(slope_f[:, ri])):10.4f}')
    # Crests within 1.5 m.
    dr = 0.2
    profile_r = np.arange(0.0, 150.0 + 1e-9, dr)
    print(f'   crests within 1.5 m (dr {dr:g} cm; visible = flank |slope| > {VISIBLE_SLOPE:g}; all = above 1e-4 of the largest); '
          f'spacing of the crest pair around 0.5 m; lambda = stationary phase at 0.5 m')
    print('   t s  ring: visible phys / in game / all  spacing cm (pair)    lambda cm  |  whole: visible phys / in game  spacing cm')
    listing = []
    for t in (1.0, 2.0, 3.0):
        line = f'   {t:3.1f}'
        for full in (False, True):
            response = Response(sigma_b, args.sigma_g, args.depth, args.nu, impulse=impulse, full=full, dk=args.dk)
            sample_times = [t] if not pull_after else [t, t - pull_after]
            eta, slope = response.field(profile_r, sample_times)
            e, s = eta[0], slope[0]
            if pull_after:
                e, s = e - eta[1], s - slope[1]
            visible = find_crests(profile_r, e, s, VISIBLE_SLOPE)
            visible_game = find_crests(profile_r, e, s, VISIBLE_SLOPE / scale)
            every = find_crests(profile_r, e, s, 1e-4 * float(np.max(np.abs(s))))
            spacing = spacing_near(every, 50.0)
            if not full:
                spacing_text = f'{spacing[0]:6.1f} ({spacing[1][0]:5.1f}-{spacing[1][1]:5.1f})' if spacing else '     -              '
                wavelength, _ = response.dominant_wavelength(50.0, t)
                wl_text = f'{wavelength:9.1f}' if wavelength < NO_WAVELENGTH else '     none'
                line += f'        {len(visible):3d}  /  {len(visible_game):3d}  / {len(every):3d}  {spacing_text}  {wl_text}  |'
                listing.append(f'   t {t:g} s: ' + '; '.join(
                    f'{c[0]:.1f} cm {c[1] * 10:.2f} mm s {c[2]:.3g} B {response.dominant_wavelength(c[0], t)[0]:.0f}' for c in every))
            else:
                spacing_text = f'{spacing[0]:6.1f}' if spacing else '     -'
                line += f'         {len(visible):3d}  /  {len(visible_game):3d}         {spacing_text}'
        print(line)
    print('   ring crests (r, physical height, flank slope, LUT B at the crest):')
    for line in listing:
        print(line)
    print()
    return peaks


def contacts(args):
    print(f'== 3. Example contacts (sigma_g {args.sigma_g:g} cm, depth {args.depth:g} cm, nu {args.nu:g}); "in game" = x Wave Impulse '
          f'Scale {args.impulse_scale:g} x Wave Ring Strength {args.ring_strength:g}')
    k = np.geomspace(1e-3, 50.0, 200000)
    speeds = group_speed(k, args.depth)
    slowest = int(np.argmin(speeds))
    print(f'   slowest group speed {speeds[slowest]:.2f} cm/s at a {2.0 * math.pi / k[slowest]:.2f} cm wavelength: inside r = '
          f'{speeds[slowest]:.2f} t no stationary point exists (the LUT\'s B there is {NO_WAVELENGTH:g}), yet the rings\' slow tail is there')
    plunge = contact_report(args, 'plunge, 3.5 kg x 1250 cm/s', 6.0, 3.5 * 1250.0, 3.5, 1250.0, emitted=2.5 * 1250.0)
    contact_report(args, 'plunge as an entry push and an equal pull 0.25 s later (a momentum model that releases the added mass '
                   'while decelerating)', 6.0, 3.5 * 1250.0, 3.5, 1250.0, pull_after=0.25)
    dip = contact_report(args, 'head dip, 0.3 kg x 150 cm/s', 2.5, 0.3 * 150.0, 0.3, 150.0)
    scale = args.impulse_scale
    print(f'-- Wave Ring Strength window at Wave Impulse Scale {scale:g} (in game slope = physical x Wave Impulse Scale x Wave Ring '
          f'Strength):')
    for name, peaks in (('plunge', plunge), ('head dip', dip)):
        low = HARD_TO_SEE_SLOPE / (scale * peaks[60.0])
        low_far = HARD_TO_SEE_SLOPE / (scale * peaks[100.0])
        high = BREAKING_SLOPE / (scale * peaks[30.0])
        print(f'   {name:9s}: peak slope {peaks[60.0]:.4f} at 0.6 m, {peaks[100.0]:.4f} at 1 m, {peaks[30.0]:.4f} at 0.3 m (physical): '
              f'visible ({HARD_TO_SEE_SLOPE:g}) at 0.6 m from strength {low:.3g}, at 1 m from {low_far:.3g}; '
              f'breaks ({BREAKING_SLOPE:g}) at 0.3 m above {high:.3g}')
    print()


def main():
    global USE_SCIPY
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sigma-g', type=float, default=23.44, help='grid splat sigma, cm (2 wave cells; default 2 x 11.72)')
    parser.add_argument('--depth', type=float, default=335.0, help='simulation depth, cm (default 335)')
    parser.add_argument('--nu', type=float, default=0.05, help='ring viscosity, cm^2/s (default 0.05)')
    parser.add_argument('--dk', type=float, default=2e-4, help='k step of the reference quadrature, rad/cm (default 2e-4)')
    parser.add_argument('--impulse-scale', type=float, default=0.35, help='Wave Impulse Scale for the in-game columns (default 0.35)')
    parser.add_argument('--ring-strength', type=float, default=1.0, help='Wave Ring Strength for the in-game columns (default 1)')
    parser.add_argument('--own-bessel', action='store_true', help='use this file\'s J0/J1 even when scipy is installed')
    parser.add_argument('--skip', default='', help='comma list of sections to skip: table, checks, contacts')
    args = parser.parse_args()
    if args.own_bessel:
        USE_SCIPY = False
    skip = {s.strip() for s in args.skip.split(',') if s.strip()}
    start = time.perf_counter()
    print(f'numpy {np.__version__}, scipy {"missing" if _scipy_special is None else "present"}')
    print()
    if 'table' not in skip:
        response_table(args)
    ok = True
    if 'checks' not in skip:
        ok = checks(args)
    if 'contacts' not in skip:
        contacts(args)
    print(f'{time.perf_counter() - start:.1f} s')
    print('RINGS REFERENCE OK' if ok else 'RINGS REFERENCE CHECKS FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
