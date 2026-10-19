// FluidNinja Waves - stage "Wave Step" (iterates Emitter.WaveGrid, after Wave Prepare). Linear Airy waves with a surface-pressure source,
// symplectic Euler at the fixed tick:
//   v'   = (v - dt g L(s) - dt (Damping + sponge) v - dt nu4 T0(T0(v))) exp(-dt (shore + breaking)) m,   eta' = (eta + dt v') m
//   eta' = (eta' + P) exp(-dt sponge) - P                                                                   (the open-edge sponge)
//   v'  -= dt VMeanRelax <v> m,   eta' -= dt^2 VMeanRelax <v> m                    (a walled window only; interface version 5, below)
//   eta' -= dt MeanRelax <s> m                                                                (the k = 0 mode; both rates clamped, below)
// L = (1/dx) sum_l w_l(h/dx) T_l, T_0 = s - B3[s m]/B3[m] (masked 3x3 binomial on mip 0), T_l = s - G_l[s m]/G_l[m] (B-spline read of WaveOp
// mip l, 4 bilinear fetches, UV clamped per mip because the render target wraps); w_l from the P0 minimax table (12 depth rows x 9 levels,
// level 6 is zero everywhere). The erase flag sets eta = -P, v = 0.
// Breaking judges eta (mip 0 .w = eta m) across source-free cells only, never s = eta + P + P_dyn: a hull's static-head edge and a one-tick
// impulse are sources, not waves. The k = 0 modes (a uniform level, invisible to L and to the damping of v, and a uniform v, invisible to
// L) have three sinks: the open-edge sponge relaxes eta + P toward 0 at the rate it damps v with, a walled window's mean v over wet cells
// (.z / .y of WaveOp's top, 1 x 1, mip) is relaxed at Wave.VMeanRelax, and the window's mean s (.x / .y) at Wave.MeanRelax (1/s each; 0
// off). FluidNinja Buoyancy's self-field step (SelfStepCS) ports this stage line for line, except interface version 5's <v> pin and the
// in-step clamp of the two rates (below).
// Interface version 3: the grid is the pressure buffer's cells over Emitter.Wave.ScaleDown (S), so this stage runs on 1 / S^2 of the cells
// and its cell dx = Extents / Cells is S pressure cells: the dispersion weights, the sponge and wall bands in cm and the hyperviscosity's
// finest wavelength all follow the wave cell. The depth is the mean over each wave cell's S x S block of pressure cells (_depth.hlsl).
// At S = 1 everything is what interface 2 did.
// Interface version 4: the foam F (WaveGrid attribute Foam, 0..1), a separate field nothing feeds back into (eta, v): the foam
// Wave Prepare carried (the foam target's scratch) plus what breaks and what hits this tick, decayed at Wave.FoamDecay and clamped to 1.
//   (a) breaking: the breaking decay (1/s, the part of Decay that is the slope and crest tests) x |v| / 100 cm/s x Wave.FoamBreaking:
//       the energy the breaking takes out of the wave turns white;
//   (b) waterline shear: |grad(P + P_dyn)| (clamped to 1: a hull's side) x the hull's speed through the water at the cell (the
//       source's A, cm/s, written by the plugin) / 100 cm/s x Wave.FoamWake: a moving hull shears foam off its waterline, a resting
//       one does not, and the trail is what the hull leaves behind;
//   (c) impacts: |y| / 10 cm s x Wave.FoamImpact over the frame's ticks (a 175 kg crate from 3 m over a square metre is about 13 cm s):
//       the slam and drag impulses, Add Water Impulse, the pokes and the swimmers' parts alike.
// The erase flag clears it. Wave Aux exports it. The cost is one float attribute on the grid, one load and one write here.
// Interface version 5: the closed-window <v> pin. Under a current, breaking at the floating hulls' hollow edges damps the refilling v on one
// side only, which pumps the uniform vertical velocity <v>; nothing restored it, and the s pin alone parked the level at <v> / MeanRelax,
// so a walled window's level sank for minutes. Wave.VMeanRelax (1/s; 0 off, the asset default) relaxes a walled window's (Wave.EdgeOpen <
// 0.5) mean v over wet cells toward 0 and takes the same constant's step out of eta, so the integrator is gone and the s pin can be gentle.
// Both mean-relax rates are clamped here against the Dt this stage integrates with: Wave.MeanRelax to 0.5 / Dt, Wave.VMeanRelax to
// 1 / Dt (a core without a fixed tick can step Niagara's 0.125 s), each only past a 0.01 % margin. At VMeanRelax 0 and MeanRelax up to
// 0.5 / Dt, equality included, (eta, v) is what interface 4 computes, bit for bit, so a plugin that sends no VMeanRelax gets exactly the
// old step, also at a MeanRelax it clamped to 0.5 / its tick itself. The cost is one more parameter and, in a walled window, a few ALU per
// cell: the top-mip texel is loaded once for both pins.
int IX;
int IY;
WaveGrid.ExecutionIndexToGridIndex(IX, IY);
int NX;
int NY;
WaveGrid.GetNumCells(NX, NY);
float2 Cells = float2(NX, NY);

