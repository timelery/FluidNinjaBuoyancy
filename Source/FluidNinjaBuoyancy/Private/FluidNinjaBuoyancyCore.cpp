#include "FluidNinjaBuoyancyCore.h"

#include "Components/PrimitiveComponent.h"
#include "FluidNinjaBuoyancyLog.h"
#include "FluidNinjaBuoyancyStats.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Stats/Stats.h"

namespace FluidNinjaBuoyancy
{
	namespace
	{
		struct FMeshBuilder
		{
			TArray<FVector3f> V;
			TArray<FIntVector3> T;
		};

		/** Turns the triangles from FirstTriangle on to face away from Centre (each collision piece is convex). */
		void OrientOutward(FMeshBuilder& Mesh, int32 FirstTriangle, const FVector3f& Centre)
		{
			for (int32 Index = FirstTriangle; Index < Mesh.T.Num(); ++Index)
			{
				FIntVector3& Tri = Mesh.T[Index];
				const FVector3f A = Mesh.V[Tri.X];
				const FVector3f B = Mesh.V[Tri.Y];
				const FVector3f C = Mesh.V[Tri.Z];
				const FVector3f Normal = FVector3f::CrossProduct(B - A, C - A);
				if (FVector3f::DotProduct(Normal, (A + B + C) / 3.f - Centre) < 0.f)
				{
					Swap(Tri.Y, Tri.Z);
				}
			}
		}

		uint64 EdgeKey(int32 A, int32 B)
		{
			if (A > B)
			{
				Swap(A, B);
			}
			return (static_cast<uint64>(static_cast<uint32>(A)) << 32) | static_cast<uint32>(B);
		}

		/** A convex piece (its points and triangles), as planes facing out. */
		struct FConvexPiece
		{
			TArray<FVector3f> Normals;
			TArray<float> Offsets;
			FVector3f Centre = FVector3f::ZeroVector;
			FVector3f Min = FVector3f(TNumericLimits<float>::Max());
			FVector3f Max = FVector3f(-TNumericLimits<float>::Max());

			FConvexPiece(const TArray<FVector3f>& Points, const TArray<FIntVector3>& Triangles)
			{
				for (const FVector3f& Point : Points)
				{
					Centre += Point;
					Min = Min.ComponentMin(Point);
					Max = Max.ComponentMax(Point);
				}
				Centre /= static_cast<float>(FMath::Max(Points.Num(), 1));
				for (const FIntVector3& Tri : Triangles)
				{
					const FVector3f A = Points[Tri.X];
					FVector3f Normal = FVector3f::CrossProduct(Points[Tri.Y] - A, Points[Tri.Z] - A);
					if (Normal.SizeSquared() < 1e-8f)
					{
						continue;
					}
					Normal.Normalize();
					if (FVector3f::DotProduct(Normal, A - Centre) < 0.f)
					{
						Normal = -Normal;
					}
					Normals.Add(Normal);
					Offsets.Add(FVector3f::DotProduct(Normal, A));
				}
			}

			/** Where the ray from the centre through Point leaves the piece. */
			FVector3f Project(const FVector3f& Point) const
			{
				FVector3f Direction = Point - Centre;
				const float Length = Direction.Size();
				if (Length < 1e-3f)
				{
					return Point;
				}
				Direction /= Length;
				float Exit = TNumericLimits<float>::Max();
				for (int32 Index = 0; Index < Normals.Num(); ++Index)
				{
					const float Facing = FVector3f::DotProduct(Normals[Index], Direction);
					if (Facing > 1e-6f)
					{
						Exit = FMath::Min(Exit, (Offsets[Index] - FVector3f::DotProduct(Normals[Index], Centre)) / Facing);
					}
				}
				return Exit < TNumericLimits<float>::Max() ? Centre + Direction * Exit : Point;
			}

			FIntVector Cells(float Resolution) const
			{
				const FVector3f Extent = Max - Min;
				return FIntVector(FMath::Max(1, FMath::CeilToInt32(Extent.X / Resolution)), FMath::Max(1, FMath::CeilToInt32(Extent.Y / Resolution)),
					FMath::Max(1, FMath::CeilToInt32(Extent.Z / Resolution)));
			}
		};

		int32 ShellTriangles(const FIntVector& Cells)
		{
			return 4 * (Cells.X * Cells.Y + Cells.Y * Cells.Z + Cells.X * Cells.Z);
		}

		/** An even shell over a convex piece: a grid box matching its bounds (cells of about the resolution), every grid point pushed
		 * out from the centre onto the piece's surface. Even, well-shaped triangles whatever the source triangulation (a collision
		 * hull's long slivers would otherwise need many splits each). */
		void AddEvenShell(FMeshBuilder& Mesh, const FConvexPiece& Piece, const FIntVector& Cells)
		{
			const int32 FirstTriangle = Mesh.T.Num();
			const int32 Count[3] = {Cells.X, Cells.Y, Cells.Z};
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const int32 U = (Axis + 1) % 3;
				const int32 V = (Axis + 2) % 3;
				for (int32 Side = 0; Side < 2; ++Side)
				{
					const int32 Base = Mesh.V.Num();
					for (int32 J = 0; J <= Count[V]; ++J)
					{
						for (int32 I = 0; I <= Count[U]; ++I)
						{
							FVector3f Point;
							Point[Axis] = Side ? Piece.Max[Axis] : Piece.Min[Axis];
							Point[U] = FMath::Lerp(Piece.Min[U], Piece.Max[U], static_cast<float>(I) / Count[U]);
							Point[V] = FMath::Lerp(Piece.Min[V], Piece.Max[V], static_cast<float>(J) / Count[V]);
							Mesh.V.Add(Piece.Project(Point));
						}
					}
					const int32 Row = Count[U] + 1;
					for (int32 J = 0; J < Count[V]; ++J)
					{
						for (int32 I = 0; I < Count[U]; ++I)
						{
							const int32 A = Base + J * Row + I;
							Mesh.T.Add(FIntVector3(A, A + 1, A + Row + 1));
							Mesh.T.Add(FIntVector3(A, A + Row + 1, A + Row));
						}
					}
				}
			}
			OrientOutward(Mesh, FirstTriangle, Piece.Centre);
		}

		void AddIcosphere(FMeshBuilder& Mesh, const FTransform& Transform, const FVector& Scale, float Radius, float HalfLength, int32 Levels)
		{
			// Icosahedron, refined Levels times and pushed onto the sphere; a capsule moves each half apart along Z.
			const float G = (1.f + FMath::Sqrt(5.f)) * 0.5f;
			TArray<FVector3f> Points = {
				{-1, G, 0}, {1, G, 0}, {-1, -G, 0}, {1, -G, 0}, {0, -1, G}, {0, 1, G},
				{0, -1, -G}, {0, 1, -G}, {G, 0, -1}, {G, 0, 1}, {-G, 0, -1}, {-G, 0, 1}};
			TArray<FIntVector3> Faces = {
				{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
				{3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
			for (FVector3f& Point : Points)
			{
				Point.Normalize();
			}
			for (int32 Level = 0; Level < Levels; ++Level)
			{
				TMap<uint64, int32> Midpoints;
				TArray<FIntVector3> Next;
				auto Mid = [&Points, &Midpoints](int32 A, int32 B)
				{
					if (const int32* Found = Midpoints.Find(EdgeKey(A, B)))
					{
						return *Found;
					}
					const int32 Index = Points.Add(((Points[A] + Points[B]) * 0.5f).GetSafeNormal());
					Midpoints.Add(EdgeKey(A, B), Index);
					return Index;
				};
				for (const FIntVector3& Face : Faces)
				{
					const int32 AB = Mid(Face.X, Face.Y);
					const int32 BC = Mid(Face.Y, Face.Z);
					const int32 CA = Mid(Face.Z, Face.X);
					Next.Add(FIntVector3(Face.X, AB, CA));
					Next.Add(FIntVector3(Face.Y, BC, AB));
					Next.Add(FIntVector3(Face.Z, CA, BC));
					Next.Add(FIntVector3(AB, BC, CA));
				}
				Faces = MoveTemp(Next);
			}
			const int32 Base = Mesh.V.Num();
			const int32 FirstTriangle = Mesh.T.Num();
			for (const FVector3f& Point : Points)
			{
				FVector Local = FVector(Point) * Radius;
				Local.Z += Point.Z >= 0.f ? HalfLength : -HalfLength;
				Mesh.V.Add(FVector3f(Transform.TransformPosition(Local) * Scale));
			}
			for (const FIntVector3& Face : Faces)
			{
				Mesh.T.Add(FIntVector3(Base + Face.X, Base + Face.Y, Base + Face.Z));
			}
			OrientOutward(Mesh, FirstTriangle, FVector3f(Transform.GetTranslation() * Scale));
		}

		/** What AddIcosphere makes of a sphere or capsule refined Levels times: 20 faces, four from each per level. */
		int32 IcosphereTriangles(int32 Levels)
		{
			return 20 << (2 * Levels);
		}

		/** One collision piece as a point test, for the overlap check: a convex piece by its planes, a sphere or capsule in the element's
		 * own frame (the shell's vertices are the element's points transformed and then scaled, so a point goes back the same way). */
		struct FPieceProbe
		{
			FString Name;
			const FConvexPiece* Convex = nullptr;   // null: a sphere or a capsule
			FTransform ElementTransform;
			FVector Scale = FVector::OneVector;
			float Radius = 0.f;
			float HalfLength = 0.f;
			FVector3f Min = FVector3f(TNumericLimits<float>::Max());
			FVector3f Max = FVector3f(-TNumericLimits<float>::Max());

			bool Contains(const FVector3f& Point) const
			{
				if (Convex)
				{
					for (int32 Index = 0; Index < Convex->Normals.Num(); ++Index)
					{
						if (FVector3f::DotProduct(Convex->Normals[Index], Point) > Convex->Offsets[Index])
						{
							return false;
						}
					}
					return true;
				}
				FVector Unscaled(Point);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					Unscaled[Axis] = FMath::Abs(Scale[Axis]) > 1e-6 ? Unscaled[Axis] / Scale[Axis] : 0.0;
				}
				const FVector Local = ElementTransform.InverseTransformPosition(Unscaled);
				const double Along = FMath::Clamp(Local.Z, -static_cast<double>(HalfLength), static_cast<double>(HalfLength));
				return FVector::DistSquared(Local, FVector(0.0, 0.0, Along)) <= static_cast<double>(Radius) * Radius;
			}
		};

		/** The pairs of collision pieces that share volume, each pair's common bounds sampled on a small grid: the shell is every piece's
		 * surface, so a shared volume is enclosed twice and the body lifts and drags there as if it were two bodies. Lists the pairs
		 * that share at least half a percent of the hull's volume, and returns the sum of their shares (the hull's volume counts the
		 * overlaps twice already, so the share is of that). A pair that only touches at a face or an edge is not an overlap. */
		float OverlappingPieces(const TArray<FPieceProbe>& Probes, float HullVolume, FString& OutPairs)
		{
			constexpr int32 Cells = 8;
			double Shared = 0.0;
			for (int32 I = 0; I < Probes.Num(); ++I)
			{
				for (int32 J = I + 1; J < Probes.Num(); ++J)
				{
					const FVector3f Low = Probes[I].Min.ComponentMax(Probes[J].Min);
					const FVector3f Extent = Probes[I].Max.ComponentMin(Probes[J].Max) - Low;
					if (Extent.GetMin() <= 0.01f)
					{
						continue;
					}
					int32 Inside = 0;
					for (int32 X = 0; X < Cells; ++X)
					{
						for (int32 Y = 0; Y < Cells; ++Y)
						{
							for (int32 Z = 0; Z < Cells; ++Z)
							{
								const FVector3f Point = Low + Extent * FVector3f((X + 0.5f) / Cells, (Y + 0.5f) / Cells, (Z + 0.5f) / Cells);
								if (Probes[I].Contains(Point) && Probes[J].Contains(Point))
								{
									++Inside;
								}
							}
						}
					}
					if (Inside == 0)
					{
						continue;
					}
					const double Volume = static_cast<double>(Extent.X) * Extent.Y * Extent.Z * Inside / (Cells * Cells * Cells);
					const double Share = Volume / FMath::Max(static_cast<double>(HullVolume), 1.0);
					if (Share < 0.005)
					{
						continue;
					}
					OutPairs += FString::Printf(TEXT("%s%s and %s (%.0f%%)"), OutPairs.IsEmpty() ? TEXT("") : TEXT(", "), *Probes[I].Name, *Probes[J].Name, Share * 100.0);
					Shared += Share;
				}
			}
			return static_cast<float>(Shared);
		}
	}

	bool FWaterPatch::Contains(double X, double Y) const
	{
		const double I = (X - Origin.X) / Texel;
		const double J = (Y - Origin.Y) / Texel;
		return NX > 0 && NY > 0 && I >= -0.5 && J >= -0.5 && I <= NX - 0.5 && J <= NY - 0.5;
	}

	bool FWaterPatch::Sample(double X, double Y, float& OutHeight, float& OutRate, FVector2f& OutFlow, float* OutWave) const
	{
		if (!Contains(X, Y))
		{
			return false;
		}
		const double I = FMath::Clamp((X - Origin.X) / Texel, 0.0, static_cast<double>(NX - 1));
		const double J = FMath::Clamp((Y - Origin.Y) / Texel, 0.0, static_cast<double>(NY - 1));
		const int32 I0 = FMath::FloorToInt32(I);
		const int32 J0 = FMath::FloorToInt32(J);
		const int32 I1 = FMath::Min(I0 + 1, NX - 1);
		const int32 J1 = FMath::Min(J0 + 1, NY - 1);
		const float A = static_cast<float>(I - I0);
		const float B = static_cast<float>(J - J0);
		// The waves ride on the same taps and weights: they count only where the water is drawn. Asked for without waves they are 0.
		const bool bWave = OutWave && bHasWave && Wave.Num() == NX * NY;
		float Weight = 0.f;
		float H = 0.f;
		float R = 0.f;
		float Wv = 0.f;
		FVector2f F = FVector2f::ZeroVector;
		auto Tap = [&](int32 TI, int32 TJ, float W)
		{
			if (W <= 0.f)
			{
				return;
			}
			const int32 K = TJ * NX + TI;
			const float Value = Height[K];
			if (FMath::IsNaN(Value))
			{
				return;
			}
			Weight += W;
			H += Value * W;
			R += Rate[K] * W;
			if (bHasFlow)
			{
				F += Flow[K] * W;
			}
			if (bWave)
			{
				Wv += Wave[K] * W;
			}
		};
		Tap(I0, J0, (1.f - A) * (1.f - B));
		Tap(I1, J0, A * (1.f - B));
		Tap(I0, J1, (1.f - A) * B);
		Tap(I1, J1, A * B);
		if (Weight < 0.3f)
		{
			return false;
		}
		OutHeight = H / Weight;
		OutRate = R / Weight;
		OutFlow = F / Weight;
		if (OutWave)
		{
			*OutWave = bWave ? Wv / Weight : 0.f;
		}
		return true;
	}

	float FWaterPatch::SampleWave(double X, double Y) const
	{
		float H = 0.f;
		float R = 0.f;
		FVector2f F = FVector2f::ZeroVector;
		float W = 0.f;
		return Sample(X, Y, H, R, F, &W) ? W : 0.f;
	}

	bool FWaterView::Sample(const FVector& Point, float& OutHeight, FVector& OutWaterVelocity) const
	{
		if (Patch)
		{
			float Height = 0.f;
			float Rate = 0.f;
			FVector2f Flow = FVector2f::ZeroVector;
			float Wave = 0.f;
			// One set of taps for the surface and the waves (κ = 0 asks for no waves, so nothing is read for them).
			if (Patch->Sample(Point.X, Point.Y, Height, Rate, Flow, WaveFeel != 0.f ? &Wave : nullptr))
			{
				if (bAmbientFlow && Patch->bAmbient)
				{
					Flow = Patch->AmbientAt(Point.X, Point.Y);
				}
				// FluidNinja's own surface carried forward, plus κ of the waves the body feels (everything's but its own: B - B_self).
				OutHeight = Height + Rate * Extrapolate + (WaveFeel != 0.f ? WaveFeel * Wave : 0.f);
				// The rate only carries the heights forward. It is not a water velocity for drag: FluidNinja steps unevenly and starts up
				// with jumps, and as a vertical speed against a flat bottom those launched hulls.
				OutWaterVelocity = FVector(Flow.X, Flow.Y, 0.0);
				return true;
			}
			if (Patch->Contains(Point.X, Point.Y))
			{
				return false;   // drawn area, no water here (a bank, a rock)
			}
		}
		if (bFlat)
		{
			OutHeight = FlatHeight;
			OutWaterVelocity = FVector::ZeroVector;
			return true;
		}
		return false;
	}

	bool BuildHull(UPrimitiveComponent& Primitive, float ResolutionCm, int32 MaxTriangles, FHull& Out, FString& OutWhy)
	{
		Out = FHull();
		const UBodySetup* Setup = Primitive.GetBodySetup();
		if (!Setup)
		{
			OutWhy = TEXT("no body setup (collision)");
			return false;
		}
		const FKAggregateGeom& Geom = Setup->AggGeom;
		const FVector Scale = Primitive.GetComponentTransform().GetScale3D();

		// The collision pieces as convex point sets (boxes too); spheres and capsules separately.
		TArray<FConvexPiece> Pieces;
		int32 Convexes = 0;
		for (const FKConvexElem& Source : Geom.ConvexElems)
		{
			FKConvexElem Elem = Source;
			if (Elem.IndexData.Num() < 3)
			{
				Elem.ComputeChaosConvexIndices(true);
			}
			if (Elem.IndexData.Num() < 3 || Elem.VertexData.Num() < 4)
			{
				continue;
			}
			const FTransform ElemTransform = Elem.GetTransform();
			TArray<FVector3f> Points;
			for (const FVector& Point : Elem.VertexData)
			{
				Points.Add(FVector3f(ElemTransform.TransformPosition(Point) * Scale));
			}
			TArray<FIntVector3> Triangles;
			for (int32 Index = 0; Index + 2 < Elem.IndexData.Num(); Index += 3)
			{
				Triangles.Add(FIntVector3(Elem.IndexData[Index], Elem.IndexData[Index + 1], Elem.IndexData[Index + 2]));
			}
			Pieces.Emplace(Points, Triangles);
			++Convexes;
		}
		for (const FKBoxElem& Box : Geom.BoxElems)
		{
			const FTransform BoxTransform = Box.GetTransform();
			const FVector Half(Box.X * 0.5, Box.Y * 0.5, Box.Z * 0.5);
			TArray<FVector3f> Points;
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector Local((Corner & 1) ? Half.X : -Half.X, (Corner & 2) ? Half.Y : -Half.Y, (Corner & 4) ? Half.Z : -Half.Z);
				Points.Add(FVector3f(BoxTransform.TransformPosition(Local) * Scale));
			}
			static const int32 Faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
			TArray<FIntVector3> Triangles;
			for (const auto& Face : Faces)
			{
				Triangles.Add(FIntVector3(Face[0], Face[1], Face[2]));
				Triangles.Add(FIntVector3(Face[0], Face[2], Face[3]));
			}
			Pieces.Emplace(Points, Triangles);
		}
		if (Pieces.IsEmpty() && Geom.SphereElems.IsEmpty() && Geom.SphylElems.IsEmpty())
		{
			OutWhy = TEXT("no simple collision (convex, box, sphere or capsule) to float on");
			return false;
		}

		// Cells of about the resolution; coarser if the shells would pass MaxTriangles. The spheres and capsules are in the budget too: an
		// icosphere refined to its radius is 20 × 4^levels triangles, up to 5,120, which used to come on top of the convex shells' count.
		float Resolution = FMath::Max(ResolutionCm, 1.f);
		auto Levels = [](float Radius, float AtResolution)
		{
			return FMath::Clamp(FMath::CeilToInt32(FMath::Log2(FMath::Max(1.05f * Radius / AtResolution, 1.f))), 1, 4);
		};
		for (int32 Attempt = 0; Attempt < 12; ++Attempt)
		{
			int32 Total = 0;
			for (const FConvexPiece& Piece : Pieces)
			{
				Total += ShellTriangles(Piece.Cells(Resolution));
			}
			for (const FKSphereElem& Sphere : Geom.SphereElems)
			{
				Total += IcosphereTriangles(Levels(Sphere.Radius * static_cast<float>(Scale.GetMax()), Resolution));
			}
			for (const FKSphylElem& Capsule : Geom.SphylElems)
			{
				Total += IcosphereTriangles(Levels(Capsule.Radius * static_cast<float>(Scale.GetMax()), Resolution));
			}
			if (Total <= MaxTriangles)
			{
				break;
			}
			Resolution *= 1.25f;
		}
		// Every piece as a point test and its bounds, for the overlap check once the shell is built (Pieces is complete here, so the
		// pointers into it hold).
		TArray<FPieceProbe> Probes;
		Probes.Reserve(Pieces.Num() + Geom.SphereElems.Num() + Geom.SphylElems.Num());
		FMeshBuilder Mesh;
		for (int32 Index = 0; Index < Pieces.Num(); ++Index)
		{
			const FConvexPiece& Piece = Pieces[Index];
			AddEvenShell(Mesh, Piece, Piece.Cells(Resolution));
			FPieceProbe& Probe = Probes.AddDefaulted_GetRef();
			Probe.Name = Index < Convexes ? FString::Printf(TEXT("convex %d"), Index) : FString::Printf(TEXT("box %d"), Index - Convexes);
			Probe.Convex = &Piece;
			Probe.Min = Piece.Min;
			Probe.Max = Piece.Max;
		}
		auto AddRound = [&Mesh, &Probes, &Scale, &Levels, Resolution](const FString& Name, const FTransform& Transform, float Radius, float HalfLength)
		{
			const int32 FirstVertex = Mesh.V.Num();
			AddIcosphere(Mesh, Transform, Scale, Radius, HalfLength, Levels(Radius * static_cast<float>(Scale.GetMax()), Resolution));
			FPieceProbe& Probe = Probes.AddDefaulted_GetRef();
			Probe.Name = Name;
			Probe.ElementTransform = Transform;
			Probe.Scale = Scale;
			Probe.Radius = Radius;
			Probe.HalfLength = HalfLength;
			for (int32 Index = FirstVertex; Index < Mesh.V.Num(); ++Index)
			{
				Probe.Min = Probe.Min.ComponentMin(Mesh.V[Index]);
				Probe.Max = Probe.Max.ComponentMax(Mesh.V[Index]);
			}
		};
		for (int32 Index = 0; Index < Geom.SphereElems.Num(); ++Index)
		{
			const FKSphereElem& Sphere = Geom.SphereElems[Index];
			AddRound(FString::Printf(TEXT("sphere %d"), Index), FTransform(Sphere.Center), Sphere.Radius, 0.f);
		}
		for (int32 Index = 0; Index < Geom.SphylElems.Num(); ++Index)
		{
			const FKSphylElem& Capsule = Geom.SphylElems[Index];
			AddRound(FString::Printf(TEXT("capsule %d"), Index), Capsule.GetTransform(), Capsule.Radius, Capsule.Length * 0.5f);
		}

		Out.Vertices = MoveTemp(Mesh.V);
		Out.BoundsMin = FVector3f(TNumericLimits<float>::Max());
		Out.BoundsMax = FVector3f(-TNumericLimits<float>::Max());
		for (const FVector3f& Point : Out.Vertices)
		{
			Out.BoundsMin = Out.BoundsMin.ComponentMin(Point);
			Out.BoundsMax = Out.BoundsMax.ComponentMax(Point);
		}
		double Volume = 0.0;
		FVector3d Moment = FVector3d::ZeroVector;
		for (const FIntVector3& Tri : Mesh.T)
		{
			const FVector3f A = Out.Vertices[Tri.X];
			const FVector3f B = Out.Vertices[Tri.Y];
			const FVector3f C = Out.Vertices[Tri.Z];
			const FVector3f Cross = FVector3f::CrossProduct(B - A, C - A);
			const float TwiceArea = Cross.Size();
			if (TwiceArea < 1e-4f)
			{
				continue;
			}
			Out.Triangles.Add(Tri);
			Out.Normals.Add(Cross / TwiceArea);
			Out.Areas.Add(0.5f * TwiceArea);
			Out.TotalArea += 0.5f * TwiceArea;
			const double Tetra = FVector3d::DotProduct(FVector3d(A), FVector3d(Cross)) / 6.0;
			Volume += Tetra;
			Moment += FVector3d(A + B + C) * (Tetra / 4.0);
		}
		if (Volume < 0.0)
		{
			for (int32 Index = 0; Index < Out.Triangles.Num(); ++Index)
			{
				Swap(Out.Triangles[Index].Y, Out.Triangles[Index].Z);
				Out.Normals[Index] = -Out.Normals[Index];
			}
			Volume = -Volume;
		}
		Out.Volume = static_cast<float>(Volume);
		Out.Centroid = Volume > 0.0 ? FVector3f(Moment / Volume) : (Out.BoundsMin + Out.BoundsMax) * 0.5f;
		const FVector3f Extent = Out.BoundsMax - Out.BoundsMin;
		Out.Length = FMath::Max(Extent.X, Extent.Y);
		Out.Source = FString::Printf(TEXT("%d convex, %d box, %d sphere, %d capsule; even shell of %d triangles at %.0f cm"), Convexes, Geom.BoxElems.Num(),
			Geom.SphereElems.Num(), Geom.SphylElems.Num(), Out.Triangles.Num(), Resolution);
		if (!Out.IsValid())
		{
			OutWhy = TEXT("the collision encloses no volume");
			return false;
		}
		// Pieces that cross each other: the shell encloses the shared volume twice, and nothing here takes it out (the geometry is left as
		// it is). Measured and said once per hull, in the hull's source line and as a warning naming the pieces.
		if (Probes.Num() > 1)
		{
			FString Pairs;
			const float Shared = OverlappingPieces(Probes, Out.Volume, Pairs);
			if (!Pairs.IsEmpty())
			{
				Out.Source += FString::Printf(TEXT(" (pieces overlap: %.0f%% of the volume counts twice)"), Shared * 100.f);
				const AActor* Owner = Primitive.GetOwner();
				const FString Who = Owner ? FString::Printf(TEXT("%s (%s)"), *Owner->GetActorNameOrLabel(), *Primitive.GetName()) : Primitive.GetPathName();
				const FString Message = FString::Printf(TEXT("%s: its collision pieces overlap, %s: about %.0f%% of the hull's volume is enclosed twice, so the water lifts and drags that part as if it were two bodies. Shape the pieces to meet, not to cross."),
					*Who, *Pairs, Shared * 100.f);
				// The same collision is built into several levels of detail, and registered again on every Play: one warning per hull.
				static FCriticalSection WarnedLock;
				static TSet<FString> Warned;
				bool bWarned = false;
				{
					FScopeLock Lock(&WarnedLock);
					Warned.Add(Message, &bWarned);
				}
				if (!bWarned)
				{
					UE_LOG(LogFluidNinjaBuoyancy, Warning, TEXT("%s"), *Message);
				}
			}
		}
		return true;
	}


