// ---- depth: water Z (ClampingValue) minus the bed where the height field has one (and Wave.DepthFromBed is on), else Wave.DepthCm. With
// ClampHeightLowerValues FluidNinja flattens every bed below ClampingValue - 1 to that value, so those cells carry no depth. Wave.DepthFromBed
// 0: Wave.DepthCm everywhere (a pool or channel whose height field reads its water surface, not its floor).
// Interface version 3: the depth is the mean over the ScaleDown x ScaleDown block of pressure cells this wave cell covers, over the sub-cells
// whose bed the height field places below the water (a block with none runs at Wave.DepthCm), so a sloping bed is read at the wave cell's
// own scale. At ScaleDown 1 the block is the cell itself, read at its centre exactly as before. DepthShift is the grid's lag behind the
// window in cells (the scroll remainder in Wave Step, 0 in Wave Aux). Wave Step and Wave Aux take it alike.
float Depth = DepthCm;
if (HeightReaders && DepthFromBed > 0.5)
{
	float DepthSum = 0.0;
	float DepthCount = 0.0;
	for (int DepthJ = 0; DepthJ < ScaleDown; ++DepthJ)
	{
		for (int DepthK = 0; DepthK < ScaleDown; ++DepthK)
		{
			float2 DepthU = (float2(IX, IY) + (float2(DepthK, DepthJ) + 0.5) / float(ScaleDown) - DepthShift) / Cells;
			float3 DepthSim = mul(float4(DepthU, 0.5, 1.0), UnitToWorld).xyz;
			float Elevation;
			FieldCaptureGrid.SamplePreviousGridFloatValue<Attribute=LandscapeElevation>(DepthU, Elevation);
			float Bed = DepthSim.z + Elevation;
			if (!HeightClamp || Bed > ClampingValue - 0.5)
			{
				DepthSum += ClampingValue - Bed;
				DepthCount += 1.0;
			}
		}
	}
	if (DepthCount > 0.0)
	{
		Depth = DepthSum / DepthCount;
	}
}
Depth = max(Depth, 1.0);
