// FluidNinja Waves - emitter update (CPU). Switches the wave layer: sizes WaveGrid / WaveOp to the wave grid (1 x 1 when off), sets
// RT_Pressure's format (RGBA16f with waves, RG16f - the stock override - without), turns FluidNinja's unit-space scroll into a whole
// wave-cell shift plus the remainder, and copies the tuning into Emitter.Wave.*.
// Wave Aux runs only while User.WaveAuxTarget holds a render target of the wave grid's size (its reader sizes it; never resized here).
// Interface version 3: the wave grid is decoupled from the pressure buffer. User.Wave.ScaleDown (S = 1, 2 or 4) runs the wave stages on
// (RTW / S) x (RTH / S) cells of S pressure cells each, so a tick's Wave Prepare, Wave Step and Wave Aux cost 1 / S^2 of what they do on
// the pressure buffer's cells; Wave Export still writes every pressure texel. Emitter.Wave.ScaleDown, Emitter.Wave.Cells and
// Emitter.Wave.CellCm (cm) tell the stages and anything else on the emitter what the grid is. At S = 1 everything is what interface 2 did.
// Interface version 4: the foam channel F (Ninja_WaveStep). It needs a render target of the wave grid's size at User.WaveFoamTarget (its
// reader sizes it, as it sizes Wave Aux's; never resized here) and Wave Aux running: Emitter.Wave.FoamExport says the target is there
// (Wave Aux writes F into it, 0 while the foam is off), Emitter.Wave.FoamEnabled that User.Wave.Foam asks for it (Wave Prepare and Wave
// Step accumulate it). Its tuning goes into Emitter.Wave.Foam*. With no target bound (the 1 x 1 default) nothing runs.
// Interface version 5: Emitter.Wave.VMeanRelax, the closed-window <v> pin's rate (1/s) from User.Wave.VMeanRelax, only floored at 0 here,
// as Wave.MeanRelax is: Wave Step clamps both against the Dt it integrates with, next to the terms they scale. The asset default 0 keeps
// the pin off, so a plugin that never sets it runs interface 4's (eta, v).
// Interface version 6: the aged foam and the wash (Ninja_WaveStep), foam only. Emitter.Wave.FoamAgedDecay (1/s), .FoamTransfer (0..1),
// .FoamDiffusion and .FoamAgedDiffusion (cm2/s), .FoamSlope (cm/cm, 0 = the physics' Wave.BreakingSlope) and .FoamWash (scale) from the
// matching User.Wave.* parameters, floored at 0 (FoamTransfer clamped to 0..1). Nothing here touches the wave state's parameters.
int RTW;
int RTH;
RT_Pressure.GetRenderTargetSize(RTW, RTH);
bool Want = SimEnabled && RTW > 1 && RTH > 1;
// The integer division keeps the grid whole: a buffer that is not a multiple of S gets cells a little larger than S pressure cells, the
// window still covered end to end (the plugin divides the same way, so its source texture and Wave Aux target match the grid). Selects
// rather than int clamp / max: this module runs on the Niagara VM, and selects are what the module used already.
int Scale = InScaleDown < 1 ? 1 : (InScaleDown > 4 ? 4 : InScaleDown);
int DivX = RTW / Scale;
int DivY = RTH / Scale;
int GridX = Want ? (DivX < 1 ? 1 : DivX) : 1;
int GridY = Want ? (DivY < 1 ? 1 : DivY) : 1;
bool OkGrid;
bool OkOp;
bool OkFormat;
WaveGrid.SetNumCells(GridX, GridY, true, OkGrid);
WaveOp.SetRenderTargetSize(GridX, GridY, true, OkOp);
RT_Pressure.SetRenderTargetFormat(Want ? 6 : 5, OkFormat);
Enabled = Want && OkGrid && OkOp && OkFormat;
ExportEnabled = Enabled && ExportPD;
int AuxW;
int AuxH;
WaveAux.GetRenderTargetSize(AuxW, AuxH);
AuxEnabled = Enabled && AuxW == GridX && AuxH == GridY;
float2 Accumulated = Enabled ? (PrevRemainder + OffsetDelta.xy * float2(GridX, GridY)) : float2(0.0, 0.0);
float2 Whole = floor(Accumulated + 0.5);
Scroll = Whole;
Remainder = Accumulated - Whole;
ScaleDown = Scale;
Cells = float2(GridX, GridY);
CellCm = 0.5 * (Extents.x / float(GridX) + Extents.y / float(GridY));
DepthCm = max(InDepthCm, 1.0);
DepthFromBed = InDepthFromBed;
Damping = max(InDamping, 0.0);
SpongeCm = max(InSpongeCm, 0.0);
WallAbsorption = max(InWallAbsorption, 0.0);
WallBandCm = max(InWallBandCm, 0.0);
EdgeOpen = InEdgeOpen;
ShoreDepthCm = max(InShoreDepthCm, 0.0);
BreakingSlope = InBreakingSlope;
Hyperviscosity = clamp(InHyperviscosity, 0.0, 50.0);
CurrentCoupling = InCurrentCoupling;
MeanRelax = max(InMeanRelax, 0.0);
VMeanRelax = max(InVMeanRelax, 0.0);
int FoamW;
int FoamH;
WaveFoam.GetRenderTargetSize(FoamW, FoamH);
FoamExport = AuxEnabled && FoamW == GridX && FoamH == GridY;
FoamEnabled = FoamExport && InFoam > 0.5;
FoamDecay = max(InFoamDecay, 0.0);
FoamBreaking = max(InFoamBreaking, 0.0);
FoamWake = max(InFoamWake, 0.0);
FoamImpact = max(InFoamImpact, 0.0);
FoamAgedDecay = max(InFoamAgedDecay, 0.0);
FoamTransfer = clamp(InFoamTransfer, 0.0, 1.0);
FoamDiffusion = max(InFoamDiffusion, 0.0);
FoamAgedDiffusion = max(InFoamAgedDiffusion, 0.0);
FoamSlope = max(InFoamSlope, 0.0);
FoamWash = max(InFoamWash, 0.0);