	namespace
	{
		/** Solid-angle winding number of a closed triangle shell about a point (about 1 inside, 0 outside; overlapping pieces add). */
		float Winding(const FHull& Shell, const FVector3f& Point)
		{
			double Total = 0.0;
			for (const FIntVector3& Tri : Shell.Triangles)
			{
				const FVector3d A = FVector3d(Shell.Vertices[Tri.X] - Point);
				const FVector3d B = FVector3d(Shell.Vertices[Tri.Y] - Point);
				const FVector3d C = FVector3d(Shell.Vertices[Tri.Z] - Point);
				const double LA = A.Size();
				const double LB = B.Size();
				const double LC = C.Size();
				const double Numerator = FVector3d::DotProduct(A, FVector3d::CrossProduct(B, C));
				const double Denominator = LA * LB * LC + FVector3d::DotProduct(A, B) * LC + FVector3d::DotProduct(A, C) * LB + FVector3d::DotProduct(B, C) * LA;
				Total += 2.0 * FMath::Atan2(Numerator, Denominator);
			}
			return static_cast<float>(FMath::Abs(Total) / (4.0 * UE_DOUBLE_PI));
		}

		void CopyFrame(const FHull& Surface, FHull& Out)
		{
			Out.Vertices = Surface.Vertices;
			Out.BoundsMin = Surface.BoundsMin;
			Out.BoundsMax = Surface.BoundsMax;
			Out.Length = Surface.Length;
			Out.TotalArea = Surface.TotalArea;
			Out.Volume = Surface.Volume;
			Out.Centroid = Surface.Centroid;
		}
	}

	bool BuildVoxels(const FHull& Surface, float VoxelSizeCm, int32 TargetCount, FHull& Out, FString& OutWhy)
	{
		Out = FHull();
		if (!Surface.IsValid() || Surface.Model != EModel::Triangles)
		{
			OutWhy = TEXT("no hull surface to fill");
			return false;
		}
		CopyFrame(Surface, Out);
		Out.Model = EModel::Voxels;
		float Size = VoxelSizeCm > 0.f ? VoxelSizeCm : FMath::Pow(Surface.Volume / FMath::Max(TargetCount, 1), 1.f / 3.f);
		const FVector3f Extent = Surface.BoundsMax - Surface.BoundsMin;
		for (int32 Attempt = 0; Attempt < 6 && Out.Points.IsEmpty(); ++Attempt, Size *= 0.7f)
		{
			const FIntVector Cells(FMath::Max(1, FMath::RoundToInt32(Extent.X / Size)), FMath::Max(1, FMath::RoundToInt32(Extent.Y / Size)),
				FMath::Max(1, FMath::RoundToInt32(Extent.Z / Size)));
			const FVector3f Step(Extent.X / Cells.X, Extent.Y / Cells.Y, Extent.Z / Cells.Z);
			for (int32 X = 0; X < Cells.X; ++X)
			{
				for (int32 Y = 0; Y < Cells.Y; ++Y)
				{
					for (int32 Z = 0; Z < Cells.Z; ++Z)
					{
						const FVector3f Centre = Surface.BoundsMin + FVector3f((X + 0.5f) * Step.X, (Y + 0.5f) * Step.Y, (Z + 0.5f) * Step.Z);
						if (Winding(Surface, Centre) >= 0.5f)
						{
							Out.Points.Add(Centre);
							Out.PointSizes.Add(Step.Z);
						}
					}
				}
			}
		}
		if (Out.Points.IsEmpty())
		{
			OutWhy = TEXT("the hull is too thin to hold any voxel");
			return false;
		}
		const float Each = Surface.Volume / Out.Points.Num();
		Out.PointVolumes.Init(Each, Out.Points.Num());
		Out.PointLift.Init(1.f, Out.Points.Num());
		Out.PointDrag.Init(1.f, Out.Points.Num());
		Out.PointSectionScale = FMath::Clamp(Surface.TotalArea * 0.25f / (Out.Points.Num() * FMath::Square(Out.PointSizes[0])), 0.05f, 2.f);
		Out.Source = Surface.Source + FString::Printf(TEXT("; %d voxels of %.0f cm"), Out.Points.Num(), Out.PointSizes[0]);
		return true;
	}

	bool BuildSpheres(const FHull& Surface, const TArray<FPontoon>& Pontoons, FHull& Out, FString& OutWhy)
	{
		Out = FHull();
		if (!Surface.IsValid())
		{
			OutWhy = TEXT("no hull to place pontoons on");
			return false;
		}
		CopyFrame(Surface, Out);
		Out.Model = EModel::Spheres;
		if (!Pontoons.IsEmpty())
		{
			for (const FPontoon& Pontoon : Pontoons)
			{
				Out.Points.Add(FVector3f(Pontoon.Location));
				Out.PointSizes.Add(FMath::Max(Pontoon.Radius, 1.f));
				Out.PointLift.Add(Pontoon.LiftWeight);
				Out.PointDrag.Add(Pontoon.DragWeight);
			}
			Out.Source = Surface.Source + FString::Printf(TEXT("; %d pontoons (set by hand)"), Out.Points.Num());
		}
		else
		{
			// Two rows along the bottom, two to four per side, lifting together what the whole hull would.
			const FVector3f Extent = Surface.BoundsMax - Surface.BoundsMin;
			const float Radius = FMath::Max(1.f, FMath::Min(Extent.Y * 0.25f, Extent.Z * 0.5f));
			const int32 PerSide = FMath::Clamp(FMath::RoundToInt32(Extent.X / FMath::Max(Extent.Y, 1.f) * 2.f), 2, 4);
			double SphereVolume = 0.0;
			for (int32 Side = 0; Side < 2; ++Side)
			{
				for (int32 Index = 0; Index < PerSide; ++Index)
				{
					const float X = Surface.BoundsMin.X + Radius + (Extent.X - 2.f * Radius) * (PerSide > 1 ? static_cast<float>(Index) / (PerSide - 1) : 0.5f);
					const float Y = Side ? Surface.BoundsMax.Y - Radius : Surface.BoundsMin.Y + Radius;
					Out.Points.Add(FVector3f(X, Y, Surface.BoundsMin.Z + Radius));
					Out.PointSizes.Add(Radius);
					Out.PointDrag.Add(1.f);
					SphereVolume += 4.0 / 3.0 * UE_DOUBLE_PI * Radius * Radius * Radius;
				}
			}
			Out.PointLift.Init(static_cast<float>(Surface.Volume / FMath::Max(SphereVolume, 1.0)), Out.Points.Num());
			Out.Source = Surface.Source + FString::Printf(TEXT("; %d automatic pontoons of %.0f cm (lift x%.2f)"), Out.Points.Num(), Radius, Out.PointLift[0]);
		}
		Out.PointVolumes.Init(0.f, Out.Points.Num());
		double Sections = 0.0;
		for (const float Radius : Out.PointSizes)
		{
			Sections += UE_DOUBLE_PI * Radius * Radius;
		}
		Out.PointSectionScale = static_cast<float>(FMath::Clamp(Surface.TotalArea * 0.25 / FMath::Max(Sections, 1.0), 0.05, 2.0));
		return true;
	}

	namespace
	{
		/** A piece of water line (centre, length, the hull's outward normal there): into the body's sector table and the wash. */
		void AddWaterline(FStepResult& Out, const FHull& Hull, const FBodyState& Body, const FVector& Mid, float Length, const FVector& Normal,
			const FVector& WaterVelocity, double& WashSum)
		{
			if (Length < 0.01f)
			{
				return;
			}
			const FVector HullVelocity = Body.Velocity + FVector::CrossProduct(Body.AngularVelocity, Mid - Body.CenterOfMass);
			FVector Relative = HullVelocity - WaterVelocity;
			// Sinking, the hull displaces water through its water plane, and that water leaves outward across the water line: for a
			// hull of water plane A and water line L it spreads over a layer about A / L deep, so it runs out at about the sinking speed.
			const float Plunge = static_cast<float>(FMath::Max(0.0, -Relative.Z));
			Relative.Z = 0.0;
			Out.WaterlineLength += Length;
			WashSum += Length * Relative.Size();
			FVector Outward(Normal.X, Normal.Y, 0.0);
			const double Horizontal = Outward.Size();
			if (Horizontal < 0.2)
			{
				// A flat bottom or deck piece (or a point on the hull's axis) has no side to push with. Sinking, it still displaces water,
				// and that water leaves outward from the hull: taken as running from the hull's centre out through the piece, a flat-bottomed
				// drop sends its ring out too.
				if (Plunge <= 0.f)
				{
					return;
				}
				Outward = Mid - Body.Transform.TransformPosition(FVector(Hull.Centroid));
				Outward.Z = 0.0;
				const double Away = Outward.Size();
				if (Away < 1.0)
				{
					return;   // on the hull's axis: no way out to choose
				}
				Outward /= Away;
			}
			else
			{
				Outward /= Horizontal;
			}
			const float Push = static_cast<float>(FVector::DotProduct(Relative, Outward));
			const FVector Local = Body.Transform.InverseTransformPosition(Mid) - FVector(Hull.Centroid);
			const float Angle = static_cast<float>(FMath::Atan2(Local.Y, Local.X));
			const int32 Sector = FMath::Clamp(FMath::FloorToInt32((Angle + PI) / (2.f * PI) * WaterlineSectors), 0, WaterlineSectors - 1);
			FWaterlineSample& Sample = Out.Sectors[Sector];
			if (!Sample.bValid || Push + Plunge > Sample.Push + Sample.Plunge)
			{
				Sample.Position = Mid;
				Sample.HullVelocity = HullVelocity;
				Sample.Outward = Outward;
				Sample.Push = Push;
				Sample.Plunge = Plunge;
				Sample.bValid = true;
			}
		}

