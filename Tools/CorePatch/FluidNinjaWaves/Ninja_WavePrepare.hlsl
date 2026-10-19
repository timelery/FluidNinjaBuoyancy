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

// ---- foam: the previous F at the same back-traced position (outside the grid 0, as eta), into the foam target for Wave Step
if (FoamEnabled)
{
	float FoamAdv = 0.0;
	float2 FoamPos = float2(IX, IY) + Scroll - Drift;
	if (all(FoamPos >= -0.5) && all(FoamPos <= Cells - 0.5))
	{
		if (dot(Drift, Drift) <= 1e-4)
		{
			int2 FoamQ = int2(IX, IY) + int2(Scroll);
			if (FoamQ.x >= 0 && FoamQ.y >= 0 && FoamQ.x < NX && FoamQ.y < NY)
			{
				WaveGrid.GetPreviousFloatValue<Attribute=Foam>(FoamQ.x, FoamQ.y, FoamAdv);
			}
		}
		else
		{
			WaveGrid.SamplePreviousGridFloatValue<Attribute=Foam>((FoamPos + 0.5) / Cells, FoamAdv);
		}
	}
	WaveFoam.SetRenderTargetValue(true, IX, IY, float4(FoamAdv * Wet, 0.0, 0.0, 0.0));
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
