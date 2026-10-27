// FluidNinja Waves - stage "Wave Export" (iterates Emitter.RT_Pressure, after Export PressureAndDivergence). Rewrites each texel as
// (pressure, divergence, eta + P, P): (p, div) exactly as CopyGridToRenderTarget003 reads them, eta from the wave grid at the texel centre
// (the grid lags the window by the scroll remainder: Catmull-Rom, exact when there is none), P from the wave source at the texel's world centre.
// Interface version 3: the wave grid may be coarser than this target (Emitter.Wave.ScaleDown, S: the pressure buffer's cells over S). Then
// eta is upsampled bilinearly from the four wave cells around the texel's centre, and P comes from the source texture at the wave grid's
// cells, bilinear between them by world position as the source read always was; so RT_PressureDivergence keeps the pressure buffer's
// resolution and FluidNinja's surface material and the plugin's gather read it as before. At S = 1 the exact load and the Catmull-Rom
// read are what interface 2 did.
// Interface version 7: the wind sea (_windsea.hlsl). B gains W, the long band of the plugin's wind-sea table (User.WaveWindSeaTexture)
// summed at the texel's world centre and faded out across an open window's sponge band: B = eta + P + W. A keeps P; nothing else here
// changes, and with no table bound W is exactly 0 (interface 6's export).
int IX;
int IY;
RT_Pressure.ExecToIndex(IX, IY);
float3 PUnit;
PressureGrid.IndexToUnit(IX, IY, PUnit);
float2 PUnitScaled = float2(PUnit.x, PUnit.y) * Factor;
int PX;
int PY;
PressureGrid.UnitToIndex(PUnitScaled, PX, PY);
float2 PD;
PressureGrid.GetPreviousVector2DValue<Attribute=Pressure>(PX, PY, PD);

int NX;
int NY;
WaveGrid.GetNumCells(NX, NY);
int RW;
int RH;
RT_Pressure.GetRenderTargetSize(RW, RH);
int Scale = max(ScaleDown, 1);
float Eta = 0.0;
// The grid is sized for this target (Ninja_WaveParams divides the same way); a 1 x 1 grid (waves off, or not yet sized) exports 0.
if (NX == max(RW / Scale, 1) && NY == max(RH / Scale, 1))
{
	if (Scale == 1)
	{
		if (dot(Remainder, Remainder) <= 1e-4)
		{
			float2 Exact;
			WaveGrid.GetPreviousVector2DValue<Attribute=Wave>(IX, IY, Exact);
			Eta = Exact.x;
		}
		else
		{
			float2 CrPos = float2(IX, IY) + Remainder;
//@include _catmullrom.hlsl
			Eta = CrValue.x;
		}
	}
	else
	{
		// This texel's centre in wave cells (cell centres at integers; wave cell q's centre is at unit (q + 0.5 - Remainder) / N), and the
		// bilinear blend of the four cells around it, clamped to the grid so the window's outermost half cell reads its border cell.
		float2 Pos = (float2(IX, IY) + 0.5) / float2(RW, RH) * float2(NX, NY) - 0.5 + Remainder;
		float2 Base = floor(Pos);
		float2 F = Pos - Base;
		for (int BJ = 0; BJ < 2; ++BJ)
		{
			for (int BK = 0; BK < 2; ++BK)
			{
				int2 Q = clamp(int2(Base) + int2(BK, BJ), int2(0, 0), int2(NX - 1, NY - 1));
				float2 Tap;
				WaveGrid.GetPreviousVector2DValue<Attribute=Wave>(Q.x, Q.y, Tap);
				Eta += ((BK == 0 ? 1.0 - F.x : F.x) * (BJ == 0 ? 1.0 - F.y : F.y)) * Tap.x;
			}
		}
	}
}
float2 U = (float2(IX, IY) + 0.5) / float2(RW, RH);
float3 SrcSim = mul(float4(U, 0.5, 1.0), UnitToWorld).xyz;
//@include _source.hlsl
//@include _windsea.hlsl
RT_Pressure.SetRenderTargetValue(true, IX, IY, float4(PD.x, PD.y, Eta + SrcP + WindSea, SrcP));
Done = Eta;