		void ResetWaterline(FStepResult& Out)
		{
			for (FWaterlineSample& Sample : Out.Sectors)
			{
				Sample = FWaterlineSample();
			}
			Out.WaterlineLength = 0.f;
			Out.Wash = 0.f;
		}
	}

	namespace
	{
		/** A piece slams only driving into the water at least this fast (cm/s): an entry, not the wetting of a body bobbing on waves (a
		 * few cm/s). Even a drop from 30 cm enters at 240 cm/s. Voxels, spheres and hull triangles alike (the triangles used to slam on
		 * any inward speed). */
		constexpr float PointSlamMinSpeed = 50.f;

		/** Ripples: the list a thread gathers the wet pieces' impulses into, moved out whole into the step's result once the pieces are
		 * done (no copy), and the count it last held, so the next step reserves it in one allocation (the move takes the memory with it). */
		thread_local TArray<FVector4f> RippleScratch;
		thread_local int32 RippleScratchLast = 0;

		/** A planing hull's pressure distribution for its wave-making resistance: (x and y about the centre of mass, the piece's steady
		 * vertical load, its spread σ), gathered over the wet pieces of the steps that evaluate it. */
		thread_local TArray<FVector4f> WaveLoadScratch;

		/** The wave-making resistance is evaluated every WaveResistanceInterval seconds (its loads gathered only on those steps) and
		 * low-passed over WaveResistanceSmoothSeconds: it changes with the speed and the trim, not from step to step, and a chop must not
		 * turn into resistance spikes. Every 0.1 s is well inside the low-pass; it was every 1/30 s, three times the cost for nothing. */
		constexpr float WaveResistanceInterval = 0.1f;
		constexpr float WaveResistanceSmoothSeconds = 0.25f;

		/** WaveMakingResistance's pressure merged into cells: per cell the summed load, the summed |load| and the |load|-weighted x and
		 * y, the summed σ² (then σ), and the cell's spread group. */
		struct FWaveCell
		{
			double Load = 0.0;
			double Weight = 0.0;
			double X = 0.0;
			double Y = 0.0;
			double Spread = 0.0;
			int32 Group = 0;
		};
		thread_local TMap<FIntPoint, int32> WaveCellOf;
		thread_local TArray<FWaveCell> WaveCells;
		thread_local TArray<FVector4f> WaveCellLoads;
		/** Loads under this share of the largest are left out of the wave-making resistance (a hull's sides and transom carry none). */
		constexpr float WaveLoadSkipShare = 1e-3f;
		/** The merged cells' spread filter, exp(-k² σ² / 2), is taken once per group of about the same σ: σ rounded to steps of the largest
		 * σ / (WaveSpreadGroups - 1), so within 1/30 of it. */
		constexpr int32 WaveSpreadGroups = 16;

		/**
		 * A wetted transom. In the linear theory the hydrostatic pressure ending at a transom is a step, and a step radiates a wave whose
		 * resistance, about ½ ρ g T² B (T the transom's draft, B its beam), does not fall with the speed: a floor from the slowest speed the
		 * hull's pieces resolve, √(g × piece / π) (88 cm/s at 25 cm pieces), upward, so where it began depended on the tessellation. A
		 * real transom runs wetted below a transom Froude number U / √(g T) of about 2.5: the water closes up behind it and makes no such
		 * wave. So below TransomWetFroude the wave-making resistance's loads fade in from the trailing edge (the hull's aftmost wet
		 * vertex along its way through the water) over TransomFadeDrafts × T (T the deepest wet vertex within one hull piece of that
		 * edge), and between TransomWetFroude and TransomDryFroude the edge sharpens back to the step a dry transom really has. Only
		 * those loads are faded; the hull's lift and buoyancy are not touched. A 5 m, 800 kg V-bottom runabout (25 cm draft at its
		 * transom, 25 cm pieces): 380-420 N at 0.9-1.5 m/s before, 1-17 N after; its hump (about 690 N near Fn_L 0.55) kept, and 12.5
		 * and 25 cm pieces within about 3 % of each other from 3 m/s up, as before.
		 */
		constexpr float TransomFadeDrafts = 6.f;
		constexpr float TransomWetFroude = 2.f;
		constexpr float TransomDryFroude = 3.f;

		/** Fades the wave-making resistance's loads (x along the motion, about Centre) in from a wetted transom (TransomFadeDrafts):
		 * Points and PointDepths are the hull's vertices this step (world) and their depths (above 0 wet), Along the motion's direction
		 * (unit, level), Speed its speed through the water (cm/s), Band one hull piece (cm). */
		void FadeWettedTransom(TArray<FVector4f>& Loads, TArrayView<const FVector> Points, TArrayView<const float> PointDepths, const FVector& Centre,
			const FVector& Along, float Speed, float Gravity, float Band)
		{
			const int32 NumPoints = FMath::Min(Points.Num(), PointDepths.Num());
			double Aft = TNumericLimits<double>::Max();
			for (int32 Index = 0; Index < NumPoints; ++Index)
			{
				if (PointDepths[Index] > 0.f)
				{
					Aft = FMath::Min(Aft, FVector::DotProduct(Points[Index] - Centre, Along));
				}
			}
			float Draft = 0.f;
			for (int32 Index = 0; Index < NumPoints; ++Index)
			{
				if (PointDepths[Index] > Draft && FVector::DotProduct(Points[Index] - Centre, Along) <= Aft + Band)
				{
					Draft = PointDepths[Index];
				}
			}
			if (!(Draft > 0.f) || !(Gravity > 0.f))
			{
				return;
			}
			const float Sharp = FMath::Clamp((Speed / FMath::Sqrt(Gravity * Draft) - TransomWetFroude) / (TransomDryFroude - TransomWetFroude), 0.f, 1.f);
			if (Sharp >= 1.f)
			{
				return;
			}
			const float Edge = static_cast<float>(Aft);
			const float InverseLength = 1.f / (TransomFadeDrafts * Draft);
			for (FVector4f& Load : Loads)
			{
				const float Weight = FMath::Clamp((Load.X - Edge) * InverseLength, 0.f, 1.f);
				Load.Z *= Weight + (1.f - Weight) * Sharp;
			}
		}

		void TakeRipples(TArray<FVector4f>& Scratch, FStepResult& Out)
		{
			RippleScratchLast = Scratch.Num();
			Out.RippleImpulses = MoveTemp(Scratch);
		}

		/** Stable steps: a piece's drag, gaining Gain (kg/s) per cm/s along Direction (unit), at Arm from the centre of mass. */
		void AddDragDamping(FStepResult& Out, float Gain, const FVector& Arm, const FVector& Direction)
		{
			Out.DragDamping += FVector(Direction.X * Direction.X, Direction.Y * Direction.Y, Direction.Z * Direction.Z) * Gain;
			const FVector Lever = FVector::CrossProduct(Arm, Direction);
			Out.SpinDamping += FVector(Lever.X * Lever.X, Lever.Y * Lever.Y, Lever.Z * Lever.Z) * Gain;
		}

		/** The same for a drag that follows the water moving past, whichever way that is (a voxel's, a pontoon's). */
		void AddDragDampingAnyWay(FStepResult& Out, float Gain, const FVector& Arm)
		{
			Out.DragDamping += FVector(Gain);
			const FVector Square(Arm.X * Arm.X, Arm.Y * Arm.Y, Arm.Z * Arm.Z);
			Out.SpinDamping += FVector(Square.Y + Square.Z, Square.X + Square.Z, Square.X + Square.Y) * Gain;
		}

		/** Stable steps: a piece of waterplane at Arm from the centre of mass, holding the body by Stiffness (ρ g × its area, kg/s²;
		 * negative for a wet face looking up, so the sum over a hull is its waterplane). */
		void AddWaterplane(FStepResult& Out, float Stiffness, const FVector& Arm)
		{
			Out.HeaveStiffness += Stiffness;
			Out.TiltStiffness.X += Stiffness * Arm.Y * Arm.Y;
			Out.TiltStiffness.Y += Stiffness * Arm.X * Arm.X;
		}

		/** A hull piece (a triangle, a voxel, a sphere) driving into the water this step, gathered for the body's slam (ResolveSlam): its
		 * wetted surface's growth (cm²/s), its speed into the water (cm/s) along Push (unit, the way the water pushes it back), its whole
		 * surface (cm², for the stop force's share), its centre (world) and where it meets the water; and, for the ripples, up to two
		 * pieces of it seen from above (world XY, area, and the share of the piece's slam that reaches the surface there). */
		struct FSlamPiece
		{
			float Growth = 0.f;
			float Speed = 0.f;
			float Surface = 0.f;
			FVector Centre = FVector::ZeroVector;
			FVector Push = FVector::UpVector;
			FVector Location = FVector::ZeroVector;
			int32 NumRipples = 0;
			float RippleX[2] = {};
			float RippleY[2] = {};
			float RippleArea[2] = {};
			float RippleShare[2] = {};
		};
		thread_local TArray<FSlamPiece> SlamScratch;

		/** The hull's beam (cm): the middle one of its three extents in the body frame (a log's or a boat's width, a crate's side, a
		 * ball's diameter), the length the body's slam scales with whatever its rest attitude. */
		float SlamLength(const FHull& Hull)
		{
			const FVector3f Extent = Hull.BoundsMax - Hull.BoundsMin;
			return FMath::Max(FMath::Max(FMath::Min(Extent.X, Extent.Y), FMath::Min(FMath::Max(Extent.X, Extent.Y), Extent.Z)), 1.f);
		}

		/**
		 * The body's slam this step (von Kármán/Wagner), from the pieces driving into the water. A hull section of wetted beam b going in
		 * at v carries the added-mass momentum of the water under it, ρ π b² / 8 per unit length for a flat section, and the slam is its
		 * rate of change, (π / 4) ρ b ḃ v per unit length: ρ v × the growth of the wetted area × the wetted beam. Taken piece by piece with
		 * each piece's own √area for the beam it depended on how finely the hull was cut (the same log slammed at 6.5 kW on its finest
		 * hull and 68 kW on its coarsest), so the growth is summed over the body's pieces (the sum is the hull's, however it is cut) and the
		 * body's slam is Slam Multiplier × ρ × Σ growth_i v_i × the hull's beam, capped at the stop force, the body's momentum per step
		 * M v̄ / dt (v̄ the growth-weighted inward speed) times the share of its surface going in (twice the share, as each piece's cap
		 * was; at most all of it). It is handed back to the pieces by their share of Σ growth_i v_i, the piece's share of the added-mass
		 * momentum (at one speed, by growth; a piece going in faster takes more, as it would on its own), so where it hits, its torque
		 * and what it presses into the water stay per piece. Each piece's force over its speed, F_i / v_i, is the slam's derivative with
		 * respect to the inward speed (the growth taken as this step's), and goes into the drag and spin damping (AddDragDamping) so
		 * stable steps take implicitly the slam that would stop the body in less than a step. Out.SlamPower is the body's whole slam,
		 * Σ F_i v_i; Out.SlamLocation where the piece taking the largest share meets the water.
		 */
		void ResolveSlam(const FBodyState& Body, const FParams& P, float DeltaSeconds, float TotalSurface, float Length, const TArray<FSlamPiece>& Pieces,
			FVector& Force, FVector& Torque, FStepResult& Out, TArray<FVector4f>* Ripples)
		{
			if (Pieces.IsEmpty() || DeltaSeconds <= 0.f)
			{
				return;
			}
			double Growth = 0.0;     // Σ growth_i, cm²/s
			double Momentum = 0.0;   // Σ growth_i v_i, cm³/s²
			double Surface = 0.0;
			for (const FSlamPiece& Piece : Pieces)
			{
				Growth += Piece.Growth;
				Momentum += static_cast<double>(Piece.Growth) * Piece.Speed;
				Surface += Piece.Surface;
			}
			if (Growth <= 0.0 || Momentum <= 0.0)
			{
				return;
			}
			const double MeanSpeed = Momentum / Growth;
			const double Stop = static_cast<double>(Body.Mass) * MeanSpeed / DeltaSeconds * FMath::Min(2.0 * Surface / FMath::Max(static_cast<double>(TotalSurface), 1.0), 1.0);
			const double Slam = FMath::Min(static_cast<double>(P.SlamMultiplier) * P.Rho * Momentum * Length, Stop);
			if (Slam <= 0.0)
			{
				return;
			}
			double Power = 0.0;
			double Largest = 0.0;
			for (const FSlamPiece& Piece : Pieces)
			{
				const double Share = static_cast<double>(Piece.Growth) * Piece.Speed / Momentum;
				const double PieceForce = Slam * Share;
				const FVector SlamForce = Piece.Push * PieceForce;
				Force += SlamForce;
				Torque += FVector::CrossProduct(Piece.Centre - Body.CenterOfMass, SlamForce);
				AddDragDamping(Out, static_cast<float>(PieceForce / Piece.Speed), Piece.Centre - Body.CenterOfMass, Piece.Push);
				Power += PieceForce * Piece.Speed;
				if (PieceForce > Largest)
				{
					Largest = PieceForce;
					Out.SlamLocation = Piece.Location;
				}
				// Ripples: the slam's upward part on the piece × the step is the impulse it presses down into the water, as much of it as
				// reaches the surface from each of its pieces seen from above.
				if (Ripples)
				{
					const float SlamZ = static_cast<float>(SlamForce.Z);
					for (int32 RippleIndex = 0; RippleIndex < Piece.NumRipples; ++RippleIndex)
					{
						const float Impulse = SlamZ * Piece.RippleShare[RippleIndex] * DeltaSeconds;
						if (Impulse != 0.f)
						{
							Ripples->Add(FVector4f(Piece.RippleX[RippleIndex], Piece.RippleY[RippleIndex], Impulse, Piece.RippleArea[RippleIndex]));
						}
					}
				}
			}
			Out.SlamPower = static_cast<float>(Power);
		}

		/** Below this the propeller's shaft is taken as stopped (rev/s): the advance ratio is not formed. */
		constexpr float PropStoppedRevs = 0.05f;
		/** A disc less than this much under the water (share of its area) ventilates while it is loaded and turning faster than
		 * PropVentilateRevs; it stays ventilated until it has been PropReattachImmersion under for PropReattachSeconds. */
		constexpr float PropVentilateImmersion = 0.8f;
		constexpr float PropVentilateRevs = 5.f;
		constexpr float PropReattachImmersion = 0.98f;
		constexpr float PropReattachSeconds = 0.3f;
		/** The slipping clutch's capacity at the least throttle: the governor's share of the rated torque that holds idle (a crash
		 * shift with the throttle barely open still turns the propeller round, slowly). */
		constexpr float ClutchIdleShare = 0.3f;

		/** The open-water table at advance ratio J (forward running), linear between entries and extrapolated past the last. */
		float PropTable(const float* Values, float Step, float J)
		{
			const float T = FMath::Max(J, 0.f) / FMath::Max(Step, 1e-3f);
			const int32 Index = FMath::Clamp(FMath::FloorToInt32(T), 0, PropTableSize - 2);
			return Values[Index] + (Values[Index + 1] - Values[Index]) * (T - static_cast<float>(Index));
		}

