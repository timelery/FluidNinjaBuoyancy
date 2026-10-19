// ---- the drift the waves are carried by (wave cells per tick): FluidNinja's current around the cell, averaged by a 3 x 3 binomial at two
// wave cells' spacing. A wave feels the current over its own size, not a grid-scale eddy; and Catmull-Rom back-tracing through a drift that
// changes sign from one cell to the next (inside an eddy) amplifies grid-scale waves. Wave Prepare and Wave Aux take it alike.
float2 Drift = float2(0.0, 0.0);
{
	float DriftW[3] = {0.25, 0.5, 0.25};
	for (int DriftJ = 0; DriftJ < 3; ++DriftJ)
	{
		for (int DriftK = 0; DriftK < 3; ++DriftK)
		{
			float3 DriftComp;
			CompositeGrid.SamplePreviousGridVector3Value<Attribute=Composite>(U + float2(DriftK - 1, DriftJ - 1) * 2.0 / Cells, DriftComp);
			Drift += (DriftW[DriftK] * DriftW[DriftJ]) * DriftComp.xy;
		}
	}
	Drift *= CellUnit * SimSpeed * CurrentCoupling * Cells;
}