// ---- mip 0 (s m, m, v, eta) on the 5 x 5 around the cell; loads outside the target return 0 (dry)
float4 M[5][5];
for (int Lj = 0; Lj < 5; ++Lj)
{
	for (int Li = 0; Li < 5; ++Li)
	{
		WaveOp.LoadRenderTargetValue(IX + Li - 2, IY + Lj - 2, 0, M[Lj][Li]);
	}
}
float4 C = M[2][2];
float Wet = C.y;
float S = C.x;
float VAdv = C.z;
float EtaAdv = C.w;
float BW[3] = {0.25, 0.5, 0.25};

// ---- level 0: masked binomial, and the biharmonic hyperviscosity T0(T0(v))
float Num0 = 0.0;
float Den0 = 0.0;
float T0v[3][3];
for (int Kj = 0; Kj < 3; ++Kj)
{
	for (int Ki = 0; Ki < 3; ++Ki)
	{
		float4 Q = M[Kj + 1][Ki + 1];
		float Bk = BW[Ki] * BW[Kj];
		Num0 += Bk * Q.x;
		Den0 += Bk * Q.y;
		float NumV = 0.0;
		float DenV = 0.0;
		for (int Qj = 0; Qj < 3; ++Qj)
		{
			for (int Qi = 0; Qi < 3; ++Qi)
			{
				float4 R = M[Kj + Qj][Ki + Qi];
				float Bq = BW[Qi] * BW[Qj];
				NumV += Bq * R.y * R.z;
				DenV += Bq * R.y;
			}
		}
		T0v[Kj][Ki] = DenV > 0.0 ? Q.z - NumV / DenV : 0.0;
	}
}
float T0 = Den0 > 0.0 ? S - Num0 / Den0 : 0.0;
float NumH = 0.0;
float DenH = 0.0;
for (int Hj = 0; Hj < 3; ++Hj)
{
	for (int Hi = 0; Hi < 3; ++Hi)
	{
		float Mk = M[Hj + 1][Hi + 1].y;
		float Bk = BW[Hi] * BW[Hj];
		NumH += Bk * Mk * T0v[Hj][Hi];
		DenH += Bk * Mk;
	}
}
float HyperV = DenH > 0.0 ? T0v[1][1] - NumH / DenH : 0.0;

// ---- cell size (the wave cell: S pressure cells) and the cell's unit position in the window (the grid lags the window by the scroll
// remainder), then the depth (_depth.hlsl: the block mean of the bed the height field gives, else Wave.DepthCm)
float2 CellCm = Extents / Cells;
float Dx = 0.5 * (CellCm.x + CellCm.y);
float2 U = (float2(IX, IY) + 0.5 - Remainder) / Cells;
float3 SrcSim = mul(float4(U, 0.5, 1.0), UnitToWorld).xyz;
float2 DepthShift = Remainder;
//@include _depth.hlsl