		/**
		 * The propeller model (EPropulsionModel::Propeller), one step. The engine's torque goes through the gearbox to the shaft, which
		 * the water's torque loads (ρ n² D⁵ KQ); the shaft's speed is stepped semi-implicitly (the load's slope taken implicitly, so a light
		 * shaft stays stable at any step). The thrust ρ n² D⁴ KT pushes at the disc along its axis (turned with the rudder for an outboard
		 * or a sterndrive); both scale with the disc's immersion, and drop to the ventilated share when air is drawn down onto the blades.
		 * The water's torque reacts on the hull about the shaft. Returns the jet's excess speed over the disc (cm/s), the wash the rudder
		 * gets ahead (none astern: the jet then leaves the rudder).
		 */
		float Propel(const FBodyState& Body, const FWaterView& Water, const FParams& P, float DeltaSeconds, FStepState& State, FVector& Force, FVector& Torque,
			FStepResult& Out)
		{
			const FTransform& Transform = Body.Transform;
			const FVector& Com = Body.CenterOfMass;
			const FVector Prop = Transform.TransformPosition(P.ThrustLocal);
			FVector Axis = Transform.TransformVectorNoScale(P.ThrustDirectionLocal.GetSafeNormal());
			if (P.bSteerableThrust && P.RudderAngle != 0.f)
			{
				// Positive rudder turns the bow to +Y: the drive swings its thrust to -Y at the stern.
				Axis = FQuat(Transform.GetUnitAxis(EAxis::Z), -P.RudderAngle).RotateVector(Axis);
			}
			const float Diameter = FMath::Max(P.PropDiameter, 1.f);
			const float Radius = 0.5f * Diameter;

			// The disc in the water: the share of its area under the surface over its centre (a disc whose axis lies about level).
			float Height = 0.f;
			FVector WaterVelocity = FVector::ZeroVector;
			float Immersion = 0.f;
			if (Water.Sample(Prop, Height, WaterVelocity))
			{
				const float X = FMath::Clamp((Height - static_cast<float>(Prop.Z)) / Radius, -1.f, 1.f);
				Immersion = (FMath::Acos(-X) + X * FMath::Sqrt(FMath::Max(1.f - X * X, 0.f))) / PI;
			}
			const FVector Water2D(WaterVelocity.X * P.FlowCoupling, WaterVelocity.Y * P.FlowCoupling, 0.0);
			const FVector Relative = Body.Velocity + FVector::CrossProduct(Body.AngularVelocity, Prop - Com) - Water2D;
			const float Advance = static_cast<float>(FVector::DotProduct(Relative, Axis)) * (1.f - FMath::Clamp(P.PropWakeFraction, 0.f, 0.9f));

			// The command: the throttle's sign picks the gear, its size the engine's throttle, which the engine catches up with.
			const float Command = FMath::Clamp(P.Throttle, -1.f, 1.f);
			const int8 Gear = Command > 0.02f ? 1 : Command < -0.02f ? -1 : 0;
			const int8 PreviousGear = State.Gear;   // read before it is written: the clutch engages on a change
			State.Gear = Gear;
			State.EngineThrottle += (FMath::Abs(Command) - State.EngineThrottle) * (1.f - FMath::Exp(-DeltaSeconds / FMath::Max(P.EngineResponseSeconds, 0.01f)));
			State.EngineThrottle = FMath::Clamp(State.EngineThrottle, 0.f, 1.f);

			// Ventilation: a loaded disc breaking the surface draws air down onto its blades; it holds on until the disc is well under again.
			if (!State.bVentilated)
			{
				if (Gear != 0 && Immersion < PropVentilateImmersion && FMath::Abs(State.ShaftOmega) / (2.f * PI) > PropVentilateRevs)
				{
					State.bVentilated = true;
					State.ImmersedSeconds = 0.f;
				}
			}
			else
			{
				State.ImmersedSeconds = Immersion >= PropReattachImmersion ? State.ImmersedSeconds + DeltaSeconds : 0.f;
				if (State.ImmersedSeconds >= PropReattachSeconds)
				{
					State.bVentilated = false;
				}
			}
			const float Wet = Immersion * (State.bVentilated ? FMath::Clamp(P.VentilatedFactor, 0.f, 1.f) : 1.f);

			// KT and KQ at a shaft speed (rad/s, signed): forward running ahead, the same curves at the reverse efficiency astern, the
			// advance ratio taken in the direction the disc pushes from; against the flow (J below 0) the blades bite harder.
			const auto Coefficients = [&P, Advance, Diameter](float Omega, float& KT, float& KQ)
			{
				const float Revs = FMath::Abs(Omega) / (2.f * PI);
				const bool bAstern = Omega < 0.f;
				const float Into = bAstern ? -Advance : Advance;
				const float J = Revs > PropStoppedRevs ? FMath::Clamp(Into / (Revs * Diameter), -2.f, 3.f * P.PropTableStep * (PropTableSize - 1)) : 0.f;
				KT = PropTable(P.PropKT, P.PropTableStep, J);
				KQ = PropTable(P.PropKQ, P.PropTableStep, J);
				if (J < 0.f)
				{
					const float Bite = 1.f + 0.5f * FMath::Min(-J, 1.f);
					KT = P.PropKT[0] * Bite;
					KQ = P.PropKQ[0] * Bite;
				}
				if (bAstern)
				{
					const float Reverse = FMath::Clamp(P.ReverseEfficiency, 0.f, 1.f);
					KT *= Reverse;
					KQ *= Reverse;
				}
			};
			// The torque the water takes from the shaft (signed like Omega) and the thrust it gives (along Axis, negative astern).
			const float D4 = Diameter * Diameter * Diameter * Diameter;
			const auto WaterTorque = [&](float Omega)
			{
				float KT = 0.f;
				float KQ = 0.f;
				Coefficients(Omega, KT, KQ);
				const float Revs = Omega / (2.f * PI);
				return P.Rho * Revs * FMath::Abs(Revs) * D4 * Diameter * KQ * Wet;
			};

			// The engine: the throttle's share of a torque curve that peaks low and gives the rated power at the rev limit, a governor
			// holding idle, its own friction, and the rev limiter (no torque from 4 % over the limit).
			const float RatedOmega = FMath::Max(P.EngineMaxRpm, 100.f) * (2.f * PI / 60.f);
			const float IdleOmega = FMath::Clamp(P.EngineIdleRpm, 0.f, 0.9f * P.EngineMaxRpm) * (2.f * PI / 60.f);
			const float RatedTorque = FMath::Max(P.EnginePower, 0.f) / RatedOmega;
			const auto EngineTorque = [&](float Omega)
			{
				const float Rate = FMath::Max(Omega, 0.f) / RatedOmega;
				float Engine = State.EngineThrottle * RatedTorque * (1.25f - 0.25f * Rate * Rate) * FMath::Clamp((1.04f - Rate) / 0.04f, 0.f, 1.f);
				Engine += RatedTorque * 0.3f * FMath::Clamp((IdleOmega - Omega) / FMath::Max(0.2f * IdleOmega, 1.f), 0.f, 1.f);
				return Engine - RatedTorque * 0.04f * Rate;
			};
			// What the engine could give at a speed at full throttle: the clutch's capacity while it slips, and the load's reference.
			const auto FullThrottleTorque = [&](float Speed)
			{
				const float Rate = FMath::Max(Speed, 0.f) / RatedOmega;
				return RatedTorque * FMath::Max(1.25f - 0.25f * Rate * Rate, 0.2f);
			};
			const float Ratio = FMath::Max(P.GearRatio, 0.1f);
			const float Efficiency = FMath::Clamp(P.GearEfficiency, 0.1f, 1.f);
			// The clutch. In gear the engine (I_e, kg·cm², turning one way only) is locked to the shaft through the gear: it turns at
			// G ω R (G the gear's sign, ω the shaft's speed, R the ratio) and adds I_e R² to the shaft's inertia. Going into gear while the
			// shaft stands still or already turns the gear's way, the two lock at once sharing their angular momentum (about the shaft, the
			// engine's I_e R² at G ω_e / R): ω = (I_e R G ω_e + I_p ω) / (I_e R² + I_p). Setting the engine to the shaft's speed instead
			// dropped it to about 0 rpm on every engagement from rest. Going into gear while the shaft still turns against it (a crash
			// reverse, or reverse put in with the propeller windmilling ahead), or when the water drives a locked shaft backwards, the
			// engine cannot follow it below 0: the clutch slips. It passes at most what the engine could give at its own speed at the
			// throttle it has reached (its full-throttle torque times that throttle, never less than the governor's idle share, so a light
			// touch astern at speed does not kick like full reverse) through the gear to the propeller alone (I_p), and loads the engine
			// with it: the engine keeps its own
			// speed, bogging where the throttle gives less than that, the governor keeping it at idle at least, while the propeller is
			// turned against the water, through 0, until it meets the engine's speed G ω_e / R; there the two lock. Under a heavy load at
			// full throttle (a crash reverse at speed) the propeller can stay slower than the engine for a second or more, the engine
			// sagging slowly, until the boat has slowed. Snapping the engine to the shaft read 0 rpm for about 0.4 s there while the
			// engine's full low-speed torque drove the propeller astern.
			const float Sense = static_cast<float>(Gear);
			const bool bAgainst = Gear != 0 && (Gear != PreviousGear ? Sense * State.ShaftOmega < 0.f : State.bClutchSlipping);
			const bool bSlipping = bAgainst && Sense * State.ShaftOmega < State.EngineOmega / Ratio;
			const float SlipLoad = bSlipping ? FullThrottleTorque(State.EngineOmega) * FMath::Clamp(State.EngineThrottle, ClutchIdleShare, 1.f) : 0.f;   // at the engine
			const float SlipDrive = Sense * SlipLoad * Ratio * Efficiency;                     // at the shaft
			// The shaft's net torque at Omega: the engine through the gear (in gear; while the clutch slips, what it passes) less the water's.
			const auto Net = [&](float Omega)
			{
				const float Drive = bSlipping ? SlipDrive : Gear != 0 ? Sense * EngineTorque(Sense * Omega * Ratio) * Ratio * Efficiency : 0.f;
				return Drive - WaterTorque(Omega);
			};
			const float LockedInertia = FMath::Max(P.EngineInertia * Ratio * Ratio + P.PropInertia, 1.f);
			const float Inertia = Gear != 0 && !bSlipping ? LockedInertia : FMath::Max(P.PropInertia, 1.f);
			// The shaft's speed once the engine (at EngineSpeed) and the propeller (at ShaftSpeed) lock in this gear.
			const auto LockedSpeed = [&P, Ratio, Sense, LockedInertia](float EngineSpeed, float ShaftSpeed)
			{
				return (P.EngineInertia * Ratio * Sense * EngineSpeed + P.PropInertia * ShaftSpeed) / LockedInertia;
			};
			float Omega = State.ShaftOmega;
			if (Gear != 0 && !bSlipping && (Gear != PreviousGear || State.bClutchSlipping))
			{
				Omega = LockedSpeed(State.EngineOmega, Omega);
			}
			if (DeltaSeconds > 0.f)
			{
				const float Probe = FMath::Max(1.f, 0.01f * FMath::Abs(Omega));
				const float Slope = (Net(Omega + Probe) - Net(Omega - Probe)) / (2.f * Probe);   // negative where the shaft settles
				Omega += DeltaSeconds * Net(Omega) / (Inertia + DeltaSeconds * FMath::Max(-Slope, 0.f));
				if (!FMath::IsFinite(Omega))
				{
					Omega = 0.f;
				}
				if (bSlipping)
				{
					// The engine, loaded by what the clutch passes (semi-implicit, as the shaft).
					const auto EngineNet = [&](float Speed)
					{
						return EngineTorque(Speed) - SlipLoad;
					};
					const float EngineProbe = FMath::Max(1.f, 0.01f * State.EngineOmega);
					const float EngineSlope = (EngineNet(State.EngineOmega + EngineProbe) - EngineNet(State.EngineOmega - EngineProbe)) / (2.f * EngineProbe);
					State.EngineOmega += DeltaSeconds * EngineNet(State.EngineOmega)
						/ (FMath::Max(P.EngineInertia, 1.f) + DeltaSeconds * FMath::Max(-EngineSlope, 0.f));
					State.EngineOmega = FMath::IsFinite(State.EngineOmega) ? FMath::Max(State.EngineOmega, IdleOmega) : IdleOmega;
				}
			}
			// The slip ends where the speeds meet: the two lock sharing their angular momentum (little is left to share by then).
			const bool bMet = bSlipping && Sense * Omega >= State.EngineOmega / Ratio;
			if (bMet)
			{
				Omega = LockedSpeed(State.EngineOmega, Omega);
			}
			if (Gear != 0 && (bSlipping ? bMet : Sense * Omega >= 0.f))
			{
				State.EngineOmega = Sense * Omega * Ratio;
				State.bClutchSlipping = false;
			}
			else if (bSlipping)
			{
				State.bClutchSlipping = true;   // the engine went its own way above
			}
			else
			{
				// Neutral (or a locked shaft the water drove backwards, which slips from the next step): the engine revs to the throttle,
				// free of the shaft.
				const float Target = IdleOmega + State.EngineThrottle * (RatedOmega - IdleOmega);
				State.EngineOmega += (Target - State.EngineOmega) * (1.f - FMath::Exp(-DeltaSeconds / 0.3f));
				State.bClutchSlipping = Gear != 0;
			}
			State.ShaftOmega = Omega;

			float KT = 0.f;
			float KQ = 0.f;
			Coefficients(Omega, KT, KQ);
			const float Revs = Omega / (2.f * PI);
			const float Thrust = P.Rho * Revs * FMath::Abs(Revs) * D4 * KT * Wet;
			const float ShaftTorque = WaterTorque(Omega);
			if (Thrust != 0.f)
			{
				Force += Axis * Thrust;
				Torque += FVector::CrossProduct(Prop - Com, Axis * Thrust);
			}
			// The water's torque on the blades reacts on the hull about the shaft: a right-handed propeller going ahead (clockwise from
			// astern) lists the boat to port.
			Torque += Axis * (FMath::Sign(P.PropHand) * ShaftTorque);
			Out.ThrustApplied = Thrust;

			// The slipstream (actuator disc): T = ½ ρ A (Vj² - Va²) over the wet part of the disc.
			const float DiscArea = 0.25f * PI * Diameter * Diameter * FMath::Max(Immersion, 0.1f);
			const float Jet = FMath::Sqrt(FMath::Max(Advance * Advance + 2.f * FMath::Abs(Thrust) / (P.Rho * DiscArea), 0.f));
			// What the engine could give at this rpm at full throttle (the load is the share of it the water takes).
			const float Available = FMath::Max(FullThrottleTorque(State.EngineOmega), 1e-3f);

			FPropulsorResult& Result = Out.Propulsor;
			Result.bActive = true;
			Result.Thrust = Thrust;
			Result.ShaftTorque = ShaftTorque;
			Result.ShaftRpm = Omega * (60.f / (2.f * PI));
			Result.EngineRpm = State.EngineOmega * (60.f / (2.f * PI));
			Result.Power = ShaftTorque * Omega;
			Result.Immersion = Immersion;
			Result.JetSpeed = Jet;
			Result.AdvanceSpeed = Advance;
			Result.Load = Gear != 0 ? FMath::Clamp(FMath::Abs(ShaftTorque) / (Ratio * Efficiency * Available), 0.f, 1.5f) : 0.f;
			Result.Gear = Gear;
			Result.bVentilated = State.bVentilated;
			Result.Location = Prop;
			Result.Axis = Axis;
			return Gear > 0 && Thrust > 0.f ? FMath::Max(Jet - FMath::Max(Advance, 0.f), 0.f) * Immersion : 0.f;
		}

		/** A boat's drift angle (FStepResult::DriftAngleDeg) is read only at least this fast through the water (cm/s): slower, the
		 * heading and the way through the water barely relate (a boat idling in a current). */
		constexpr float DriftMinSpeed = 50.f;

