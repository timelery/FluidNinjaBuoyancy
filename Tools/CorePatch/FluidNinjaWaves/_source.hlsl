	// ---- the wave source at this cell's world centre (User.WaveSourceTexture, W x (H + 1); header texel (0, H) = origin x, origin y, cell, serial).
	// Texel (i, j) covers [origin + i * cell, origin + (i + 1) * cell); aligned cells load exactly, others bilinearly; outside it, or with no
	// texture bound (the black default) or no header, the source is zero. A = the hull's speed through the water at the cell (cm/s, the
	// fastest hull pressing it; interface version 4, for the foam's waterline shear): 0 from a plugin that writes none.
	float SrcP = 0.0;
	float SrcY = 0.0;
	float SrcErase = 0.0;
	float SrcSpeed = 0.0;
	{
		float2 SrcDims;
		WaveSource.GetTextureDimensions(0, SrcDims);
		int SrcW = int(SrcDims.x);
		int SrcH = int(SrcDims.y) - 1;
		if (SrcW >= 2 && SrcH >= 1)
		{
			float4 SrcHeader;
			WaveSource.LoadTexture2D(0, SrcH, 0, SrcHeader);
			if (SrcHeader.z > 0.0)
			{
				float2 SrcWorld = SrcSim.xy + LWCTile.xy * 2097152.0;
				float2 SrcT = (SrcWorld - SrcHeader.xy) / SrcHeader.z - 0.5;
				float2 SrcI = floor(SrcT);
				float2 SrcF = SrcT - SrcI;
				if (SrcF.x > 0.99) { SrcI.x += 1.0; SrcF.x = 0.0; } else if (SrcF.x < 0.01) { SrcF.x = 0.0; }
				if (SrcF.y > 0.99) { SrcI.y += 1.0; SrcF.y = 0.0; } else if (SrcF.y < 0.01) { SrcF.y = 0.0; }
				float4 SrcSum = float4(0.0, 0.0, 0.0, 0.0);
				for (int SrcJ = 0; SrcJ < 2; ++SrcJ)
				{
					for (int SrcK = 0; SrcK < 2; ++SrcK)
					{
						float SrcWeight = (SrcK == 0 ? 1.0 - SrcF.x : SrcF.x) * (SrcJ == 0 ? 1.0 - SrcF.y : SrcF.y);
						int2 SrcQ = int2(SrcI) + int2(SrcK, SrcJ);
						if (SrcWeight > 0.0 && SrcQ.x >= 0 && SrcQ.y >= 0 && SrcQ.x < SrcW && SrcQ.y < SrcH)
						{
							float4 SrcTexel;
							WaveSource.LoadTexture2D(SrcQ.x, SrcQ.y, 0, SrcTexel);
							SrcSum += SrcWeight * SrcTexel;
						}
					}
				}
				SrcP = SrcSum.x;
				SrcY = SrcSum.y;
				SrcErase = SrcSum.z;
				SrcSpeed = max(SrcSum.w, 0.0);
			}
		}
	}
