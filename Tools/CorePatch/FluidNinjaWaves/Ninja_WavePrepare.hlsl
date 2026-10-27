// FluidNinja Waves - stage "Wave Prepare" (iterates Emitter.WaveOp, after Pressure). Per wave cell: the previous (eta, v) at the whole-cell
// scroll, advected by FluidNinja's current averaged over a few cells (_drift.hlsl; 16-tap Catmull-Rom, an exact load below 0.01 cell), the wave
// source (static head P, impulse y, erase), the obstacle mask m from FluidNinja's own masks, and WaveOp mip 0 = (s m, m, v, eta) with
// s = eta + P + P_dyn.
// Interface version 3: the wave grid is the pressure buffer's cells over Emitter.Wave.ScaleDown (S), so this stage runs on 1 / S^2 of the
// cells. The mask is judged over the S x S block of pressure cells a wave cell covers (_mask.hlsl: wet when more than half the block is);
// the current is still read at the wave cell's centre, averaged over a few wave cells (its averaging footprint grows with the grid, as the
// grid-scale eddies it guards against do); the source texture is at the wave grid's cells. At S = 1 everything is what interface 2 did.
// Interface version 4: the foam F (WaveGrid attribute Foam, Ninja_WaveStep) is carried by the same back-trace as (eta, v), bilinearly
// (foam is a diffuse quantity: the bilinear read never overshoots and costs one fetch), and handed to Wave Step through the foam
// target (Emitter.WaveFoam, the plugin's) as this tick's scratch; Wave Aux overwrites it with the final F. Nothing of it touches
// (eta, v).
// Interface version 6: the aged foam F2 (WaveGrid attribute FoamAged) is carried exactly as F (now F1, the fresh foam): the same
// back-trace, the same exact load or bilinear read. The scratch is (F1 m, hi(F2 m), lo(F2 m) x FoamAgedLoScale, 0): F2 m split into its
// nearest half float hi and the remainder lo = F2 m - hi, stored scaled by FoamAgedLoScale (2048, _foamaged.hlsl), which Wave Step undoes
// as it adds it back (.y + .z / 2048). This deliberately departs from a plain (F1 m, F2 m, 0, 0): the plugin's foam target is a 16-bit
// float target, and F2 decays slowly (Wave.FoamAgedDecay 0.02 /s is 3.3e-4 of it per 60 Hz tick, below half a half-float ulp in the lower
// part of every binade), so a single 16-bit channel rounds F2 back to the same value every tick and the aged foam never fades (0.6 stays
// 0.6 for 50 s instead of falling to 0.22). hi is a half float itself, so the target stores it exactly whatever its rounding; lo is at most
// 2^-11 of F2 and keeps the rest to about 2^-22. Unscaled, lo is often a half-float denormal, which a GPU that flushes them stores or
// loads as 0, and F2 stalls again (0.2 stays 0.183 over 50 s); times 2048 it is a normal half float (_foamaged.hlsl says why and how far).
// F1 stays one 16-bit channel, exactly as interface 5 hands it over. The scratch is the core's own: Wave Aux overwrites the target with
// (F1, F2, v, 0) every tick. On an RG target lo is dropped (F2 at 16-bit precision), on an R target F2's scratch reads 0; a missing .z
// reads 0, never 1 as a missing .w would.
int IX;
int IY;
WaveOp.ExecToIndex(IX, IY);
int NX;
int NY;
WaveOp.GetRenderTargetSize(NX, NY);
float2 Cells = float2(NX, NY);
// This cell's unit position in the sim window: the grid lags the window by the scroll remainder.
float2 U = (float2(IX, IY) + 0.5 - Remainder) / Cells;
float3 SrcSim = mul(float4(U, 0.5, 1.0), UnitToWorld).xyz;

float2 MaskShift = Remainder;
//@include _mask.hlsl

// ---- previous state: whole-cell scroll, then semi-Lagrangian advection by the current (unit/tick x cells, averaged around the cell)
//@include _drift.hlsl
float2 State = float2(0.0, 0.0);
if (dot(Drift, Drift) <= 1e-4)
{
	int2 Q = int2(IX, IY) + int2(Scroll);
	if (Q.x >= 0 && Q.y >= 0 && Q.x < NX && Q.y < NY)
	{
		WaveGrid.GetPreviousVector2DValue<Attribute=Wave>(Q.x, Q.y, State);
	}
}
else
{
	float2 CrPos = float2(IX, IY) + Scroll - Drift;
//@include _catmullrom.hlsl
	State = CrValue;
}
float Eta = State.x;
float V = State.y;

// ---- foam: the previous F1 and F2 at the same back-traced position (outside the grid 0, as eta), into the foam target for Wave Step
//@include _foamaged.hlsl
if (FoamEnabled)
{
	float FoamAdv = 0.0;
	float FoamAgedAdv = 0.0;
	float2 FoamPos = float2(IX, IY) + Scroll - Drift;
	if (all(FoamPos >= -0.5) && all(FoamPos <= Cells - 0.5))
	{
		if (dot(Drift, Drift) <= 1e-4)
		{
			int2 FoamQ = int2(IX, IY) + int2(Scroll);
			if (FoamQ.x >= 0 && FoamQ.y >= 0 && FoamQ.x < NX && FoamQ.y < NY)
			{
				WaveGrid.GetPreviousFloatValue<Attribute=Foam>(FoamQ.x, FoamQ.y, FoamAdv);
				WaveGrid.GetPreviousFloatValue<Attribute=FoamAged>(FoamQ.x, FoamQ.y, FoamAgedAdv);
			}
		}
		else
		{
			WaveGrid.SamplePreviousGridFloatValue<Attribute=Foam>((FoamPos + 0.5) / Cells, FoamAdv);
			WaveGrid.SamplePreviousGridFloatValue<Attribute=FoamAged>((FoamPos + 0.5) / Cells, FoamAgedAdv);
		}
	}
	float FoamAgedOut = FoamAgedAdv * Wet;
	float FoamAgedHi = f16tof32(f32tof16(FoamAgedOut));
	// lo stored times FoamAgedLoScale, so it is a normal half float, not a denormal a GPU may flush (_foamaged.hlsl); Wave Step undoes it.
	WaveFoam.SetRenderTargetValue(true, IX, IY, float4(FoamAdv * Wet, FoamAgedHi, (FoamAgedOut - FoamAgedHi) * FoamAgedLoScale, 0.0));
}

//@include _source.hlsl

float Steps = max(float(NumTimeSteps), 1.0);
float PDyn = DeltaTime > 0.0 ? SrcY / (Steps * DeltaTime) : 0.0;
if (SrcErase > 0.5)
{
	Eta = -SrcP;
	V = 0.0;
}
float S = Eta + SrcP + PDyn;
WaveOp.SetRenderTargetValue(true, IX, IY, float4(S * Wet, Wet, V * Wet, Eta * Wet));
Done = Wet;