		/** Art-directed damping, propulsion, the keel, and the totals: shared by every model. */
		void FinishStep(const FBodyState& Body, const FWaterView& Water, const FParams& P, float DeltaSeconds, float WetFraction, FStepState& State, FVector& Force,
			FVector& Torque, FStepResult& Out)
		{
			const FTransform& Transform = Body.Transform;
			const FVector& Com = Body.CenterOfMass;
			const FVector& V = Body.Velocity;
			const FVector& W = Body.AngularVelocity;
			auto Apply = [&Force, &Torque, &Com](const FVector& F, const FVector& At)
			{
				Force += F;
				Torque += FVector::CrossProduct(At - Com, F);
			};

			// Extra damping per body axis, as much as the body is wet.
			if (WetFraction > 0.f && !P.AngularDamping.IsZero())
			{
				const FVector Local = Transform.InverseTransformVectorNoScale(W);
				Out.AngularDampingRate = Transform.TransformVectorNoScale(-(P.AngularDamping * Local)) * WetFraction;
			}

			// Propulsion: the propeller pushes only while it is in the water; the rudder lifts with the water flowing past it (the
			// boat's own speed plus the propeller's wash behind a fixed propeller; the boat's own speed alone on a steerable drive).
			if (P.bPropulsion)
			{
				const double ForceZBefore = Force.Z;
				float Height = 0.f;
				FVector WaterVelocity;
				float RudderWash = P.Throttle * P.PropWashSpeed;
				if (P.PropulsionModel == EPropulsionModel::Propeller)
				{
					RudderWash = Propel(Body, Water, P, DeltaSeconds, State, Force, Torque, Out);
				}
				else
				{
					const FVector Prop = Transform.TransformPosition(P.ThrustLocal);
					if (P.Throttle != 0.f && P.MaxThrust > 0.f && Water.Sample(Prop, Height, WaterVelocity) && Height > Prop.Z)
					{
						FVector Direction = Transform.TransformVectorNoScale(P.ThrustDirectionLocal.GetSafeNormal());
						if (P.bSteerableThrust && P.RudderAngle != 0.f)
						{
							Direction = FQuat(Transform.GetUnitAxis(EAxis::Z), -P.RudderAngle).RotateVector(Direction);   // an outboard swings its thrust
						}
						Out.ThrustApplied = P.Throttle * P.MaxThrust;
						Apply(Direction * Out.ThrustApplied, Prop);
					}
				}
				if (P.bSteerableThrust)
				{
					// An outboard's or a sterndrive's lower unit turns with its own propeller, so its jet meets it at no incidence: the jet's
					// side force is the swung thrust's T sin δ, already applied. The rudder (the lower unit's skeg) lifts on the boat's own
					// flow alone; adding the jet to it as well gave about three times the side force at low speed.
					RudderWash = 0.f;
				}
				const FVector Rudder = Transform.TransformPosition(P.RudderLocal);
				if (P.RudderArea > 0.f && P.RudderAngle != 0.f && Water.Sample(Rudder, Height, WaterVelocity) && Height > Rudder.Z)
				{
					const FVector Forward = Transform.GetUnitAxis(EAxis::X);
					const FVector Right = Transform.GetUnitAxis(EAxis::Y);
					const FVector Water2D(WaterVelocity.X * P.FlowCoupling, WaterVelocity.Y * P.FlowCoupling, 0.0);
					const FVector Relative = V + FVector::CrossProduct(W, Rudder - Com) - Water2D;
					const float U = static_cast<float>(FVector::DotProduct(Relative, Forward)) + RudderWash;
					const float Cl = FMath::Clamp(2.f * PI * P.RudderAngle, -1.2f, 1.2f);
					Apply(-Right * (0.5f * P.Rho * P.RudderArea * Cl * U * FMath::Abs(U)), Rudder);
				}
				Out.PropulsionForceZ = static_cast<float>(Force.Z - ForceZBefore);
			}

			// A boat's drift: its heading (level) against its way through the water at the centre of mass, for the readout.
			if (WetFraction > 0.f && (P.bPropulsion || P.KeelArea > 0.f))
			{
				float DriftHeight = 0.f;
				FVector DriftWater;
				if (Water.Sample(Com, DriftHeight, DriftWater))
				{
					const FVector Heading = Transform.GetUnitAxis(EAxis::X).GetSafeNormal2D();
					const FVector Way(V.X - DriftWater.X * P.FlowCoupling, V.Y - DriftWater.Y * P.FlowCoupling, 0.0);
					if (!Heading.IsZero() && Way.SizeSquared() >= FMath::Square(static_cast<double>(DriftMinSpeed)))
					{
						const double Drift = FMath::RadiansToDegrees(FMath::Atan2(FVector::CrossProduct(Heading, Way).Z, FVector::DotProduct(Heading, Way)));
						Out.DriftAngleDeg = FMath::IsFinite(Drift) ? static_cast<float>(Drift) : 0.f;
					}
				}
			}

			// The keel: the hull's lateral plane under the water (keel, skeg, pump housing, strakes), taken as a low-aspect-ratio wing at
			// the centre of lateral resistance. With the water moving past it at u along the hull and v across it (the body's velocity
			// there less the current, as at the rudder), it pushes back against the slide with the slender-body lift (π AR / 2) |u| v plus
			// the crossflow drag Cd v |v|, and the slide costs the induced drag (π AR / 4) v² against the way ahead, each times ½ρ × its
			// area × the share of its span under the water (AR = 2 × span² / area: the hull above mirrors it). A planing V-bottom's own
			// grip against the slide goes with sin² of its deadrise (about a tenth of the dynamic pressure at 18°), and the rudder's force
			// follows only its angle, not the slide: without a keel a planing hull skated outward through its turns. Any hull model and
			// flow, with or without propulsion.
			if (P.KeelArea > 0.f)
			{
				const FVector KeelPoint = Transform.TransformPosition(P.KeelLocal);
				float KeelHeight = 0.f;
				FVector KeelWater;
				if (Water.Sample(KeelPoint, KeelHeight, KeelWater))
				{
					const float Span = FMath::Max(P.KeelDepth, 1.f);
					const float KeelBottom = static_cast<float>(KeelPoint.Z) - 0.5f * Span;
					const float Immersion = FMath::Clamp((KeelHeight - KeelBottom) / Span, 0.f, 1.f);
					if (Immersion > 0.f)
					{
						const FVector Forward = Transform.GetUnitAxis(EAxis::X);
						const FVector Right = Transform.GetUnitAxis(EAxis::Y);
						const FVector Water2D(KeelWater.X * P.FlowCoupling, KeelWater.Y * P.FlowCoupling, 0.0);
						const FVector Relative = V + FVector::CrossProduct(W, KeelPoint - Com) - Water2D;
						const float U = static_cast<float>(FVector::DotProduct(Relative, Forward));
						const float Slide = static_cast<float>(FVector::DotProduct(Relative, Right));
						const float AspectRatio = 2.f * Span * Span / P.KeelArea;
						const float HalfRhoArea = 0.5f * P.Rho * P.KeelArea * Immersion;
						const float LiftSlope = 0.5f * PI * AspectRatio * FMath::Abs(U);   // the lift per cm/s of slide
						const float Cd = FMath::Max(P.KeelCrossflow, 0.f);
						const float SideForce = -HalfRhoArea * (LiftSlope * Slide + Cd * Slide * FMath::Abs(Slide));
						const float InducedDrag = -HalfRhoArea * 0.25f * PI * AspectRatio * Slide * Slide * FMath::Sign(U);
						if (FMath::IsFinite(SideForce) && FMath::IsFinite(InducedDrag))
						{
							const FVector KeelPush = Right * SideForce + Forward * InducedDrag;
							Apply(KeelPush, KeelPoint);
							Out.PropulsionForceZ += static_cast<float>(KeelPush.Z);   // a banked keel's push tilts; it makes no ripple pieces
							Out.KeelForce = FMath::Abs(SideForce);
							// Stable steps: the side force gains this much per cm/s of slide, so a light body with a big keel stays stable.
							AddDragDamping(Out, HalfRhoArea * (LiftSlope + 2.f * Cd * FMath::Abs(Slide)), KeelPoint - Com, Right);
						}
					}
				}
			}

			if (Force.ContainsNaN() || Torque.ContainsNaN())
			{
				Force = Torque = FVector::ZeroVector;
				Out.RippleImpulses.Reset();   // nothing reaches the body this step, so nothing reaches the water either
			}
			Out.Force = Force;
			Out.Torque = Torque;
		}

		/** Voxels and spheres: each point is wet by how deep it sits (a cube's linear fraction, a sphere's cap volume), lifted at its
		 * centre, dragged by the water moving past it, and slammed as its wetted surface grows while it drives down into the water (State
		 * keeps each point's last wetted surface). Ripples (when not null) gets each wet point's impulse into the water. */
		/**
		 * Ripples: how much of an impulse given this deep under the surface reaches the waves on it. A pressure at depth d moves the surface
		 * by about exp(-k·d) for waves of wavenumber k, and the waves a body makes are about its own size (k ≈ 1 / its mean half-extent).
		 * Without it a body sinking 3 m down kept pressing its drag into the surface over it as if it lay on top: the 20 ft wrecking ball
		 * dug the drawn water 16 m deep under itself, which sprang back up as a tall glassy spike.
		 */
		float RippleDepthScale(const FHull& Hull)
		{
			const FVector3f Extent = Hull.BoundsMax - Hull.BoundsMin;
			return FMath::Max((Extent.X + Extent.Y + Extent.Z) / 6.f, 5.f);
		}

		float RippleReach(float DepthCm, float Scale)
		{
			return FMath::Exp(-FMath::Max(DepthCm, 0.f) / Scale);
		}

		void IntegratePoints(const FHull& Hull, const FBodyState& Body, const FWaterView& Water, const FParams& P, float DeltaSeconds, FStepState& State,
			FStepResult& Out, bool bWaterline, TArray<FVector4f>* Ripples)
		{
			const FTransform& Transform = Body.Transform;
			const FVector& Com = Body.CenterOfMass;
			const FVector& V = Body.Velocity;
			const FVector& W = Body.AngularVelocity;
			const bool bSpheres = Hull.Model == EModel::Spheres;
			ResetWaterline(Out);
			double WashSum = 0.0;
			const FVector BodyCentre = Transform.TransformPosition(FVector(Hull.Centroid));
			FVector Force = FVector::ZeroVector;
			FVector Torque = FVector::ZeroVector;
			auto Apply = [&Force, &Torque, &Com](const FVector& F, const FVector& At)
			{
				Force += F;
				Torque += FVector::CrossProduct(At - Com, F);
			};
			const float ReferencePressure = 0.5f * P.Rho * P.ReferenceSpeed * P.ReferenceSpeed;
			const float InverseReference = 1.f / FMath::Max(P.ReferenceSpeed, 1.f);
			double LiftZ = 0.0;
			FVector LiftMoment = FVector::ZeroVector;
			float WettedArea = 0.f;
			float SurfaceArea = 0.f;
			// Slamming needs each point's wetted surface from the step before. A new hull (another level of detail) starts from its present
			// wetting: no slam from the switch.
			const int32 NumPoints = Hull.Points.Num();
			const bool bPrimed = State.Hull == &Hull && State.PreviousWetArea.Num() == NumPoints;
			if (!bPrimed)
			{
				State.PreviousWetArea.Init(0.f, NumPoints);
				State.Hull = &Hull;
				State.SlamLength = SlamLength(Hull);
			}
			TArray<FSlamPiece>& Slams = SlamScratch;
			Slams.Reset();
			const float RippleScale = RippleDepthScale(Hull);
			float TotalSurface = 0.f;
			for (const float PointSize : Hull.PointSizes)
			{
				TotalSurface += bSpheres ? 4.f * PI * PointSize * PointSize : 6.f * PointSize * PointSize;
			}
			for (int32 Index = 0; Index < NumPoints; ++Index)
			{
				const FVector Centre = Transform.TransformPosition(FVector(Hull.Points[Index]));
				const float Size = Hull.PointSizes[Index];
				const float Section = bSpheres ? PI * Size * Size : Size * Size;
				const float DragSection = Section * Hull.PointSectionScale;
				SurfaceArea += bSpheres ? 4.f * Section : 6.f * Section;
				const FVector Velocity = V + FVector::CrossProduct(W, Centre - Com);
				float Height = 0.f;
				FVector WaterVelocity = FVector::ZeroVector;
				float Fraction = 0.f;
				float Submerged = 0.f;
				float Rise = 0.f;          // cm³ more under water per cm deeper (the waterplane area)
				float Wet = 0.f;           // wetted surface, cm²
				float DragFraction = 0.f;  // the share of the section that is under water, facing the water moving past
				if (Water.Sample(Centre, Height, WaterVelocity))
				{
					WaterVelocity.X *= P.FlowCoupling;
					WaterVelocity.Y *= P.FlowCoupling;
					if (bSpheres)
					{
						const float Cap = FMath::Clamp(Height - static_cast<float>(Centre.Z) + Size, 0.f, 2.f * Size);
						Submerged = PI * Cap * Cap * (3.f * Size - Cap) / 3.f;
						Fraction = Submerged / (4.f / 3.f * PI * Size * Size * Size);
						Rise = PI * Cap * (2.f * Size - Cap);
						// A sphere has 2πR of surface per unit of height, so the wetted cap is 2πR × its height; the submerged part of its section is
						// the circular segment below the water line.
						Wet = 2.f * PI * Size * Cap;
						const float Below = Size - Cap;   // from the centre down to the water line (negative: the water is above the centre)
						const float Segment = Size * Size * FMath::Acos(FMath::Clamp(Below / Size, -1.f, 1.f)) - Below * FMath::Sqrt(FMath::Max(Size * Size - Below * Below, 0.f));
						DragFraction = FMath::Clamp(Segment / Section, 0.f, 1.f);
					}
					else
					{
						Fraction = FMath::Clamp((Height - static_cast<float>(Centre.Z)) / Size + 0.5f, 0.f, 1.f);
						Submerged = Fraction * Hull.PointVolumes[Index];
						Rise = Fraction > 0.f && Fraction < 1.f ? Hull.PointVolumes[Index] / Size : 0.f;
						// The bottom face and the sides up to the water line; all six faces under water.
						Wet = Fraction >= 1.f ? 6.f * Section : (Fraction > 0.f ? Section * (1.f + 4.f * Fraction) : 0.f);
						DragFraction = Fraction;
					}
				}
				// Slamming, as a hull triangle (von Kármán/Wagner): the wetted surface grows while the point drives down into the water, the
				// water's added-mass momentum. It pushes the point up, and the water down. Only an entry (PointSlamMinSpeed or faster): a
				// floating ball or crate bobbing on the waves wets and dries a little every cycle, and its added-mass push there is part of
				// how it floats, not an impact; counted as a slam it would kick the water on every downstroke. The slam is the body's, not
				// the point's (ResolveSlam, once every point is in): here the point only joins it.
				float& PreviousWet = State.PreviousWetArea[Index];
				if (bPrimed && P.SlamMultiplier > 0.f && DeltaSeconds > 0.f && Wet > PreviousWet)
				{
					const float Down = static_cast<float>(WaterVelocity.Z - Velocity.Z);   // cm/s down into the water
					if (Down >= PointSlamMinSpeed)
					{
						FSlamPiece& Piece = Slams.AddDefaulted_GetRef();
						Piece.Growth = (Wet - PreviousWet) / DeltaSeconds;
						Piece.Speed = Down;
						Piece.Surface = bSpheres ? 4.f * Section : 6.f * Section;
						Piece.Centre = Centre;
						Piece.Push = FVector::UpVector;
						Piece.Location = FVector(Centre.X, Centre.Y, Height);   // where it meets the water
						if (Ripples)
						{
							// What the slam presses into the water: over the wet part of the point's section, as much of it as reaches the
							// surface from the point's top.
							const float Top = static_cast<float>(Centre.Z) + (bSpheres ? Size : 0.5f * Size);
							Piece.NumRipples = 1;
							Piece.RippleX[0] = static_cast<float>(Centre.X);
							Piece.RippleY[0] = static_cast<float>(Centre.Y);
							Piece.RippleArea[0] = DragSection * DragFraction;
							Piece.RippleShare[0] = RippleReach(Height - Top, RippleScale);
						}
					}
				}
				PreviousWet = Wet;
				if (Fraction > 0.f)
				{
					const float Lift = P.Rho * P.Gravity * Submerged * Hull.PointLift[Index] * P.BuoyancyMultiplier;
					Apply(FVector(0.0, 0.0, Lift), Centre);
					LiftZ += Lift;
					LiftMoment += Centre * Lift;
					WettedArea += Wet;
					AddWaterplane(Out, P.Rho * P.Gravity * Rise * Hull.PointLift[Index] * P.BuoyancyMultiplier, Centre - Com);
					const FVector Relative = Velocity - WaterVelocity;
					const float Speed = static_cast<float>(Relative.Size());
					const float DragScale = ReferencePressure * DragSection * DragFraction * Hull.PointDrag[Index] * InverseReference;
					float RippleZ = 0.f;   // the drag's upward part on the point: the impulse down into the water (the slam's comes with the body's, ResolveSlam)
					if (Speed > 1.f)
					{
						const float S = Speed * InverseReference;
						const FVector PressureDrag = -Relative / Speed * (ReferencePressure * DragSection * DragFraction * Hull.PointDrag[Index] * (P.PressureDragLinear * S + P.PressureDragQuadratic * S * S));
						Apply(PressureDrag, Centre);
						AddDragDampingAnyWay(Out, DragScale * (P.PressureDragLinear + 2.f * P.PressureDragQuadratic * S), Centre - Com);
						RippleZ += static_cast<float>(PressureDrag.Z);
					}
					else
					{
						AddDragDampingAnyWay(Out, DragScale * P.PressureDragLinear, Centre - Com);
					}
					// Ripples: the drag's vertical part is what the point presses into the water (or, rising, pulls up out of it), over the
					// wet part of its section, as much of it as reaches the surface from the point's top.
					if (Ripples && RippleZ != 0.f)
					{
						const float Top = static_cast<float>(Centre.Z) + (bSpheres ? Size : 0.5f * Size);
						Ripples->Add(FVector4f(static_cast<float>(Centre.X), static_cast<float>(Centre.Y), RippleZ * DeltaSeconds * RippleReach(Height - Top, RippleScale),
							DragSection * DragFraction));
					}
					// Spheres: skin friction on the wetted cap against the sphere's spin. A pontoon is dragged at its centre, which no spin moves,
					// so without this a floating ball would spin on for ever.
					if (bSpheres && P.ViscousMultiplier > 0.f && Wet > 0.f)
					{
						const float Spin = static_cast<float>(W.Size());
						if (Spin > 1e-3f)
						{
							const float SurfaceSpeed = Spin * Size;
							const double Reynolds = FMath::Max(SurfaceSpeed * 2.0 * Size / P.KinematicViscosity, 1e5);
							const double Log = FMath::LogX(10.0, Reynolds) - 2.0;
							const float Cf = static_cast<float>(0.075 / (Log * Log));
							// The shear ½ ρ Cf A u² on the cap at the surface speed, times the radius: a torque against the spin.
							const float Shear = 0.5f * P.Rho * Cf * Wet * SurfaceSpeed * SurfaceSpeed * Size * P.ViscousMultiplier * Hull.PointDrag[Index];
							Torque -= W / Spin * Shear;
							Out.SpinDamping += FVector(2.f * Shear / Spin);
						}
					}
					if (Fraction < 1.f && bSpheres)
					{
						// A sphere's water line is its own circle: pieces all round it, each facing out from the sphere's centre. (Taken as
						// one piece facing away from the body's centre, a ball on its body's axis had no side to push with: no wake, no wet
						// line, no bow spray.)
						const float LineDepth = static_cast<float>(Height - Centre.Z);
						const float CircleRadius = FMath::Sqrt(FMath::Max(Size * Size - LineDepth * LineDepth, 0.f));
						if (CircleRadius >= 1.f)
						{
							constexpr int32 Pieces = 8;
							const float PieceLength = 2.f * PI * CircleRadius / Pieces;
							for (int32 Piece = 0; Piece < Pieces; ++Piece)
							{
								const float Theta = 2.f * PI * (Piece + 0.5f) / Pieces;
								const FVector Facing(FMath::Cos(Theta), FMath::Sin(Theta), 0.0);
								const FVector OnLine = FVector(Centre.X, Centre.Y, Height) + Facing * CircleRadius;
								AddWaterline(Out, Hull, Body, OnLine, PieceLength, Facing, WaterVelocity, WashSum);
								if (bWaterline && Out.Waterline.Num() < 2048)
								{
									const float Next = 2.f * PI * (Piece + 1.5f) / Pieces;
									Out.Waterline.Add(OnLine);
									Out.Waterline.Add(FVector(Centre.X, Centre.Y, Height) + FVector(FMath::Cos(Next), FMath::Sin(Next), 0.0) * CircleRadius);
								}
							}
						}
					}
					else if (Fraction < 1.f)
					{
						if (bWaterline && Out.Waterline.Num() < 2048)
						{
							Out.Waterline.Add(FVector(Centre.X - Size, Centre.Y, Height));
							Out.Waterline.Add(FVector(Centre.X + Size, Centre.Y, Height));
						}
						AddWaterline(Out, Hull, Body, FVector(Centre.X, Centre.Y, Height), Size, Centre - BodyCentre, WaterVelocity, WashSum);
					}
				}
				if (Fraction < 1.f && P.RhoAir > 0.f && P.AirDragCoefficient > 0.f)
				{
					const FVector Relative = Velocity - P.Wind;
					const float Speed = static_cast<float>(Relative.Size());
					if (Speed > 1.f)
					{
						const FVector Air = -Relative * (0.5f * P.RhoAir * P.AirDragCoefficient * DragSection * (1.f - DragFraction) * Speed);
						Apply(Air, Centre);
						Out.AirForce += Air;
					}
				}
			}
			Out.bTouchesWater = WettedArea > 0.f;
			Out.WettedArea = WettedArea;
			Out.Wash = static_cast<float>(WashSum / FMath::Max(Out.WaterlineLength, 1.f));
			Out.BuoyancyForce = FVector(0.0, 0.0, LiftZ);
			if (LiftZ > 0.0)
			{
				Out.DisplacedVolume = static_cast<float>(LiftZ / (P.Rho * P.Gravity * FMath::Max(P.BuoyancyMultiplier, 1e-3f)));
				Out.BuoyancyCentre = LiftMoment / LiftZ;
			}
			ResolveSlam(Body, P, DeltaSeconds, TotalSurface, State.SlamLength, Slams, Force, Torque, Out, Ripples);
			if (Ripples)
			{
				TakeRipples(*Ripples, Out);
			}
			FinishStep(Body, Water, P, DeltaSeconds, FMath::Min(1.f, 2.f * WettedArea / FMath::Max(SurfaceArea, 1.f)), State, Force, Torque, Out);
		}

