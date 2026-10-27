	// ---- the wash (interface version 6) at this cell's world centre: User.WaveWashTexture (Emitter.WaveWash), W x H RGBA16F with no header
	// row, written by the plugin on the SAME cells as rows 0..H-1 of the wave source texture: texel (i, j) covers [origin + i * cell,
	// origin + (i + 1) * cell) with the source texture's header origin and cell. Read with exactly the source texture's taps (_source.hlsl:
	// aligned cells load exactly, others bilinearly). Zero outside it, with no source header, or with no wash bound (the black default,
	// whose size is not the source's W x H). R = fresh foam rate (F1 per second), G = aged foam rate (F2 per second), B = the steady part
	// of this frame's impulse head y (cm s, the source texture's G units and sign), A = reserved. Foam only: nothing of it reaches (eta, v).
	float4 Wash = float4(0.0, 0.0, 0.0, 0.0);
	{
		float2 WashSrcDims;
		WaveSource.GetTextureDimensions(0, WashSrcDims);
		int WashSrcW = int(WashSrcDims.x);
		int WashSrcH = int(WashSrcDims.y) - 1;
		float2 WashDims;
		WaveWash.GetTextureDimensions(0, WashDims);
		int WashW = int(WashDims.x);
		int WashH = int(WashDims.y);
		if (WashSrcW >= 2 && WashSrcH >= 1 && WashW == WashSrcW && WashH == WashSrcH)
		{
			float4 WashHeader;
			WaveSource.LoadTexture2D(0, WashSrcH, 0, WashHeader);
			if (WashHeader.z > 0.0)
			{
				float2 WashWorld = SrcSim.xy + LWCTile.xy * 2097152.0;
				float2 WashT = (WashWorld - WashHeader.xy) / WashHeader.z - 0.5;
				float2 WashI = floor(WashT);
				float2 WashF = WashT - WashI;
				if (WashF.x > 0.99) { WashI.x += 1.0; WashF.x = 0.0; } else if (WashF.x < 0.01) { WashF.x = 0.0; }
				if (WashF.y > 0.99) { WashI.y += 1.0; WashF.y = 0.0; } else if (WashF.y < 0.01) { WashF.y = 0.0; }
				for (int WashJ = 0; WashJ < 2; ++WashJ)
				{
					for (int WashK = 0; WashK < 2; ++WashK)
					{
						float WashWeight = (WashK == 0 ? 1.0 - WashF.x : WashF.x) * (WashJ == 0 ? 1.0 - WashF.y : WashF.y);
						int2 WashQ = int2(WashI) + int2(WashK, WashJ);
						if (WashWeight > 0.0 && WashQ.x >= 0 && WashQ.y >= 0 && WashQ.x < WashW && WashQ.y < WashH)
						{
							float4 WashTexel;
							WaveWash.LoadTexture2D(WashQ.x, WashQ.y, 0, WashTexel);
							Wash += WashWeight * WashTexel;
						}
					}
				}
			}
		}
	}