// ---- dispersion weights (P0 table; rows H_j = 1.28 * 1.683891^j cells, Bo = (sigma/rho) / (g dx^2))
static const float WG[12][9] = {
	{ 2.652007, 0.2231427, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 2.139424, 0.5073167, 0.01866169, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 2.076839, 0.3952435, 0.1602098, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 2.169085, 0.2620277, 0.2055651, 0.04232308, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 2.115532, 0.32208, 0.09967527, 0.1033419, 0.01101243, 0.0, 0.0, 0.0, 0.0 },
	{ 2.11266, 0.330048, 0.08598847, 0.07846895, 0.03881073, 0.004212264, 0.0, 0.0, 0.0 },
	{ 2.112846, 0.3295296, 0.08799041, 0.06950843, 0.02918338, 0.02122492, 0.0, 0.0, 0.0 },
	{ 2.112486, 0.3305339, 0.08411518, 0.07931206, 0.01001729, 0.03198638, 0.0, 0.0, 0.00164963 },
	{ 2.112476, 0.3305616, 0.08400983, 0.07963947, 0.009197677, 0.02809094, 0.0, 0.006120074, 0.0 },
	{ 2.112991, 0.3291329, 0.08587961, 0.07864184, 0.00983242, 0.02616774, 0.0, 0.007710436, 0.0 },
	{ 2.109872, 0.3377785, 0.07471284, 0.08403116, 0.008085577, 0.02663098, 0.0, 0.007667287, 0.0 },
	{ 2.109872, 0.3377785, 0.07471284, 0.08403116, 0.008085577, 0.02663098, 0.0, 0.007667287, 0.0 } };
static const float WC[12][9] = {
	{ 0.5048955, 18.69172, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 4.901266, 0.0, 0.07636693, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.384365, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.899802, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.920234, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.920234, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.920234, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.920235, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.920239, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.920244, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.920241, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 },
	{ 7.920241, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } };
float RowU = clamp(log(max(Depth / Dx, 1e-6) / 1.28) * 1.91899, 0.0, 11.0);
int Row = min(int(RowU), 10);
float RowA = RowU - float(Row);
float Bo = 72.8 / (981.0 * Dx * Dx);

float L = lerp(WG[Row][0] + Bo * WC[Row][0], WG[Row + 1][0] + Bo * WC[Row + 1][0], RowA) * T0;
float2 UV = (float2(IX, IY) + 0.5) / Cells;
for (int Lv = 1; Lv < 9; ++Lv)
{
	float Wl = lerp(WG[Row][Lv] + Bo * WC[Row][Lv], WG[Row + 1][Lv] + Bo * WC[Row + 1][Lv], RowA);
	if (Wl > 0.0)
	{
		float2 Nl = float2(max(NX >> Lv, 1), max(NY >> Lv, 1));
		float2 Lo = 0.5 / Nl;
		float2 Hi = 1.0 - Lo;
		float2 Tt = clamp(UV, Lo, Hi) * Nl - 0.5;
		float2 I0 = floor(Tt);
		float2 F = Tt - I0;
		float2 F2 = F * F;
		float2 F3 = F2 * F;
		float2 B0 = (1.0 - F) * (1.0 - F) * (1.0 - F) / 6.0;
		float2 B1 = (4.0 - 6.0 * F2 + 3.0 * F3) / 6.0;
		float2 B2 = (1.0 + 3.0 * F + 3.0 * F2 - 3.0 * F3) / 6.0;
		float2 B3 = F3 / 6.0;
		float2 G0 = B0 + B1;
		float2 G1 = B2 + B3;
		float2 P0 = clamp((I0 - 1.0 + B1 / G0 + 0.5) / Nl, Lo, Hi);
		float2 P1 = clamp((I0 + 1.0 + B3 / G1 + 0.5) / Nl, Lo, Hi);
		float4 V00;
		float4 V10;
		float4 V01;
		float4 V11;
		WaveOp.SampleRenderTargetValue(float2(P0.x, P0.y), float(Lv), V00);
		WaveOp.SampleRenderTargetValue(float2(P1.x, P0.y), float(Lv), V10);
		WaveOp.SampleRenderTargetValue(float2(P0.x, P1.y), float(Lv), V01);
		WaveOp.SampleRenderTargetValue(float2(P1.x, P1.y), float(Lv), V11);
		float4 G = G0.x * G0.y * V00 + G1.x * G0.y * V10 + G0.x * G1.y * V01 + G1.x * G1.y * V11;
		float Tl = G.y >= 1e-3 ? S - G.x / G.y : 0.0;
		L += Wl * Tl;
	}
}
L /= Dx;