		/** An entry: the first step the body touches the water after a step it didn't, driving down into it at least PointSlamMinSpeed.
		 * Its splash is sized by the whole body, not by its hardest hull piece: its kinetic energy ½ M v² delivered in the time it takes
		 * to sink through its own height H (H / v), M v³ / 2H, the form of a swimmer's part (m v³ / 2r). It holds nothing of the hull's
		 * pieces, so a drop splashes the same at every Hull Resolution, level of detail, voxel size and hull model; heavier and faster
		 * splashes bigger, and a body going in end first (tall) smaller than one slapping in flat. H is measured on the finest hull: a
		 * coarse level's few vertices would cut a round body short. The water's surface moves no water up or down here
		 * (FWaterView::Sample), so v is the body's own sinking speed. The slam the same entry raises is the body's too (ResolveSlam), gated
		 * at the same speed, so the two agree on what an entry is at every hull resolution. */
		void NoteEntry(const FHull* FinestHull, const FBodyState& Body, FStepState& State, FStepResult& Out)
		{
			if (FinestHull && State.bHasStepped && !State.bWasWet && Out.bTouchesWater)
			{
				const double Down = -Body.Velocity.Z;
				if (Down >= PointSlamMinSpeed)
				{
					float Bottom = 0.f;
					float Top = 0.f;
					RestedExtent(*FinestHull, Body.Transform.GetRotation(), Bottom, Top);
					const double Tall = FMath::Max(static_cast<double>(Top - Bottom), 1.0);
					Out.EntryPower = static_cast<float>(Body.Mass * Down * Down * Down / (2.0 * Tall));
				}
			}
			State.bWasWet = Out.bTouchesWater;
			State.bHasStepped = true;
		}
	}

	void Integrate(const FHull& Hull, const FBodyState& Body, const FWaterView& Water, const FParams& P, float DeltaSeconds, FStepState& State,
		FStepResult& Out, bool bWaterline, bool bRippleImpulses, const FHull* FinestHull)
	{
		SCOPE_CYCLE_COUNTER(STAT_FNB_Integrate);
		TRACE_CPUPROFILER_EVENT_SCOPE(FluidNinjaBuoyancy::Integrate);
		Out.Force = Out.Torque = Out.AngularDampingRate = Out.BuoyancyForce = Out.AirForce = FVector::ZeroVector;
		Out.BuoyancyCentre = Body.CenterOfMass;
		Out.DisplacedVolume = Out.WettedArea = Out.SlamPower = Out.EntryPower = Out.ThrustApplied = Out.PropulsionForceZ = 0.f;
		Out.KeelForce = Out.DriftAngleDeg = 0.f;
		Out.SlamLocation = Out.WaveResistance = FVector::ZeroVector;
		Out.Propulsor = FPropulsorResult();
		Out.bTouchesWater = false;
		Out.HeaveStiffness = 0.f;
		Out.TiltStiffness = Out.DragDamping = Out.SpinDamping = FVector::ZeroVector;
		Out.Waterline.Reset();
		Out.RippleImpulses.Reset();
		ResetWaterline(Out);
		// Ripples, only when asked for: the pieces go into this thread's list, reserved to what the last step needed, and the list is
		// moved out whole once the pieces are done (TakeRipples). Not asked for, nothing is gathered and nothing is allocated.
		TArray<FVector4f>* Ripples = nullptr;
		if (bRippleImpulses && DeltaSeconds > 0.f)
		{
			RippleScratch.Reset(RippleScratchLast);
			Ripples = &RippleScratch;
		}
		if (Hull.Model != EModel::Triangles)
		{
			IntegratePoints(Hull, Body, Water, P, DeltaSeconds, State, Out, bWaterline, Ripples);
			NoteEntry(FinestHull, Body, State, Out);
			return;
		}

		const int32 NumVertices = Hull.Vertices.Num();
		const int32 NumTriangles = Hull.Triangles.Num();
		thread_local TArray<FVector> WorldPoints;
		thread_local TArray<float> Depths;
		thread_local TArray<FVector> WaterVelocities;
		WorldPoints.SetNumUninitialized(NumVertices, EAllowShrinking::No);
		Depths.SetNumUninitialized(NumVertices, EAllowShrinking::No);
		WaterVelocities.SetNumUninitialized(NumVertices, EAllowShrinking::No);
		// A new hull (another level of detail) starts from its present wetting: no slam from the switch.
		const bool bPrimed = State.Hull == &Hull && State.PreviousWetArea.Num() == NumTriangles;
		if (!bPrimed)
		{
			State.PreviousWetArea.Init(0.f, NumTriangles);
			State.Hull = &Hull;
			State.SlamLength = SlamLength(Hull);
		}
		TArray<FSlamPiece>& Slams = SlamScratch;
		Slams.Reset();

		constexpr float Dry = -1e9f;
		const FTransform& Transform = Body.Transform;
		// The body's rotation as a matrix, once per step: a vertex or a normal then turns by three multiply-adds, where the transform
		// rotated each by its quaternion.
		const FMatrix Rotation = Transform.ToMatrixNoScale();
		// Wholly above the water there is nothing to sample: the hull's lowest point (its bounds box in the body's attitude) against the
		// highest the water stands anywhere the body could meet it, the patch's ceiling (where its publisher gave one) or the flat
		// height. The depths come out as sampling would give them (none wet), without its reads.
		double Ceiling = -TNumericLimits<double>::Max();
		if (Water.Patch)
		{
			Ceiling = Water.Patch->HeightCeiling;
		}
		if (Water.bFlat)
		{
			Ceiling = FMath::Max(Ceiling, static_cast<double>(Water.FlatHeight));
		}
		double Lowest = Rotation.M[3][2];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Rise = Rotation.M[Axis][2];   // how much world Z a local step along this axis climbs
			Lowest += FMath::Min(Rise * Hull.BoundsMin[Axis], Rise * Hull.BoundsMax[Axis]);
		}
		const bool bAbove = Lowest > Ceiling;
		bool bAnyWet = false;
		for (int32 Index = 0; Index < NumVertices; ++Index)
		{
			const FVector Point(Rotation.TransformPosition(FVector(Hull.Vertices[Index])));
			WorldPoints[Index] = Point;
			float Height = 0.f;
			FVector WaterVelocity;
			if (!bAbove && Water.Sample(Point, Height, WaterVelocity))
			{
				Depths[Index] = Height - static_cast<float>(Point.Z);
				WaterVelocities[Index] = FVector(WaterVelocity.X * P.FlowCoupling, WaterVelocity.Y * P.FlowCoupling, WaterVelocity.Z);
				bAnyWet |= Depths[Index] > 0.f;
			}
			else
			{
				Depths[Index] = Dry;
				WaterVelocities[Index] = FVector::ZeroVector;
			}
		}

		const FVector& Com = Body.CenterOfMass;
		const FVector& V = Body.Velocity;
		const FVector& W = Body.AngularVelocity;
		FVector Force = FVector::ZeroVector;
		FVector Torque = FVector::ZeroVector;
		auto Apply = [&Force, &Torque, &Com](const FVector& F, const FVector& At)
		{
			Force += F;
			Torque += FVector::CrossProduct(At - Com, F);
		};

		// Skin friction coefficient (ITTC 1957) from the hull's Reynolds number through the water around it.
		float Cf = 0.f;
		if (bAnyWet && P.ViscousMultiplier > 0.f)
		{
			float Height = 0.f;
			FVector WaterVelocity = FVector::ZeroVector;
			FVector Relative = V;
			if (Water.Sample(Com, Height, WaterVelocity))
			{
				Relative -= FVector(WaterVelocity.X * P.FlowCoupling, WaterVelocity.Y * P.FlowCoupling, WaterVelocity.Z);
			}
			const double Reynolds = FMath::Max(Relative.Size() * FMath::Max(Hull.Length, 1.f) / P.KinematicViscosity, 1e5);
			const double Log = FMath::LogX(10.0, Reynolds) - 2.0;
			Cf = static_cast<float>(0.075 / (Log * Log));
		}

		const float ReferencePressure = 0.5f * P.Rho * P.ReferenceSpeed * P.ReferenceSpeed;
		const float InverseReference = 1.f / FMath::Max(P.ReferenceSpeed, 1.f);
		double WashSum = 0.0;
		float WettedArea = 0.f;
		double LiftZ = 0.0;
		FVector LiftMoment = FVector::ZeroVector;
		FVector Buoyancy = FVector::ZeroVector;

