// ---- obstacles: FluidNinja's SDF collision (MeshField.w) and height mask, judged over the ScaleDown x ScaleDown block of pressure cells
// this wave cell covers (interface version 3): the cell is wet when more than half of its block is, so a bank is kept where it is to
// within half a pressure cell whatever the scale-down. At ScaleDown 1 the block is the cell itself, read at its centre exactly as before.
// MaskShift is the grid's lag behind the window in cells (the scroll remainder in Wave Prepare, 0 in Wave Aux, which writes at the
// window's own cell centres). Wave Prepare and Wave Aux take it alike, so the plugin's own fields see the core's mask.
float Wet = 1.0;
if ((SDFCollision && SDFReader) || (HeightCollision && HeightReaders && HeightClamp))
{
	float WetCount = 0.0;
	for (int MaskJ = 0; MaskJ < ScaleDown; ++MaskJ)
	{
		for (int MaskK = 0; MaskK < ScaleDown; ++MaskK)
		{
			float2 MaskU = (float2(IX, IY) + (float2(MaskK, MaskJ) + 0.5) / float(ScaleDown) - MaskShift) / Cells;
			float SubWet = 1.0;
			if (SDFCollision && SDFReader)
			{
				float4 MeshField;
				FieldCaptureGrid.SamplePreviousGridVector4Value<Attribute=MeshField>(MaskU, MeshField);
				SubWet = MeshField.w < 0.5 ? SubWet : 0.0;
			}
			if (HeightCollision && HeightReaders && HeightClamp)
			{
				float3 MaskSim = mul(float4(MaskU, 0.5, 1.0), UnitToWorld).xyz;
				float MaskElevation;
				FieldCaptureGrid.SamplePreviousGridFloatValue<Attribute=LandscapeElevation>(MaskU, MaskElevation);
				SubWet = (MaskElevation < ClampingValue - MaskSim.z + 1.0) ? SubWet : 0.0;
			}
			WetCount += SubWet;
		}
	}
	Wet = 2.0 * WetCount > float(ScaleDown * ScaleDown) ? 1.0 : 0.0;
}
// The window border, where it is a wall.
if (EdgeOpen < 0.5 && (IX == 0 || IY == 0 || IX == NX - 1 || IY == NY - 1))
{
	Wet = 0.0;
}
