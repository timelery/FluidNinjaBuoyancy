	// ---- (eta, v) of the previous wave state at CrPos (wave cells, cell centres at integers): 16-tap Catmull-Rom, outside the grid 0.
	float2 CrValue = float2(0.0, 0.0);
	{
		float2 CrBase = floor(CrPos);
		float2 CrT = CrPos - CrBase;
		float2 CrT2 = CrT * CrT;
		float2 CrT3 = CrT2 * CrT;
		float2 CrW[4];
		CrW[0] = 0.5 * (-CrT3 + 2.0 * CrT2 - CrT);
		CrW[1] = 0.5 * (3.0 * CrT3 - 5.0 * CrT2 + 2.0);
		CrW[2] = 0.5 * (-3.0 * CrT3 + 4.0 * CrT2 + CrT);
		CrW[3] = 0.5 * (CrT3 - CrT2);
		int2 CrI = int2(CrBase);
		for (int CrJ = 0; CrJ < 4; ++CrJ)
		{
			for (int CrK = 0; CrK < 4; ++CrK)
			{
				int2 CrQ = CrI + int2(CrK - 1, CrJ - 1);
				if (CrQ.x >= 0 && CrQ.y >= 0 && CrQ.x < NX && CrQ.y < NY)
				{
					float2 CrTap;
					WaveGrid.GetPreviousVector2DValue<Attribute=Wave>(CrQ.x, CrQ.y, CrTap);
					CrValue += (CrW[CrK].x * CrW[CrJ].y) * CrTap;
				}
			}
		}
	}