		const float RippleScale = RippleDepthScale(Hull);
		// Pressure and suction fall off with the facing by a power; at the default power, 0.5, that is a square root, exact and a few
		// times cheaper than the general power.
		const bool bPressureSqrt = P.PressureFalloff == 0.5f;
		const bool bSuctionSqrt = P.SuctionFalloff == 0.5f;
		// A planing hull (EHullFlow::Planing): the planing law on the faces meeting the flow, a ventilated transom, slams on the vertical entry
		// speed alone, the steady pressure apart for the waves, and its pressure distribution kept for the wave-making resistance.
		const bool bPlaning = P.HullFlow == EHullFlow::Planing;
		const bool bSplitSteady = bPlaning && P.bSplitSteadyImpulses && Ripples;
		const float PlaningHalfRhoK = 0.5f * P.Rho * FMath::Max(P.PlaningPressureCoefficient, 0.f);
		const bool bWaveResistance = bPlaning && P.WaveResistanceScale > 0.f && DeltaSeconds > 0.f;
		// The wave-making resistance is evaluated every WaveResistanceInterval: its pressure is gathered only on the steps that evaluate it.
		const bool bWaveLoads = bWaveResistance && State.WaveResistanceSeconds >= WaveResistanceInterval - DeltaSeconds;
		TArray<FVector4f>& Loads = WaveLoadScratch;
		Loads.Reset();
		for (int32 TriIndex = 0; TriIndex < NumTriangles; ++TriIndex)
		{
			const FIntVector3& Tri = Hull.Triangles[TriIndex];
			const int32 Corner[3] = {Tri.X, Tri.Y, Tri.Z};
			const float Depth[3] = {Depths[Corner[0]], Depths[Corner[1]], Depths[Corner[2]]};
			const int32 NumWet = (Depth[0] > 0.f) + (Depth[1] > 0.f) + (Depth[2] > 0.f);
			const FVector Normal(Rotation.TransformVector(FVector(Hull.Normals[TriIndex])));
			const float Area = Hull.Areas[TriIndex];
			const FVector Centre = (WorldPoints[Corner[0]] + WorldPoints[Corner[1]] + WorldPoints[Corner[2]]) / 3.0;

			FVector WaterVelocity = FVector::ZeroVector;
			int32 Sampled = 0;
			for (int32 K = 0; K < 3; ++K)
			{
				if (Depth[K] > Dry * 0.5f)
				{
					WaterVelocity += WaterVelocities[Corner[K]];
					++Sampled;
				}
			}
			if (Sampled > 0)
			{
				WaterVelocity /= Sampled;
			}

			// The wetted part: the whole triangle, a corner triangle, or a quad (two triangles), cut where the depth crosses zero.
			FVector SubPoints[2][3];
			float SubDepths[2][3];
			int32 NumSub = 0;
			if (NumWet == 3)
			{
				SubPoints[0][0] = WorldPoints[Corner[0]];
				SubPoints[0][1] = WorldPoints[Corner[1]];
				SubPoints[0][2] = WorldPoints[Corner[2]];
				SubDepths[0][0] = Depth[0];
				SubDepths[0][1] = Depth[1];
				SubDepths[0][2] = Depth[2];
				NumSub = 1;
			}
			else if (NumWet > 0)
			{
				// K0 is the odd one out: the wet corner of a corner triangle, or the dry corner of a quad. Its neighbours follow in
				// winding order, so the pieces keep the triangle's facing.
				int32 K0 = 0;
				for (int32 K = 0; K < 3; ++K)
				{
					if ((Depth[K] > 0.f) == (NumWet == 1))
					{
						K0 = K;
						break;
					}
				}
				const int32 K1 = (K0 + 1) % 3;
				const int32 K2 = (K0 + 2) % 3;
				const FVector P0 = WorldPoints[Corner[K0]];
				const FVector P1 = WorldPoints[Corner[K1]];
				const FVector P2 = WorldPoints[Corner[K2]];
				const float D0 = Depth[K0];
				const float D1 = Depth[K1];
				const float D2 = Depth[K2];
				const FVector C01 = P0 + (P1 - P0) * (D0 / (D0 - D1));
				const FVector C02 = P0 + (P2 - P0) * (D0 / (D0 - D2));
				if (NumWet == 1)
				{
					SubPoints[0][0] = P0;
					SubPoints[0][1] = C01;
					SubPoints[0][2] = C02;
					SubDepths[0][0] = D0;
					SubDepths[0][1] = 0.f;
					SubDepths[0][2] = 0.f;
					NumSub = 1;
				}
				else
				{
					SubPoints[0][0] = C01;
					SubPoints[0][1] = P1;
					SubPoints[0][2] = P2;
					SubDepths[0][0] = 0.f;
					SubDepths[0][1] = D1;
					SubDepths[0][2] = D2;
					SubPoints[1][0] = C01;
					SubPoints[1][1] = P2;
					SubPoints[1][2] = C02;
					SubDepths[1][0] = 0.f;
					SubDepths[1][1] = D2;
					SubDepths[1][2] = 0.f;
					NumSub = 2;
				}
				if (D1 > Dry * 0.5f && D2 > Dry * 0.5f && D0 > Dry * 0.5f)
				{
					AddWaterline(Out, Hull, Body, (C01 + C02) * 0.5, static_cast<float>(FVector::Distance(C01, C02)), Normal, WaterVelocity, WashSum);
					if (bWaterline && Out.Waterline.Num() < 2048)
					{
						Out.Waterline.Add(C01);
						Out.Waterline.Add(C02);
					}
				}
			}

			// Ripples: each wet piece's centre, area and the normal (pressure drag or suction) force on it, kept until the slam is known; on a
			// planing hull also the share of that force its horizontal motion drives (its steady part).
			double RippleX[2] = {};
			double RippleY[2] = {};
			float RippleArea[2] = {};
			float RippleForceZ[2] = {};
			float RippleSteadyZ[2] = {};
			float RippleDepth[2] = {};
			int32 NumRipplePieces = 0;
			const float FacingUp = FMath::Abs(static_cast<float>(Normal.Z));   // the share of a piece seen from above

			float Wet = 0.f;
			for (int32 Sub = 0; Sub < NumSub; ++Sub)
			{
				const FVector& A = SubPoints[Sub][0];
				const FVector& B = SubPoints[Sub][1];
				const FVector& C = SubPoints[Sub][2];
				const float SubArea = 0.5f * static_cast<float>(FVector::CrossProduct(B - A, C - A).Size());
				if (SubArea < 1e-3f)
				{
					continue;
				}
				const FVector SubCentre = (A + B + C) / 3.0;
				const float DepthSum = SubDepths[Sub][0] + SubDepths[Sub][1] + SubDepths[Sub][2];
				const float SubDepth = DepthSum / 3.f;
				Wet += SubArea;

				// Hydrostatic pressure ρ g h on the piece (exact for a plane piece: the pressure is linear over it), acting at its centre
				// of pressure: for depths d_i at the corners, Σ (D + d_i) v_i / 4D with D = Σ d_i. At the centroid instead, the moment is
				// off by enough to hold a barely stable hull (a light cube) heeled.
				const FVector Pressure = DepthSum > 1e-3f
					? (A * (DepthSum + SubDepths[Sub][0]) + B * (DepthSum + SubDepths[Sub][1]) + C * (DepthSum + SubDepths[Sub][2])) / (4.0 * DepthSum)
					: SubCentre;
				const FVector Lift = -Normal * (P.Rho * P.Gravity * SubDepth * SubArea * P.BuoyancyMultiplier);
				Apply(Lift, Pressure);
				Buoyancy += Lift;
				LiftZ += Lift.Z;
				LiftMoment += Pressure * Lift.Z;
				// Stable steps: the wet surface seen from below is the waterplane (divergence theorem), so its pieces sum to it.
				const FVector Arm = SubCentre - Com;
				AddWaterplane(Out, -static_cast<float>(Normal.Z) * SubArea * P.Rho * P.Gravity * P.BuoyancyMultiplier, Arm);

				// The water moving past it: pressure drag where the face pushes into the water, suction where it pulls away, and
				// skin friction along it.
				const FVector Relative = V + FVector::CrossProduct(W, SubCentre - Com) - WaterVelocity;
				const float Speed = static_cast<float>(Relative.Size());
				const float DragScale = ReferencePressure * SubArea * InverseReference;
				float NormalForceZ = 0.f;   // the pressure drag or suction on the body, upward (ripples)
				float SteadyShare = 0.f;    // the share of it the piece's horizontal motion through the water drives (planing)
				if (Speed > 1.f)
				{
					const float NormalSpeed = static_cast<float>(FVector::DotProduct(Normal, Relative));
					const float Cos = NormalSpeed / Speed;
					const float S = Speed * InverseReference;
					if (bPlaning && FMath::Abs(NormalSpeed) > 1e-3f)
					{
						SteadyShare = FMath::Clamp(static_cast<float>(Normal.X * Relative.X + Normal.Y * Relative.Y) / NormalSpeed, 0.f, 1.f);
					}
					if (Cos > 0.f && bPlaning)
					{
						// Savitsky's planing law: the dynamic pressure k ½ρ V² on a face, scaled by its incidence cos θ (sin τ on a bottom at
						// trim τ), so a flat bottom lifts with its trim and a face meeting the flow head-on stagnates at k of it. Its slope
						// along the normal, k ½ρ A V (1 + cos² θ), is the drag's derivative for stable steps.
						const FVector NormalForce = -Normal * (PlaningHalfRhoK * SubArea * Speed * Speed * Cos);
						Apply(NormalForce, SubCentre);
						NormalForceZ = static_cast<float>(NormalForce.Z);
						AddDragDamping(Out, PlaningHalfRhoK * SubArea * Speed * (1.f + Cos * Cos), Arm, Normal);
					}
					else if (Cos > 0.f)
					{
						const float Falloff = bPressureSqrt ? FMath::Sqrt(Cos) : FMath::Pow(Cos, P.PressureFalloff);
						const FVector NormalForce = -Normal * (ReferencePressure * SubArea * (P.PressureDragLinear * S + P.PressureDragQuadratic * S * S) * Falloff);
						Apply(NormalForce, SubCentre);
						NormalForceZ = static_cast<float>(NormalForce.Z);
						AddDragDamping(Out, DragScale * (P.PressureDragLinear + 2.f * P.PressureDragQuadratic * S) * Falloff, Arm, Normal);
					}
					else
					{
						const float Falloff = bSuctionSqrt ? FMath::Sqrt(-Cos) : FMath::Pow(-Cos, P.SuctionFalloff);
						float Suction = ReferencePressure * (P.SuctionDragLinear * S + P.SuctionDragQuadratic * S * S) * Falloff;
						// A planing hull's stern: the pressure behind a face pulling away cannot fall below the air's, so the suction is at most
						// the hydrostatic head over the piece; past it the water lets go of the face (a ventilated transom) and the drag no
						// longer grows with the speed.
						const float Ventilated = P.Rho * P.Gravity * FMath::Max(SubDepth, 0.f);
						const bool bVentilatedFace = bPlaning && Suction > Ventilated;
						if (bVentilatedFace)
						{
							Suction = Ventilated;
						}
						const FVector NormalForce = Normal * (Suction * SubArea);
						Apply(NormalForce, SubCentre);
						NormalForceZ = static_cast<float>(NormalForce.Z);
						if (!bVentilatedFace)
						{
							AddDragDamping(Out, DragScale * (P.SuctionDragLinear + 2.f * P.SuctionDragQuadratic * S) * Falloff, Arm, Normal);
						}
					}
					if (Cf > 0.f)
					{
						const FVector Tangential = Relative - Normal * FVector::DotProduct(Relative, Normal);
						const float Slip = static_cast<float>(Tangential.Size());
						Apply(-Tangential * (0.5f * P.Rho * Cf * SubArea * Slip * P.ViscousMultiplier), SubCentre);
						if (Slip > 1e-3f)
						{
							AddDragDamping(Out, P.Rho * Cf * SubArea * Slip * P.ViscousMultiplier, Arm, Tangential / Slip);
						}
					}
				}
				else
				{
					// Barely moving: the drag's linear part is what a step's worth of acceleration first meets.
					AddDragDamping(Out, DragScale * 0.5f * (P.PressureDragLinear + P.SuctionDragLinear), Arm, Normal);
				}
				if (Ripples)
				{
					RippleX[NumRipplePieces] = SubCentre.X;
					RippleY[NumRipplePieces] = SubCentre.Y;
					RippleArea[NumRipplePieces] = SubArea;
					RippleForceZ[NumRipplePieces] = NormalForceZ;
					RippleSteadyZ[NumRipplePieces] = NormalForceZ * SteadyShare;
					RippleDepth[NumRipplePieces] = SubDepth;
					++NumRipplePieces;
				}
				// The wave-making resistance's pressure: the piece's hydrostatic lift and the steady part of its dynamic push, about the centre
				// of mass, spread over the disc of its area seen from above (a disc of radius r filters waves as a Gaussian of σ = r / 2).
				if (bWaveLoads)
				{
					Loads.Add(FVector4f(static_cast<float>(SubCentre.X - Com.X), static_cast<float>(SubCentre.Y - Com.Y),
						static_cast<float>(Lift.Z) + NormalForceZ * SteadyShare, 0.5f * FMath::Sqrt(FMath::Max(SubArea * FacingUp, 1.f) / PI)));
				}
			}
			WettedArea += Wet;

			// Slamming: the wetted area grows while the face drives into the water (added-mass momentum, von Kármán/Wagner). Only an
			// entry (PointSlamMinSpeed or faster), as for voxels and spheres: a hull in chop wets a little more of itself on every
			// downstroke, and that added-mass push is part of how it floats, not an impact. Slamming on any inward speed kicked the water
			// on each downstroke and offered every one as a splash. The slam is the body's, not the face's (ResolveSlam, once every face
			// is in): here the face only joins it, with its wet pieces seen from above for the ripples (the slam acts on the triangle's wet
			// part as a whole, so each piece takes its share by area, as much of it as reaches the surface from its depth).
			float& Previous = State.PreviousWetArea[TriIndex];
			const float FromAbove = FMath::Abs(static_cast<float>(Normal.Z));   // the share of a piece seen from above
			if (bPrimed && P.SlamMultiplier > 0.f && DeltaSeconds > 0.f && Wet > Previous)
			{
				const FVector Relative = V + FVector::CrossProduct(W, Centre - Com) - WaterVelocity;
				// A planing hull slams on its vertical entry speed alone: the forward speed's push on a bottom re-wetting in chop is the
				// planing law's, which a slam on the whole inward speed would count again.
				const float Vn = bPlaning ? static_cast<float>(Relative.Z * Normal.Z) : static_cast<float>(FVector::DotProduct(Relative, Normal));
				if (Vn >= PointSlamMinSpeed)
				{
					FSlamPiece& Piece = Slams.AddDefaulted_GetRef();
					Piece.Growth = (Wet - Previous) / DeltaSeconds;
					Piece.Speed = Vn;
					Piece.Surface = Area;
					Piece.Centre = Centre;
					Piece.Push = -Normal;
					Piece.Location = Centre;
					if (Ripples)
					{
						Piece.NumRipples = NumRipplePieces;
						for (int32 RippleIndex = 0; RippleIndex < NumRipplePieces; ++RippleIndex)
						{
							Piece.RippleX[RippleIndex] = static_cast<float>(RippleX[RippleIndex]);
							Piece.RippleY[RippleIndex] = static_cast<float>(RippleY[RippleIndex]);
							Piece.RippleArea[RippleIndex] = RippleArea[RippleIndex] * FromAbove;
							Piece.RippleShare[RippleIndex] = RippleArea[RippleIndex] / Wet * RippleReach(RippleDepth[RippleIndex], RippleScale);
						}
					}
				}
			}
			Previous = Wet;

			// Ripples: the impulse each wet piece gave the water this step through its pressure drag or suction (the slam's comes with the
			// body's, ResolveSlam). The water pushing the body up is the body pushing the water down, so the dynamic force's upward part ×
			// the step is the impulse down into the water. Pieces with no dynamic force (at rest, or side-on) send nothing.
			if (Ripples && NumRipplePieces > 0)
			{
				for (int32 RippleIndex = 0; RippleIndex < NumRipplePieces; ++RippleIndex)
				{
					const float Reach = RippleReach(RippleDepth[RippleIndex], RippleScale);
					const float PieceForceZ = RippleForceZ[RippleIndex] * Reach;
					if (PieceForceZ == 0.f)
					{
						continue;
					}
					const float X = static_cast<float>(RippleX[RippleIndex]);
					const float Y = static_cast<float>(RippleY[RippleIndex]);
					const float PieceArea = RippleArea[RippleIndex] * FromAbove;
					// Planing: the steady part (its horizontal motion's) as a piece of its own, marked by a negative area; the rest as usual.
					const float SteadyZ = bSplitSteady ? RippleSteadyZ[RippleIndex] * Reach : 0.f;
					if (SteadyZ != 0.f)
					{
						Ripples->Add(FVector4f(X, Y, SteadyZ * DeltaSeconds, -FMath::Max(PieceArea, 1e-3f)));
					}
					const float TransientZ = PieceForceZ - SteadyZ;
					if (TransientZ != 0.f)
					{
						Ripples->Add(FVector4f(X, Y, TransientZ * DeltaSeconds, PieceArea));
					}
				}
			}

			// Air on the dry part.
			const float DryArea = Area - Wet;
			if (DryArea > 1.f && P.RhoAir > 0.f && P.AirDragCoefficient > 0.f)
			{
				const FVector Relative = V + FVector::CrossProduct(W, Centre - Com) - P.Wind;
				const float Speed = static_cast<float>(Relative.Size());
				if (Speed > 1.f)
				{
					const float Cos = static_cast<float>(FVector::DotProduct(Normal, Relative)) / Speed;
					if (Cos > 0.f)
					{
						const FVector Air = -Relative / Speed * (0.5f * P.RhoAir * P.AirDragCoefficient * DryArea * Cos * Speed * Speed);
						Apply(Air, Centre);
						Out.AirForce += Air;
					}
				}
			}
		}

		Out.bTouchesWater = WettedArea > 0.f;
		Out.WettedArea = WettedArea;
		Out.Wash = static_cast<float>(WashSum / FMath::Max(Out.WaterlineLength, 1.f));
		Out.BuoyancyForce = Buoyancy;
		if (LiftZ > 0.0)
		{
			Out.DisplacedVolume = static_cast<float>(LiftZ / (P.Rho * P.Gravity * FMath::Max(P.BuoyancyMultiplier, 1e-3f)));
			Out.BuoyancyCentre = LiftMoment / LiftZ;
		}

