// FluidNinja Waves - stage "Wave Aux" (iterates Emitter.WaveAux, after Wave Step; runs only while a render target of the wave grid's size is
// bound at User.WaveAuxTarget). For readers outside the core (FluidNinja Buoyancy's per-body own fields, which step this same water on their own
// cells): per cell, at the window's cell centres (the lattice RT_PressureDivergence is written on), (m, depth, drift x, drift y) =
// the obstacle mask exactly as Wave Prepare builds it, the depth exactly as Wave Step takes it (cm), and FluidNinja's current as
// Wave Prepare advects by it (wave cells per tick).
// Interface version 3: the target is at the wave grid's size, the pressure buffer's cells over Emitter.Wave.ScaleDown (S); the mask and the
// depth are judged over each wave cell's S x S block of pressure cells (_mask.hlsl, _depth.hlsl), as Wave Prepare and Wave Step judge them.
// At S = 1 everything is what interface 2 did.
// Interface version 4: the foam F this tick's Wave Step wrote (WaveGrid attribute Foam) goes into the foam target (Emitter.WaveFoam,
// the plugin's, at the wave grid's size), at the window's own cell centres like everything here: the grid lags the window by the scroll
// remainder, so the window's cell reads the grid bilinearly at + Remainder (exactly when there is none), as Wave Export reads eta. The
// surface material samples that target by the simulation's UV, as it samples RT_PressureDivergence. 0 while the foam is off.
// Interface version 6: the export is (F1, F2, v, 0): the fresh foam (Foam), the aged foam (FoamAged) and the wave state's vertical velocity
// v (cm/s, the Wave attribute's .y), each read the same way, exactly when there is no remainder, else bilinearly at + Remainder. The plugin
// allocates the target as RGBA16F; an RG or R target keeps the channels it has. F1 and F2 are 0 while the foam is off; v is written
// whenever the target is (for readers to come: nothing reads it yet). This overwrites Wave Prepare's scratch, so readers never see it.
int IX;
int IY;
WaveAux.ExecToIndex(IX, IY);
int NX;
int NY;
WaveAux.GetRenderTargetSize(NX, NY);
float2 Cells = float2(NX, NY);
float2 U = (float2(IX, IY) + 0.5) / Cells;
float3 SrcSim = mul(float4(U, 0.5, 1.0), UnitToWorld).xyz;

float2 MaskShift = float2(0.0, 0.0);
//@include _mask.hlsl

float2 DepthShift = float2(0.0, 0.0);
//@include _depth.hlsl

// ---- drift (Wave Prepare): FluidNinja's current in wave cells per tick, averaged around the cell exactly as Wave Prepare takes it
//@include _drift.hlsl

WaveAux.SetRenderTargetValue(true, IX, IY, float4(Wet, Depth, Drift.x, Drift.y));

// ---- foam export
if (FoamExport)
{
	float Foam = 0.0;
	float FoamAged = 0.0;
	float2 FoamState = float2(0.0, 0.0);
	if (dot(Remainder, Remainder) <= 1e-4)
	{
		WaveGrid.GetPreviousVector2DValue<Attribute=Wave>(IX, IY, FoamState);
		if (FoamEnabled)
		{
			WaveGrid.GetPreviousFloatValue<Attribute=Foam>(IX, IY, Foam);
			WaveGrid.GetPreviousFloatValue<Attribute=FoamAged>(IX, IY, FoamAged);
		}
	}
	else
	{
		float2 FoamPos = float2(IX, IY) + Remainder;
		WaveGrid.SamplePreviousGridVector2DValue<Attribute=Wave>((FoamPos + 0.5) / Cells, FoamState);
		if (FoamEnabled)
		{
			WaveGrid.SamplePreviousGridFloatValue<Attribute=Foam>((FoamPos + 0.5) / Cells, Foam);
			WaveGrid.SamplePreviousGridFloatValue<Attribute=FoamAged>((FoamPos + 0.5) / Cells, FoamAged);
		}
	}
	WaveFoam.SetRenderTargetValue(true, IX, IY, float4(Foam, FoamAged, FoamState.y, 0.0));
}
Done = Wet;