// ---- damping: Wave.Damping and the open-edge sponge (300 cm, 8/s at the border, quadratic) as in P0, on v here and on eta below, or,
// where the border is a wall, Wave.WallAbsorption (1/s) within Wave.WallBandCm of it (a pool's gutter: a wall gives back most of a wave, not
// all); shore (8/s at zero depth, linear to Wave.ShoreDepthCm) and breaking (20/s; slope of eta past Wave.BreakingSlope, or eta above 0.6
// depth) as an exact decay.
float Gamma = Damping;
float SpongeEta = 0.0;
if (EdgeOpen >= 0.5 && SpongeCm > 0.0)
{
	float EdgeCm = min(min(float(IX) + 0.5, float(NX - IX) - 0.5) * CellCm.x, min(float(IY) + 0.5, float(NY - IY) - 0.5) * CellCm.y);
	float Sponge = saturate(1.0 - EdgeCm / SpongeCm);
	Gamma += 8.0 * Sponge * Sponge;
	SpongeEta = 8.0 * Sponge * Sponge;
}
if (EdgeOpen < 0.5 && WallAbsorption > 0.0 && WallBandCm > 0.0)
{
	float WallCm = min(min(float(IX) + 0.5, float(NX - IX) - 0.5) * CellCm.x, min(float(IY) + 0.5, float(NY - IY) - 0.5) * CellCm.y);
	Gamma += WallCm < WallBandCm ? WallAbsorption : 0.0;
}
float Decay = ShoreDepthCm > 0.0 ? 8.0 * saturate(1.0 - Depth / ShoreDepthCm) : 0.0;
// Breaking is judged on the wave, eta (mip 0 .w = eta m), not on s = eta + P + P_dyn (.x). Judged on s, a moving hull's static-head edge
// (P steps up a cell or two before eta follows) and every one-tick impulse spike (P_dyn) broke on themselves: the Kelvin wake came out at
// 0.56 of theory and each impulse was damped by exp(-20 dt) in its own tick. P_dyn still drives v through L(s) above. Under a hull eta = -P
// (its hollow), so a hull side steeper than Wave.BreakingSlope is judged too: at rest v is 0 there and the decay does nothing.
// A cell under a source (s != eta: a static head or this tick's impulse) is left out of the judgement as well: under a hull eta is its
// hollow, so a hull side would read as a steep wave and damp the water at its own waterline for good. The slope is taken only across
// pairs of wet, source-free cells (a neighbour that carries a source counts as dry here).
float4 East = M[2][3];
float4 West = M[2][1];
float4 North = M[3][2];
float4 South = M[1][2];
bool CentreFree = abs(C.x - C.w) <= 1e-3;
bool EastFree = East.y > 0.5 && abs(East.x - East.w) <= 1e-3;
bool WestFree = West.y > 0.5 && abs(West.x - West.w) <= 1e-3;
bool NorthFree = North.y > 0.5 && abs(North.x - North.w) <= 1e-3;
bool SouthFree = South.y > 0.5 && abs(South.x - South.w) <= 1e-3;
float SlopeX = !CentreFree ? 0.0 : ((EastFree && WestFree) ? 0.5 * (East.w - West.w) : (EastFree ? East.w - EtaAdv : (WestFree ? EtaAdv - West.w : 0.0)));
float SlopeY = !CentreFree ? 0.0 : ((NorthFree && SouthFree) ? 0.5 * (North.w - South.w) : (NorthFree ? North.w - EtaAdv : (SouthFree ? EtaAdv - South.w : 0.0)));
float Slope = length(float2(SlopeX, SlopeY)) / Dx;
// FoamBreak is the breaking part of Decay alone (the same numbers, summed apart so Decay's arithmetic stays exactly as it was), for (a).
float FoamBreak = 0.0;
if (BreakingSlope > 0.0 && Slope > BreakingSlope)
{
	Decay += 20.0 * (1.0 - BreakingSlope / Slope);
	FoamBreak += 20.0 * (1.0 - BreakingSlope / Slope);
}
if (EtaAdv > 0.6 * Depth)
{
	Decay += 20.0;
	FoamBreak += 20.0;
}