		ResolveSlam(Body, P, DeltaSeconds, Hull.TotalArea, State.SlamLength, Slams, Force, Torque, Out, Ripples);
		if (Ripples)
		{
			TakeRipples(*Ripples, Out);
		}
		// A planing hull's wave-making resistance (Havelock), against its way through the water at its centre of buoyancy.
		if (bWaveResistance && WettedArea > 0.f)
		{
			float Height = 0.f;
			FVector WaterVelocity = FVector::ZeroVector;
			FVector Through = V;
			if (Water.Sample(Com, Height, WaterVelocity))
			{
				Through -= FVector(WaterVelocity.X * P.FlowCoupling, WaterVelocity.Y * P.FlowCoupling, 0.0);
			}
			Through.Z = 0.0;
			if (bWaveLoads)
			{
				State.WaveResistanceSeconds = 0.f;
				const double U = Through.Size();
				FVector Raw = FVector::ZeroVector;
				if (U > 1.0)
				{
					// The loads in the frame of the motion: x along it, y across it.
					const FVector Along = Through / U;
					for (FVector4f& Load : Loads)
					{
						const float AlongX = static_cast<float>(Load.X * Along.X + Load.Y * Along.Y);
						const float Across = static_cast<float>(-Load.X * Along.Y + Load.Y * Along.X);
						Load.X = AlongX;
						Load.Y = Across;
					}
					const float Piece = FMath::Max(P.HullPieceCm, 2.f);
					FadeWettedTransom(Loads, WorldPoints, Depths, Com, Along, static_cast<float>(U), P.Gravity, Piece);
					const float Resistance = WaveMakingResistance(Loads, static_cast<float>(U), P.Rho, P.Gravity, P.WaterDepth, PI / Piece);
					Raw = -Along * (static_cast<double>(Resistance) * P.WaveResistanceScale);
				}
				State.WaveResistanceRaw = Raw;
			}
			else
			{
				State.WaveResistanceSeconds += DeltaSeconds;
			}
			const float Smooth = 1.f - FMath::Exp(-DeltaSeconds / WaveResistanceSmoothSeconds);
			State.WaveResistance += (State.WaveResistanceRaw - State.WaveResistance) * Smooth;
			Apply(State.WaveResistance, Out.BuoyancyCentre);
			Out.WaveResistance = State.WaveResistance;
		}
		else
		{
			State.WaveResistance = State.WaveResistanceRaw = FVector::ZeroVector;
			State.WaveResistanceSeconds = TNumericLimits<float>::Max();   // the next wet step evaluates at once
		}
		FinishStep(Body, Water, P, DeltaSeconds, FMath::Min(1.f, 2.f * WettedArea / FMath::Max(Hull.TotalArea, 1.f)), State, Force, Torque, Out);
		NoteEntry(FinestHull, Body, State, Out);
	}

	namespace
	{
		/**
		 * fnb.Physics.StableTheta: θ of the implicit step on the excess stiffness (experimental; 1 by default). The physics solver moves
		 * the body by dt·(v + Δv) over the step, and the excess spring is taken at θ of the way along that move, -k·(x + θ·dt·(v + Δv)).
		 * θ = 1 is backward Euler, the spring at the step's end position: the part of a bob the step is too long for dies within two
		 * steps, so above 2.4 Hz at 30 Hz steps and above 9.5 Hz at 120 Hz the same crate bobbed differently at 30 and 60 fps. θ = 0.5
		 * is the midpoint: that part keeps its amplitude from step to step, alternating in sign (it rings at the step rate). In the
		 * stiff limit the excess part's amplitude goes as 1 - 1/θ per step (0.6: alternating, a third lost every step); at every
		 * stiffness the step is stable for θ ≥ 0.5 (the step's map has determinant 1 / (1 + θ·k·dt²/m) and its trace within 1 + that).
		 * The excess damping is always backward Euler, the drag at the step's end velocity, whatever θ: on a damping a θ below 1 is not
		 * L-stable, the velocity after the step goes as 1 - (c·dt/m) / (1 + θ·c_x·dt/m), which turns negative, a reversal, for a damping
		 * that would stop the body in less than a step. The slam's one-step derivative (its force over the inward speed, up to the body's
		 * momentum per step) and its lever-squared spin damping on a tilted entry are exactly that stiff: at θ = 0.6 a row boat dropped
		 * flat reversed its pitch rate step after step, slammed again on every reversal and raised a ring two and a half times the
		 * backward Euler one, which the beach ball beside it rode at 58% instead of 100% (ripple-coupling, 2026-10-17). Backward Euler
		 * on a damping is monotone: the velocity only decays.
		 */
		float StableTheta = 1.f;
		static FAutoConsoleVariableRef CVarStableTheta(TEXT("fnb.Physics.StableTheta"), StableTheta,
			TEXT("Stable steps: θ of the implicit step on the water's stiffness beyond what the physics step follows (experimental). 1 backward Euler (default: the capped part of a bob dies within two steps), 0.5 midpoint (it rings at the step rate), between for some of each. Range 0.5-1 (values outside are clamped). Needs stable steps on (Stable Steps setting, fnb.Buoyancy.Implicit). The capped drag is backward Euler whatever this says."));
	}

	bool StabilizeStep(const FStepResult& Result, const FBodyState& Body, const FVector& InertiaDiagonal, float GravityZ, float DeltaSeconds,
		float AddedMass, const FVector& AddedInertiaDiagonal, float WaterAccelerationZ, FVector& InOutForce, FVector& InOutTorque,
		double* OutDragChangeZ, double* OutAddedChangeZ)
	{
		if (OutDragChangeZ)
		{
			*OutDragChangeZ = 0.0;
		}
		if (OutAddedChangeZ)
		{
			*OutAddedChangeZ = 0.0;
		}
		// What an explicit step follows well: a spring up to ω·dt = 0.5, a damping up to damping·dt/mass = 0.5. Only the stiffness and
		// damping beyond that are taken implicitly, so a body the step resolves keeps its motion exactly.
		constexpr double FollowedFrequency = 0.5;
		constexpr double FollowedDamping = 0.5;
		const double Dt = DeltaSeconds;
		const double Mass = Body.Mass;
		if (Dt <= 0.0 || Mass <= 0.0 || !Result.bTouchesWater)
		{
			return false;
		}
		// θ of the implicit spring (above). Under 0.5 the scheme is not stable for every stiffness, so it is held at 0.5 at least.
		const double Theta = FMath::Clamp(static_cast<double>(StableTheta), 0.5, 1.0);
		bool bChanged = false;

		// Along each world axis, the implicit step on the excess: the velocity change Δv solves
		//     m_eff Δv = dt (F + W + A a_w - c Δv - θ k dt (v + Δv)),   so   (m_eff + dt c + θ dt² k) Δv = dt (F + W + A a_w - θ dt k v),
		// with F the water's force at the step's start (the explicit spring and drag already in it), W the weight (m g, on Z), c and k
		// the excess damping and stiffness (the damping at the step's end velocity, the spring θ of the way along the solver's move
		// dt (v + Δv); the spring, heave, on Z only), and m_eff the mass the water accelerates: the body's plus, on Z, the heave added
		// mass A, the water that moves with the hull. That water's force on the body is -A (v̇ - a_w), against the body's acceleration
		// relative to the water's own, a_w (the water under a long wave accelerates with the wave and carries the body along, so the
		// added mass is no weight against it): none of it is in F, so it enters as A in m_eff and as A a_w on the right. The solver
		// knows only the body's own mass (the added mass cannot be given to the particle: it would fall with it, and it changes with
		// the wetted area every step), so the step is solved for the heavier system and the force returned, m Δv / dt - W, moves the
		// solver's own body to the same Δv once it adds gravity; at rest in equilibrium it is the force itself. The explicit part is
		// followed exactly: what the step follows is measured against m_eff too, so a heavier system keeps more of its spring explicit.
		FVector Force = InOutForce;
		double DragChangeZ = 0.0;
		double AddedChangeZ = 0.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Added = Axis == 2 ? FMath::Max(static_cast<double>(AddedMass), 0.0) : 0.0;
			const double Effective = Mass + Added;
			const double Spring = Axis == 2 ? FMath::Max(0.0, static_cast<double>(Result.HeaveStiffness) - Effective * FMath::Square(FollowedFrequency / Dt)) : 0.0;
			const double Damping = FMath::Max(0.0, Result.DragDamping[Axis] - Effective * FollowedDamping / Dt);
			if (Spring <= 0.0 && Damping <= 0.0 && Added <= 0.0)
			{
				continue;
			}
			const double Weight = Axis == 2 ? Mass * GravityZ : 0.0;
			const double Carried = Added * WaterAccelerationZ;   // the added mass pushed along with the water
			const double Change = Dt * (InOutForce[Axis] + Weight + Carried - Theta * Dt * Spring * Body.Velocity[Axis])
				/ (Effective + Dt * Damping + Theta * Dt * Dt * Spring);
			Force[Axis] = Mass * Change / Dt - Weight;
			if (Axis == 2)
			{
				// The change splits exactly into the drag at the end velocity, -c Δv, the spring along the step, -θ dt k (v + Δv), and
				// the added mass's A (a_w - Δv / dt) (rearrange the equation above). The drag's and the added mass's parts are the water's
				// dynamic push on the hull changing; the spring's is hydrostatic.
				DragChangeZ = -Damping * Change;
				AddedChangeZ = Added * (WaterAccelerationZ - Change / Dt);
			}
			bChanged = true;
		}

		// About each world axis, the same: (I_eff + dt c + θ dt² k) Δω = dt (τ - θ dt k ω), with the righting spring about X and Y and
		// I_eff the body's inertia plus the added inertia of its roll and pitch (the water's angular acceleration taken as 0); the torque
		// returned is I Δω / dt.
		FVector Torque = InOutTorque;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Inertia = InertiaDiagonal[Axis];
			if (Inertia <= 0.0)
			{
				continue;
			}
			const double Added = FMath::Max(AddedInertiaDiagonal[Axis], 0.0);
			const double Effective = Inertia + Added;
			const double Spring = Axis < 2 ? FMath::Max(0.0, Result.TiltStiffness[Axis] - Effective * FMath::Square(FollowedFrequency / Dt)) : 0.0;
			const double Damping = FMath::Max(0.0, Result.SpinDamping[Axis] - Effective * FollowedDamping / Dt);
			if (Spring <= 0.0 && Damping <= 0.0 && Added <= 0.0)
			{
				continue;
			}
			const double Change = Dt * (InOutTorque[Axis] - Theta * Dt * Spring * Body.AngularVelocity[Axis])
				/ (Effective + Dt * Damping + Theta * Dt * Dt * Spring);
			Torque[Axis] = Inertia * Change / Dt;
			bChanged = true;
		}
		if (!bChanged || Force.ContainsNaN() || Torque.ContainsNaN())
		{
			return false;
		}
		InOutForce = Force;
		InOutTorque = Torque;
		if (OutDragChangeZ)
		{
			*OutDragChangeZ = DragChangeZ;
		}
		if (OutAddedChangeZ)
		{
			*OutAddedChangeZ = AddedChangeZ;
		}
		return true;
	}

	namespace
	{
		FParams StaticParams(const FParams& Params)
		{
			FParams Static = Params;
			Static.SlamMultiplier = 0.f;
			Static.bPropulsion = false;
			Static.KeelArea = 0.f;
			Static.AirDragCoefficient = 0.f;
			Static.BuoyancyMultiplier = 1.f;
			return Static;
		}
	}

	void RestedExtent(const FHull& Hull, const FQuat& Attitude, float& OutBottom, float& OutTop)
	{
		double Bottom = TNumericLimits<double>::Max();
		double Top = -TNumericLimits<double>::Max();
		if (!Hull.Vertices.IsEmpty())
		{
			for (const FVector3f& Vertex : Hull.Vertices)
			{
				const double Z = Attitude.RotateVector(FVector(Vertex)).Z;
				Bottom = FMath::Min(Bottom, Z);
				Top = FMath::Max(Top, Z);
			}
		}
		else
		{
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector Point((Corner & 1) ? Hull.BoundsMax.X : Hull.BoundsMin.X, (Corner & 2) ? Hull.BoundsMax.Y : Hull.BoundsMin.Y, (Corner & 4) ? Hull.BoundsMax.Z : Hull.BoundsMin.Z);
				const double Z = Attitude.RotateVector(Point).Z;
				Bottom = FMath::Min(Bottom, Z);
				Top = FMath::Max(Top, Z);
			}
		}
		OutBottom = static_cast<float>(Bottom);
		OutTop = static_cast<float>(Top);
	}

	float DisplacedVolume(const FHull& Hull, float DraftCm, const FParams& Params, const FQuat& Attitude)
	{
		float Bottom = 0.f;
		float Top = 0.f;
		RestedExtent(Hull, Attitude, Bottom, Top);
		FBodyState Body;
		Body.Transform = FTransform(Attitude);
		Body.CenterOfMass = Attitude.RotateVector(FVector((Hull.BoundsMin + Hull.BoundsMax) * 0.5f));
		FWaterView Flat;
		Flat.bFlat = true;
		Flat.FlatHeight = Bottom + DraftCm;
		FStepState State;
		FStepResult Out;
		Integrate(Hull, Body, Flat, StaticParams(Params), 0.f, State, Out);
		return Out.DisplacedVolume;
	}

	float DraftForMass(const FHull& Hull, float MassKg, const FParams& Params, const FQuat& Attitude)
	{
		float Bottom = 0.f;
		float Top = 0.f;
		RestedExtent(Hull, Attitude, Bottom, Top);
		float Low = 0.f;
		float High = Top - Bottom;
		if (Params.Rho * DisplacedVolume(Hull, High, Params, Attitude) < MassKg)
		{
			return -1.f;   // sinks
		}
		for (int32 Step = 0; Step < 40; ++Step)
		{
			const float Mid = 0.5f * (Low + High);
			(Params.Rho * DisplacedVolume(Hull, Mid, Params, Attitude) < MassKg ? Low : High) = Mid;
		}
		return 0.5f * (Low + High);
	}

	float MetacentricHeight(const FHull& Hull, float MassKg, const FVector& CenterOfMassLocal, float DraftCm, int32 Axis, const FParams& Params, const FQuat& Attitude)
	{
		float Bottom = 0.f;
		float Top = 0.f;
		RestedExtent(Hull, Attitude, Bottom, Top);
		const double Heel = FMath::DegreesToRadians(3.0);
		const FQuat Rotation(Axis == 0 ? FVector::XAxisVector : FVector::YAxisVector, Heel);
		// Resting in its attitude, then heeled about its centre of mass.
		const FVector Centre = Attitude.RotateVector(CenterOfMassLocal);
		FBodyState Body;
		Body.Transform = FTransform(Rotation * Attitude, Centre - Rotation.RotateVector(Centre));
		Body.CenterOfMass = Centre;
		Body.Mass = MassKg;
		FWaterView Flat;
		Flat.bFlat = true;
		Flat.FlatHeight = Bottom + DraftCm;
		FStepState State;
		FStepResult Out;
		Integrate(Hull, Body, Flat, StaticParams(Params), 0.f, State, Out);
		const double Righting = -(Axis == 0 ? Out.Torque.X : Out.Torque.Y);
		return static_cast<float>(Righting / (MassKg * Params.Gravity * FMath::Sin(Heel)));
	}

	void FillPropellerSeries(float PitchRatio, FParams& Out)
	{
		const float Pitch = FMath::Clamp(PitchRatio, 0.4f, 2.f);
		const float J0 = 1.08f * Pitch;
		const float KT0 = 0.45f * FMath::Pow(Pitch, 1.2f);
		const float KQ0 = 0.062f * FMath::Pow(Pitch, 1.7f);
		Out.PropTableStep = 1.5f * J0 / static_cast<float>(PropTableSize - 1);
		for (int32 Index = 0; Index < PropTableSize; ++Index)
		{
			const float Share = FMath::Pow(Index * Out.PropTableStep / J0, 1.6f);
			Out.PropKT[Index] = KT0 * (1.f - Share);
			Out.PropKQ[Index] = KQ0 * (1.f - 0.76f * Share);
		}
	}

	float WaveMakingResistance(TArrayView<const FVector4f> Loads, float U, float Rho, float Gravity, float Depth, float KMax)
	{
		if (Loads.IsEmpty() || !(U > 1.f) || !(KMax > 0.f) || !(Rho > 0.f) || !(Gravity > 0.f))
		{
			return 0.f;
		}
		// The waves that keep station with the pressure run at k0 sec²θ in deep water (k0 = g / U²); none shorter than the pressure can make
		// (KMax), so θ is sampled in tan θ (sec²θ = 1 + tan²θ) out to where k0 sec²θ reaches KMax, by Simpson's rule.
		const double K0 = Gravity / (static_cast<double>(U) * U);
		if (K0 >= KMax)
		{
			return 0.f;   // too slow: the waves it would make are shorter than its pressure resolves
		}

		// The pressure on the grid of the shortest wave's half length, π / KMax (a hull's piece): the loads in each cell summed at their
		// |load|-weighted centre and spread over the cell's merged area (σ² = Σ σ_i², the disc of the summed areas), the loads under
		// WaveLoadSkipShare of the largest left out. The cells then go into WaveSpreadGroups groups by their spread, so the spread's
		// filter exp(-k² σ² / 2) is taken once per group, not once per load, and the phases by the float SinCos. A 5 m runabout at 25 cm
		// pieces: 318 loads into 122 cells, about 16 500 SinCos and 950 Exp an evaluation instead of 45 500 each of Exp, Sin and Cos, and
		// R within 1.3 % of the loads' own from 2 m/s up (below that it is under 20 N against a 690 N hump, made of the shortest waves the
		// cells only roughly keep). A smooth pressure ridge still comes within 1 % of Lamb's two-dimensional result from 3 m/s up, as before.
		float Largest = 0.f;
		for (const FVector4f& Load : Loads)
		{
			Largest = FMath::Max(Largest, FMath::Abs(Load.Z));
		}
		if (!(Largest > 0.f))
		{
			return 0.f;
		}
		const float Skip = WaveLoadSkipShare * Largest;
		const float Cell = PI / KMax;
		TMap<FIntPoint, int32>& CellOf = WaveCellOf;
		TArray<FWaveCell>& Cells = WaveCells;
		CellOf.Reset();
		Cells.Reset();
		for (const FVector4f& Load : Loads)
		{
			const float Weight = FMath::Abs(Load.Z);
			if (!(Weight >= Skip))
			{
				continue;
			}
			const FIntPoint Key(FMath::FloorToInt32(Load.X / Cell), FMath::FloorToInt32(Load.Y / Cell));
			int32 CellIndex = 0;
			if (const int32* Found = CellOf.Find(Key))
			{
				CellIndex = *Found;
			}
			else
			{
				CellIndex = Cells.AddDefaulted();
				CellOf.Add(Key, CellIndex);
			}
			FWaveCell& Merged = Cells[CellIndex];
			Merged.Load += Load.Z;
			Merged.Weight += Weight;
			Merged.X += static_cast<double>(Weight) * Load.X;
			Merged.Y += static_cast<double>(Weight) * Load.Y;
			Merged.Spread += static_cast<double>(Load.W) * Load.W;
		}
		double SpreadMax = 0.0;
		for (FWaveCell& Merged : Cells)
		{
			Merged.Spread = FMath::Sqrt(Merged.Spread);
			SpreadMax = FMath::Max(SpreadMax, Merged.Spread);
		}
		const double SpreadStep = SpreadMax / (WaveSpreadGroups - 1);
		int32 GroupStart[WaveSpreadGroups + 1] = {};
		for (FWaveCell& Merged : Cells)
		{
			Merged.Group = SpreadStep > 0.0 ? FMath::Clamp(FMath::RoundToInt32(Merged.Spread / SpreadStep), 0, WaveSpreadGroups - 1) : 0;
			++GroupStart[Merged.Group + 1];
		}
		int32 GroupFill[WaveSpreadGroups] = {};
		for (int32 Group = 0; Group < WaveSpreadGroups; ++Group)
		{
			GroupStart[Group + 1] += GroupStart[Group];
			GroupFill[Group] = GroupStart[Group];
		}
		// The cells as loads (x, y, the summed load), group by group.
		TArray<FVector4f>& Sorted = WaveCellLoads;
		Sorted.SetNumUninitialized(Cells.Num(), EAllowShrinking::No);
		// |P̂|² swings about once for every π of phase across the pressure, which reaches KMax × its length: enough samples for about eight
		// a swing (a 5 m hull of 25 cm pieces takes about 160), even for Simpson's rule.
		double Reach = 0.0;
		for (const FWaveCell& Merged : Cells)
		{
			const double X = Merged.X / Merged.Weight;
			const double Y = Merged.Y / Merged.Weight;
			Reach = FMath::Max(Reach, FMath::Max(FMath::Abs(X), FMath::Abs(Y)));
			Sorted[GroupFill[Merged.Group]++] = FVector4f(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Merged.Load), 0.f);
		}

		const double TanMax = FMath::Sqrt(KMax / K0 - 1.0);
		const int32 Intervals = 2 * FMath::Clamp(FMath::CeilToInt32(2.0 * KMax * Reach * 8.0 / (2.0 * UE_DOUBLE_PI)), 16, 128);
		const double Step = 2.0 * TanMax / Intervals;
		const bool bFinite = Depth > 0.f;
		double Sum = 0.0;
		for (int32 Index = 0; Index <= Intervals; ++Index)
		{
			const double Tan = -TanMax + Index * Step;
			const double Sec2 = 1.0 + Tan * Tan;
			const double Cos = 1.0 / FMath::Sqrt(Sec2);
			const double Sin = Tan * Cos;
			double K = K0 * Sec2;
			double Factor = 1.0;   // T / |T - kh sech²(kh)|: 1 in deep water
			if (bFinite && K * Depth < 12.0)
			{
				// g tanh(kh) = U² cos²θ k: no root where U cos θ passes the longest wave's speed √(gh) (no wave keeps station that way);
				// below it, one between 0 and the deep root, by bisection.
				const double A = static_cast<double>(U) * U * Cos * Cos;
				if (A >= Gravity * static_cast<double>(Depth))
				{
					continue;
				}
				double Low = 0.0;
				double High = K;
				for (int32 Iteration = 0; Iteration < 40; ++Iteration)
				{
					const double Mid = 0.5 * (Low + High);
					(Gravity * FMath::Tanh(Mid * Depth) - A * Mid > 0.0 ? Low : High) = Mid;
				}
				K = 0.5 * (Low + High);
				const double Tanh = FMath::Tanh(K * Depth);
				const double Slope = FMath::Abs(Tanh - K * Depth * (1.0 - Tanh * Tanh));
				Factor = FMath::Min(Tanh / FMath::Max(Slope, 1e-12), 10.0);
			}
			if (K > KMax)
			{
				continue;
			}
			double Real = 0.0;
			double Imaginary = 0.0;
			for (int32 Group = 0; Group < WaveSpreadGroups; ++Group)
			{
				if (GroupStart[Group] == GroupStart[Group + 1])
				{
					continue;
				}
				double GroupReal = 0.0;
				double GroupImaginary = 0.0;
				for (int32 Entry = GroupStart[Group]; Entry < GroupStart[Group + 1]; ++Entry)
				{
					const FVector4f& Load = Sorted[Entry];
					float Sine = 0.f;
					float Cosine = 0.f;
					FMath::SinCos(&Sine, &Cosine, static_cast<float>(K * (Load.X * Cos + Load.Y * Sin)));
					GroupReal += static_cast<double>(Load.Z) * Cosine;
					GroupImaginary += static_cast<double>(Load.Z) * Sine;
				}
				const double GroupSpread = Group * SpreadStep;
				const double Filter = FMath::Exp(-0.5 * K * K * GroupSpread * GroupSpread);
				Real += Filter * GroupReal;
				Imaginary += Filter * GroupImaginary;
			}
			const double Simpson = (Index == 0 || Index == Intervals) ? 1.0 : ((Index & 1) ? 4.0 : 2.0);
			Sum += Simpson * (Real * Real + Imaginary * Imaginary) * Cos * K * K * K * Factor / Sec2;   // dθ = d(tan θ) / sec²θ
		}
		const double Resistance = Sum * Step / 3.0 / (2.0 * UE_DOUBLE_PI * Rho * Gravity);
		return FMath::IsFinite(Resistance) ? static_cast<float>(FMath::Max(Resistance, 0.0)) : 0.f;
	}
}
