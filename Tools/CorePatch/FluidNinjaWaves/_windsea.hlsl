	// ---- the wind sea (interface version 7): the incident wind waves of the plugin's table (User.WaveWindSeaTexture, bound to
	// Emitter.WaveWindSea), added to the exported B only. The table is (N + 1) x 1 RGBA32F: texel (0, 0) = (origin x, origin y, L, N), the
	// origin in world cm, L the long band's count, N every component's; texels (1..N, 0) = (kx, ky in rad/cm, amplitude cm, phase rad at the
	// origin this frame), the long band first. Here the long band is summed at the texel's world centre,
	//   W = sum over i = 1..L of a_i cos(k_i . (x - origin) + phase_i),
	// and the surface material draws the rest (texels L + 1..N) per pixel. The field is anchored in the world and the plugin advances the
	// phases on the CPU, so the window's scroll and remainder and the wave state play no part: nothing of it reaches (eta, v), the
	// breaking, the foam, Wave Aux or the bodies' own fields. Floating bodies feel it through B, as they feel the waves. An open window
	// fades it out across its sponge band (Wave.SpongeCm) so it ends with the window as the waves do. With no table bound (a 1 x 1
	// default), a table whose width is not N + 1, or L 0, it adds exactly 0.
	float WindSea = 0.0;
	{
		float2 WsDims;
		WaveWindSea.GetTextureDimensions(0, WsDims);
		int WsW = int(WsDims.x);
		if (WsW >= 2 && int(WsDims.y) == 1)
		{
			float4 WsHeader;
			WaveWindSea.LoadTexture2D(0, 0, 0, WsHeader);
			int WsLong = int(WsHeader.z + 0.5);
			int WsTotal = int(WsHeader.w + 0.5);
			if (WsTotal == WsW - 1 && WsLong >= 1 && WsLong <= WsTotal)
			{
				// The texel's offset from the origin, taken before anything large is added (LWCTile is 0 in a world without large coordinates).
				float2 WsRel = SrcSim.xy - (WsHeader.xy - LWCTile.xy * 2097152.0);
				for (int WsI = 1; WsI <= WsLong; ++WsI)
				{
					float4 WsC;
					WaveWindSea.LoadTexture2D(WsI, 0, 0, WsC);
					WindSea += WsC.z * cos(dot(WsC.xy, WsRel) + WsC.w);
				}
				if (EdgeOpen >= 0.5 && SpongeCm > 0.0)
				{
					float WsEdgeCm = min(min(U.x, 1.0 - U.x) * Extents.x, min(U.y, 1.0 - U.y) * Extents.y);
					WindSea *= saturate(WsEdgeCm / SpongeCm);
				}
			}
		}
	}