float Dt = DeltaTime;
float VNew = (VAdv - Dt * 981.0 * L - Dt * Gamma * VAdv - Dt * Hyperviscosity * HyperV) * exp(-Dt * Decay) * Wet;
float EtaNew = (EtaAdv + Dt * VNew) * Wet;

// ---- the k = 0 modes: a uniform level and a uniform vertical velocity are both invisible to L, and the damping of v reaches the second
// only at Damping plus the wall band. Three sinks, in this order. The open-edge sponge relaxes eta + P toward 0 at the 8 Sponge^2 /s it
// damps v with, as an exact decay (stable at any dt; damping eta and v alike leaves a wave's shape alone, so the band stays
// reflection-free). Then, below, a walled window's mean v (interface version 5) and the window's mean s, both explicit terms on the means
// WaveOp's top mip gives.
//@include _source.hlsl
// The band relaxes the wave over a hull, eta + P, not the hollow itself: a hull parked in the band keeps its draft.
EtaNew = (EtaNew + SrcP * Wet) * exp(-Dt * SpongeEta) - SrcP * Wet;
// ---- the means (interface version 5). WaveOp's top mip is the 1 x 1 level FloorLog2(max(NX, NY)) of the Blur2 chain the RT2D DI builds;
// its texel averages Wave Prepare's (s m, m, v m, eta m) over the window, so <s> = .x / .y and <v> = .z / .y over the wet cells. The
// chain is built PostStage after Wave Prepare, so these are THIS tick's means, not last tick's. Taking a constant off v or eta in every
// wet cell changes no L (T_l of a constant is 0). It is read once, for both pins.
// A uniform v decays only at Damping plus the wall band (about 0.06 /s on an 800 x 800 pool), so a one-signed pump on Sigma v (breaking at
// a hull hollow's step edge under a current, v advected through a divergent drift) integrated into a growing <v>, and the s pin below,
// which acts on eta only, then parked the level at <v> / MeanRelax: a sink that went on for minutes (-40 to -67 cm in the bench). In a
// walled window Sigma_wet v = d/dt Sigma_wet eta = 0 is exact volume conservation, so a <v> is no wave but a pump's residue, and relaxing it
// at Wave.VMeanRelax (1/s, 0 off) restores a true constraint. With this tick's mean the term is a plain projection for any rate up to
// 1 / Dt (Dt RateV = 1 removes <v> in one tick), so that is its clamp. Open windows (a river, the sponge) are left alone: their Sigma v has
// real edge fluxes. The self-field port has no <v> pin either: its tiles are open windows.
// Both rates are clamped here against the Dt this stage actually integrates with: a core without a fixed tick can step Niagara's 0.125 s,
// and a rate the plugin chose for 1/60 s would then overshoot. Each clamp acts only when Dt times the rate passes its limit by more than
// 0.01 %, and otherwise the rate is used exactly as sent. The plugin clamps its own rate to 0.5f / its tick on the CPU (29.999998 /s at
// 1/60 s), and a plain min() against the GPU's 0.5 / Dt, a division D3D lets round by up to 2.5 ULP, could swap that for a value one ULP
// off. With the margin, at VMeanRelax 0 and MeanRelax up to 0.5 / Dt the arithmetic is interface 4's, bit for bit, and Dt r stays within
// 0.5001 (1.0001 for v), still far inside the explicit terms' stable range.
float4 Top = float4(0, 0, 0, 0);
if (MeanRelax > 0.0 || VMeanRelax > 0.0)
{
	WaveOp.LoadRenderTargetValue(0, 0, int(firstbithigh(uint(max(max(NX, NY), 1)))), Top);
}
if (VMeanRelax > 0.0 && EdgeOpen < 0.5 && Top.y > 1e-6)
{
	float MeanV = Top.z / Top.y;
	float RateV = VMeanRelax * Dt > 1.0001 ? 1.0 / max(Dt, 1e-4) : VMeanRelax;
	VNew -= Dt * RateV * MeanV * Wet;
	EtaNew -= Dt * Dt * RateV * MeanV * Wet;   // EtaNew = EtaAdv + Dt VNew was formed above with the unpinned VNew
}
if (MeanRelax > 0.0 && Top.y > 1e-6)
{
	// The mean of s = eta + P (.x), not of eta: under a hull s is about 0, so a body's hollow adds no bias, while the drawn level <B> = <s>
	// is exactly what drifts. Archimedes' rise in a basin (the free surface at +Sigma P / wet area) is relaxed too. The self-field port
	// relaxes the same quantity, so it stays free of a bias from its own hollow. Clamped to 0.5 / Dt: an explicit term on the mean is
	// monotone below 1 / Dt, and at half that it stays damped even if the mean it reads were a tick late (at Dt r = 1 that rings undamped).
	float MeanS = Top.x / Top.y;
	float RateS = MeanRelax * Dt > 0.5001 ? 0.5 / max(Dt, 1e-4) : MeanRelax;
	EtaNew -= Dt * RateS * MeanS * Wet;
}

// ---- foam (interface version 4): after the physics, which it never feeds. 0 while the foam is off, so the grid's Foam is always defined.
float FoamNew = 0.0;
if (FoamEnabled)
{
	float4 FoamIn;
	WaveFoam.LoadRenderTargetValue(IX, IY, 0, FoamIn);
	float Foam = FoamIn.x;
	// (a) what breaks: the breaking decay times the water's speed.
	Foam += Dt * FoamBreaking * FoamBreak * abs(VAdv) * 0.01;
	// (b) the waterline: the static head's slope (x - w = P + P_dyn in each wet cell of the 5 x 5; a dry one reads 0) times the hull's speed
	// through the water there. Inside a flat hull the slope is 0; at its side it is the draft over a cell, clamped to 1.
	float2 GradP = float2((East.x - East.w) - (West.x - West.w), (North.x - North.w) - (South.x - South.w)) * (0.5 / Dx);
	Foam += Dt * FoamWake * saturate(length(GradP)) * SrcSpeed * 0.01;
	// (c) what hits: this tick's share of the frame's impulse head.
	Foam += FoamImpact * abs(SrcY) / (max(float(NumTimeSteps), 1.0) * 10.0);
	FoamNew = min(Foam * exp(-Dt * FoamDecay), 1.0) * Wet;
}

if (SrcErase > 0.5)
{
	VNew = 0.0;
	EtaNew = -SrcP * Wet;
	FoamNew = 0.0;
}
WaveGrid.SetVector2DValue<Attribute=Wave>(IX, IY, float2(EtaNew, VNew));
WaveGrid.SetFloatValue<Attribute=Foam>(IX, IY, FoamNew);
Done = Wet;
