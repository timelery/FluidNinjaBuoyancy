#include "FluidNinjaBuoyancySetupCheck.h"

#include "FluidNinjaBuoyancyEditorLog.h"
#include "FluidNinjaBuoyancySettings.h"
#include "FluidNinjaWaveSimComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Text.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "Materials/MaterialInterface.h"
#include "Misc/UObjectToken.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "ScopedTransaction.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "FluidNinjaBuoyancySetupCheck"

const FName FluidNinjaBuoyancySetupCheck::LogName(TEXT("FluidNinjaBuoyancy"));

namespace
{
	/** FluidNinja LIVE's simulation system at its default location. */
	const TCHAR* CorePath = TEXT("/Game/FluidNinjaLive/NinjaLiveCore.NinjaLiveCore");
	/** The core's interface this plugin build speaks and the first build that has it: FluidNinjaBuoyancyWaves.cpp's WaveRequiredInterfaceVersion
	 * and WaveRequiredCoreBuild (keep them together), read here from the asset's exposed parameters as the waves read them from the core. */
	constexpr int32 RequiredInterfaceVersion = 2;
	constexpr int32 RequiredCoreBuild = 20261017;
	const FName WaveSimEnabledName(TEXT("User.WaveSimEnabled"));
	const FName WaveAuxTargetName(TEXT("User.WaveAuxTarget"));
	const FName WaveInterfaceVersionName(TEXT("User.Wave.InterfaceVersion"));
	const FName WaveCoreBuildName(TEXT("User.Wave.CoreBuild"));
	/** The NinjaLive component's switch for the plugin's wake points (FluidNinja's data channel reader), and the property naming the tag of
	 * the actors it draws its second output material on (the surface actors, which the plugin floats bodies on). FluidNinja's own names. */
	const FName DataChannelReaderProperty(TEXT("DataChannelPointReaderEnabled"));
	const FName SurfaceTagProperty(TEXT("Apply2ndOutMatToActorsWithTag"));
	/** The plugin's own surface tag (Project Settings → FluidNinja Buoyancy → Surface Tags ships it). */
	const FName PluginSurfaceTag(TEXT("FluidNinjaBuoyancySurface"));

	/** A live component of a NinjaLive actor: Blueprint reinstancing can leave unregistered TRASH_ / REINST_ copies behind. */
	bool IsLiveComponent(const UActorComponent* Component)
	{
		if (!Component || !Component->IsRegistered())
		{
			return false;
		}
		const FString Name = Component->GetName();
		return !Name.StartsWith(TEXT("TRASH_")) && !Name.StartsWith(TEXT("REINST_"));
	}

	/** A NinjaLive actor and its live components: where its settings are. */
	TArray<UObject*> NinjaHolders(AActor& Actor)
	{
		TArray<UObject*> Holders = {&Actor};
		for (UActorComponent* Component : Actor.GetComponents())
		{
			if (IsLiveComponent(Component))
			{
				Holders.Add(Component);
			}
		}
		return Holders;
	}

	/** A NinjaLive actor, as the subsystem knows one: something on it (the actor or a component) has FluidNinja's tracking-tag property. */
	bool IsNinjaLive(AActor& Actor, const UFluidNinjaBuoyancySettings& Settings)
	{
		for (const UObject* Holder : NinjaHolders(Actor))
		{
			if (FindFProperty<FNameProperty>(Holder->GetClass(), Settings.TrackingTagProperty))
			{
				return true;
			}
		}
		return false;
	}

	/** A NinjaLive's simulations: its live Niagara components running NinjaLiveCore. */
	TArray<UNiagaraComponent*> NinjaSimulations(AActor& Actor)
	{
		TArray<UNiagaraComponent*> Simulations;
		for (UActorComponent* Component : Actor.GetComponents())
		{
			UNiagaraComponent* Simulation = IsLiveComponent(Component) ? Cast<UNiagaraComponent>(Component) : nullptr;
			if (Simulation && Simulation->GetAsset() && Simulation->GetAsset()->GetName() == TEXT("NinjaLiveCore"))
			{
				Simulations.Add(Simulation);
			}
		}
		return Simulations;
	}

	/** A user parameter of a core's system, by type and name: in the asset's exposed parameters. */
	bool SystemHasParameter(const UNiagaraSystem& System, const FNiagaraVariable& Variable)
	{
		return System.GetExposedParameters().IndexOf(Variable) != INDEX_NONE;
	}

	/** An int32 user parameter of the asset, or Fallback (as UFluidNinjaBuoyancyWaves reads a core: the instance's value, else the system's default). */
	int32 ReadSystemInt(const UNiagaraSystem& System, const FName& Name, int32 Fallback)
	{
		const FNiagaraVariable Variable(FNiagaraTypeDefinition::GetIntDef(), Name);
		const FNiagaraParameterStore& Exposed = System.GetExposedParameters();
		return Exposed.IndexOf(Variable) != INDEX_NONE ? Exposed.GetParameterValue<int32>(Variable) : Fallback;
	}

	int32 ReadCoreInt(UNiagaraComponent& Core, const FName& Name, int32 Fallback)
	{
		const FNiagaraVariable Variable(FNiagaraTypeDefinition::GetIntDef(), Name);
		const FNiagaraParameterStore& Overrides = Core.GetOverrideParameters();
		if (Overrides.IndexOf(Variable) != INDEX_NONE)
		{
			return Overrides.GetParameterValue<int32>(Variable);
		}
		return Core.GetAsset() ? ReadSystemInt(*Core.GetAsset(), Name, Fallback) : Fallback;
	}

	int64 ReadInt(const FProperty& Property, const UObject& Holder)
	{
		const void* Value = Property.ContainerPtrToValuePtr<void>(&Holder);
		if (const FNumericProperty* Number = CastField<FNumericProperty>(&Property))
		{
			return Number->IsFloatingPoint() ? FMath::RoundToInt64(Number->GetFloatingPointPropertyValue(Value)) : Number->GetSignedIntPropertyValue(Value);
		}
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(&Property))
		{
			return Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
		}
		return 0;
	}

	void WriteInt(const FProperty& Property, UObject& Holder, int64 NewValue)
	{
		void* Value = Property.ContainerPtrToValuePtr<void>(&Holder);
		if (const FNumericProperty* Number = CastField<FNumericProperty>(&Property))
		{
			if (Number->IsFloatingPoint())
			{
				Number->SetFloatingPointPropertyValue(Value, static_cast<double>(NewValue));
			}
			else
			{
				Number->SetIntPropertyValue(Value, NewValue);
			}
		}
		else if (const FEnumProperty* Enum = CastField<FEnumProperty>(&Property))
		{
			Enum->GetUnderlyingProperty()->SetIntPropertyValue(Value, NewValue);
		}
	}

	bool IsNumeric(const FProperty* Property)
	{
		return CastField<FNumericProperty>(Property) || CastField<FEnumProperty>(Property);
	}

	/** A NinjaLive's say on whether its tracking paints velocity relative to the water (as the subsystem reads it): a bool or number
	 * property named Switch on the actor or a component, or a float user parameter of its simulation; on above 0.5. */
	bool ReadTrackingSwitch(AActor& Actor, FName Switch, FString& OutHow)
	{
		for (const UObject* Holder : NinjaHolders(Actor))
		{
			if (const FBoolProperty* Flag = FindFProperty<FBoolProperty>(Holder->GetClass(), Switch))
			{
				OutHow = FString::Printf(TEXT("%s.%s"), *Holder->GetName(), *Switch.ToString());
				return Flag->GetPropertyValue_InContainer(Holder);
			}
			if (const FNumericProperty* Number = FindFProperty<FNumericProperty>(Holder->GetClass(), Switch))
			{
				OutHow = FString::Printf(TEXT("%s.%s"), *Holder->GetName(), *Switch.ToString());
				const void* Value = Number->ContainerPtrToValuePtr<void>(Holder);
				return (Number->IsFloatingPoint() ? Number->GetFloatingPointPropertyValue(Value) : static_cast<double>(Number->GetSignedIntPropertyValue(Value))) > 0.5;
			}
		}
		const FNiagaraVariable Parameter(FNiagaraTypeDefinition::GetFloatDef(), FName(*(TEXT("User.") + Switch.ToString())));
		for (const UNiagaraComponent* Simulation : NinjaSimulations(Actor))
		{
			if (Simulation->GetOverrideParameters().IndexOf(Parameter) != INDEX_NONE)
			{
				OutHow = FString::Printf(TEXT("%s user parameter %s"), *Simulation->GetName(), *Switch.ToString());
				return Simulation->GetOverrideParameters().GetParameterValue<float>(Parameter) > 0.5f;
			}
		}
		OutHow = FString::Printf(TEXT("no %s switch"), *Switch.ToString());
		return false;
	}

	/** The holder and property of the NinjaLive's tracked-density setting (FluidNinja's Selectively Kill Brush Density Keep Velocity), its
	 * value, and whether that leaves tracked bodies' density out (1, 4 or 5). */
	bool ReadTrackedDensity(AActor& Actor, FName Switch, UObject*& OutHolder, const FProperty*& OutProperty, int64& OutValue)
	{
		OutHolder = nullptr;
		OutProperty = nullptr;
		OutValue = 0;
		for (UObject* Holder : NinjaHolders(Actor))
		{
			const FProperty* Property = FindFProperty<FProperty>(Holder->GetClass(), Switch);
			if (IsNumeric(Property))
			{
				OutHolder = Holder;
				OutProperty = Property;
				OutValue = ReadInt(*Property, *Holder);
				return OutValue == 1 || OutValue == 4 || OutValue == 5;
			}
		}
		return false;
	}

	/** The holder of a bool property of this name on the NinjaLive, and its value. */
	bool ReadBoolSwitch(AActor& Actor, FName Name, UObject*& OutHolder, const FBoolProperty*& OutProperty)
	{
		OutHolder = nullptr;
		OutProperty = nullptr;
		for (UObject* Holder : NinjaHolders(Actor))
		{
			if (const FBoolProperty* Flag = FindFProperty<FBoolProperty>(Holder->GetClass(), Name))
			{
				OutHolder = Holder;
				OutProperty = Flag;
				return Flag->GetPropertyValue_InContainer(Holder);
			}
		}
		return false;
	}

	FName ReadNameProperty(AActor& Actor, FName Name, bool& bOutFound)
	{
		bOutFound = false;
		FName Value = NAME_None;
		for (const UObject* Holder : NinjaHolders(Actor))
		{
			if (const FNameProperty* Property = FindFProperty<FNameProperty>(Holder->GetClass(), Name))
			{
				bOutFound = true;
				const FName Read = Property->GetPropertyValue_InContainer(Holder);
				Value = Read.IsNone() ? Value : Read;
			}
		}
		return Value;
	}

	bool HasSurfaceTag(const AActor& Actor, const UFluidNinjaBuoyancySettings& Settings)
	{
		for (const FName& Tag : Settings.SurfaceTags)
		{
			if (Actor.ActorHasTag(Tag))
			{
				return true;
			}
		}
		return false;
	}

	/** The tag the Fix adds to a surface actor: the plugin's own when the project still lists it, else the first the project does. */
	FName FixSurfaceTag(const UFluidNinjaBuoyancySettings& Settings)
	{
		return Settings.SurfaceTags.Contains(PluginSurfaceTag) || Settings.SurfaceTags.IsEmpty() ? PluginSurfaceTag : Settings.SurfaceTags[0];
	}

	/** Whether the Surface Mirror would draw this primitive (RefreshSources' rule: horizontal spline and static mesh pieces, a Niagara sheet). */
	bool IsSurfacePrimitive(const UPrimitiveComponent& Primitive, bool bNiagaraActor)
	{
		if (!Primitive.IsVisible() || Primitive.bHiddenInGame)
		{
			return false;
		}
		if (const USplineMeshComponent* Piece = Cast<const USplineMeshComponent>(&Primitive))
		{
			const FTransform& Transform = Piece->GetComponentTransform();
			const FVector Run = Transform.TransformPosition(Piece->GetEndPosition()) - Transform.TransformPosition(Piece->GetStartPosition());
			return FMath::Abs(Run.Z) <= Run.Size2D();
		}
		if (Primitive.IsA<UStaticMeshComponent>())
		{
			const FVector Extent = Primitive.Bounds.BoxExtent;
			return Extent.Z <= FMath::Max(Extent.X, Extent.Y);
		}
		return bNiagaraActor && Primitive.IsA<UNiagaraComponent>();
	}

	/** The first surface material of an actor (the material the Surface Mirror and the flow sources read). */
	UMaterialInterface* SurfaceMaterial(AActor& Actor)
	{
		TInlineComponentArray<UPrimitiveComponent*> Primitives(&Actor);
		for (const UPrimitiveComponent* Primitive : Primitives)
		{
			if (IsLiveComponent(Primitive) && !Primitive->IsA<UNiagaraComponent>() && IsSurfacePrimitive(*Primitive, false))
			{
				if (UMaterialInterface* Material = Primitive->GetMaterial(0))
				{
					return Material;
				}
			}
		}
		return nullptr;
	}

	/** The page's lines: PASS, FAIL or WARN, the actor it is about, the detail, and a Fix where one is offered. */
	struct FReport
	{
		FMessageLog Log;
		int32 Failures = 0;
		int32 Warnings = 0;
		int32 Passes = 0;

		explicit FReport(const FName& InLogName) : Log(InLogName) {}

		TSharedRef<FTokenizedMessage> Line(EMessageSeverity::Type Severity, const FString& Detail, AActor* Actor = nullptr, const UObject* Asset = nullptr)
		{
			const TCHAR* Verdict = Severity == EMessageSeverity::Error ? TEXT("FAIL") : Severity == EMessageSeverity::Warning ? TEXT("WARN") : TEXT("PASS");
			(Severity == EMessageSeverity::Error ? Failures : Severity == EMessageSeverity::Warning ? Warnings : Passes) += 1;
			TSharedRef<FTokenizedMessage> Message = FTokenizedMessage::Create(Severity, FText::FromString(Verdict));
			if (Actor)
			{
				Message->AddToken(FActorToken::Create(Actor->GetPathName(), Actor->GetActorGuid(), FText::FromString(Actor->GetActorNameOrLabel())));
			}
			if (Asset)
			{
				Message->AddToken(FUObjectToken::Create(Asset));
			}
			Message->AddToken(FTextToken::Create(FText::FromString(Detail)));
			Log.AddMessage(Message);
			return Message;
		}

		/** A Fix action on the line: what it does, as the user sees it, and the write. A fix in Play changes the Play world's copies. */
		static void Fix(const TSharedRef<FTokenizedMessage>& Message, const FText& What, TFunction<void()> Action)
		{
			Message->AddToken(FActionToken::Create(LOCTEXT("Fix", "Fix"), What, FOnActionTokenExecuted::CreateLambda([What, Action = MoveTemp(Action)]
			{
				const FScopedTransaction Transaction(FText::Format(LOCTEXT("FixTransaction", "FluidNinja Buoyancy: {0}"), What));
				Action();
				UE_LOG(LogFluidNinjaBuoyancyEditor, Display, TEXT("Check Setup fix: %s"), *What.ToString());
			}), /*bInSingleUse*/ true));
		}
	};

	/** (a) The modified core: the asset at FluidNinja's default location, its waves interface and build, its relative-tracking parameter. */
	UNiagaraSystem* CheckCore(FReport& Report, const UFluidNinjaBuoyancySettings& Settings)
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, CorePath);
		if (!System)
		{
			Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("FluidNinja LIVE's NinjaLiveCore is not at %s: install FluidNinja LIVE at its default location (Content/FluidNinjaLive), with the modified core (the waves zip)."), CorePath));
			return nullptr;
		}
		const bool bWaves = SystemHasParameter(*System, FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), WaveSimEnabledName));
		const bool bAux = SystemHasParameter(*System, FNiagaraVariable(FNiagaraTypeDefinition(UTextureRenderTarget::StaticClass()), WaveAuxTargetName));
		const int32 InterfaceVersion = ReadSystemInt(*System, WaveInterfaceVersionName, 1);
		const int32 CoreBuild = ReadSystemInt(*System, WaveCoreBuildName, 0);
		const bool bRelative = SystemHasParameter(*System, FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(*(TEXT("User.") + Settings.RelativeTrackingSwitch.ToString()))));
		if (!bWaves)
		{
			Report.Line(EMessageSeverity::Error, TEXT("is a stock NinjaLiveCore (no User.WaveSimEnabled): no simulation makes waves. Copy the modified core in (the waves zip's README.txt), or re-apply it with Tools/CorePatch/apply_fluidninja_waves.py."), nullptr, System);
		}
		else if (!bAux)
		{
			Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("is an older FluidNinja Waves core (no User.WaveAuxTarget): the bodies' own fields need the Wave Aux stage. Re-apply the waves (core build %d, interface %d; this plugin needs interface %d, core build %d or later)."), CoreBuild, InterfaceVersion, RequiredInterfaceVersion, RequiredCoreBuild), nullptr, System);
		}
		else if (InterfaceVersion < RequiredInterfaceVersion || CoreBuild < RequiredCoreBuild)
		{
			Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("is FluidNinja Waves core build %d, interface %d; this plugin needs interface %d, core build %d or later: install the current zip's core."), CoreBuild, InterfaceVersion, RequiredInterfaceVersion, RequiredCoreBuild), nullptr, System);
		}
		else
		{
			Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("is the modified NinjaLiveCore: FluidNinja Waves core build %d, interface %d (this plugin needs interface %d, build %d or later)."), CoreBuild, InterfaceVersion, RequiredInterfaceVersion, RequiredCoreBuild), nullptr, System);
		}
		if (bRelative)
		{
			Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("has the relative-tracking user parameter %s: tracked bodies can paint their velocity relative to the water."), *Settings.RelativeTrackingSwitch.ToString()), nullptr, System);
		}
		else
		{
			Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("has no user parameter %s: the relative-tracking half of the modified core is missing, so no body is handed to FluidNinja's tracking (Docs/RelativeTrackingProposal.md, The code)."), *Settings.RelativeTrackingSwitch.ToString()), nullptr, System);
		}
		return System;
	}

	/** (b) One NinjaLive actor: its switches, each with a Fix, and the surfaces it draws on. */
	void CheckNinjaLive(FReport& Report, AActor& Ninja, UWorld& World, const UFluidNinjaBuoyancySettings& Settings, const UNiagaraSystem* ExpectedCore, const TArray<AActor*>& AllActors)
	{
		const TWeakObjectPtr<AActor> WeakNinja(&Ninja);

		// Its simulation's core: the one checked above, or another asset checked the same way.
		TArray<UNiagaraComponent*> Simulations = NinjaSimulations(Ninja);
		if (Simulations.IsEmpty())
		{
			Report.Line(EMessageSeverity::Error, TEXT("has no Niagara component running NinjaLiveCore: the plugin finds a simulation's core by that system's name."), &Ninja);
		}
		for (UNiagaraComponent* Simulation : Simulations)
		{
			UNiagaraSystem* Asset = Simulation->GetAsset();
			if (Asset && Asset != ExpectedCore)
			{
				const bool bWaves = SystemHasParameter(*Asset, FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), WaveSimEnabledName));
				const int32 InterfaceVersion = ReadCoreInt(*Simulation, WaveInterfaceVersionName, 1);
				const int32 CoreBuild = ReadCoreInt(*Simulation, WaveCoreBuildName, 0);
				if (!bWaves || InterfaceVersion < RequiredInterfaceVersion)
				{
					Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("runs %s, %s (core build %d, interface %d): this simulation makes no waves."), *Asset->GetPathName(),
						bWaves ? TEXT("an older FluidNinja Waves core") : TEXT("a stock core without User.WaveSimEnabled"), CoreBuild, InterfaceVersion), &Ninja, Asset);
				}
				else
				{
					Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("runs %s rather than the default core: FluidNinja Waves core build %d, interface %d."), *Asset->GetPathName(), CoreBuild, InterfaceVersion), &Ninja, Asset);
				}
			}
		}

		// Relative tracking: the core's user parameter (the modified FluidNinja's), set on the NinjaLiveCore component, as the bench sets it.
		{
			FString How;
			const bool bRelative = ReadTrackingSwitch(Ninja, Settings.RelativeTrackingSwitch, How);
			if (bRelative)
			{
				Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("tracks bodies relative to the water (%s)."), *How), &Ninja);
			}
			else
			{
				const TSharedRef<FTokenizedMessage> Line = Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("paints tracked bodies' world velocity (%s): a floater carried by a current then speeds that current up, and the plugin hands no body to FluidNinja's tracking. Set the NinjaLiveCore user parameter %s to 1."), *How, *Settings.RelativeTrackingSwitch.ToString()), &Ninja);
				if (!Simulations.IsEmpty())
				{
					const FName Switch = Settings.RelativeTrackingSwitch;
					FReport::Fix(Line, FText::Format(LOCTEXT("FixRelative", "set {0} to 1 on {1}'s NinjaLiveCore"), FText::FromName(Switch), FText::FromString(Ninja.GetActorNameOrLabel())), [WeakNinja, Switch]
					{
						if (AActor* Actor = WeakNinja.Get())
						{
							for (UNiagaraComponent* Simulation : NinjaSimulations(*Actor))
							{
								Simulation->Modify();
								Simulation->SetVariableFloat(Switch, 1.f);
							}
						}
					});
				}
			}
		}

		// Tracked density: FluidNinja's Selectively Kill Brush Density Keep Velocity at 1, 4 or 5 (the property on the NinjaLive component; in
		// Play the core's Painter parameter as well, as the bench sets it).
		{
			UObject* Holder = nullptr;
			const FProperty* Property = nullptr;
			int64 Value = 0;
			const bool bDensityOff = ReadTrackedDensity(Ninja, Settings.TrackedDensitySwitch, Holder, Property, Value);
			if (!Holder)
			{
				Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("has no '%s' setting (Project Settings → FluidNinja Buoyancy → Tracked Density Switch): is this FluidNinja LIVE 2.0.0.56?"), *Settings.TrackedDensitySwitch.ToString()), &Ninja);
			}
			else if (bDensityOff)
			{
				Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("leaves tracked bodies' density out (%s.%s %lld)."), *Holder->GetName(), *Settings.TrackedDensitySwitch.ToString(), Value), &Ninja);
			}
			else
			{
				const TSharedRef<FTokenizedMessage> Line = Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("lets tracked bodies paint density (%s.%s %lld): FluidNinja's current pushes harder wherever floaters go. Set it to 1 (ARRAY), 4 or 5."), *Holder->GetName(), *Settings.TrackedDensitySwitch.ToString(), Value), &Ninja);
				const TWeakObjectPtr<UObject> WeakHolder(Holder);
				const FName Switch = Settings.TrackedDensitySwitch;
				const bool bGameWorld = World.IsGameWorld();
				FReport::Fix(Line, FText::Format(LOCTEXT("FixDensity", "set {0} to 1 on {1}"), FText::FromName(Switch), FText::FromString(Ninja.GetActorNameOrLabel())), [WeakHolder, WeakNinja, Switch, bGameWorld]
				{
					UObject* Target = WeakHolder.Get();
					const FProperty* Found = Target ? FindFProperty<FProperty>(Target->GetClass(), Switch) : nullptr;
					if (Target && IsNumeric(Found))
					{
						Target->Modify();
						WriteInt(*Found, *Target, 1);
					}
					if (AActor* Actor = bGameWorld ? WeakNinja.Get() : nullptr)
					{
						// NinjaLive passes its setting to the core at its next parameter update; in Play this takes effect now.
						const FName PainterDensity(*(TEXT("Painter.") + Switch.ToString()));
						for (UNiagaraComponent* Simulation : NinjaSimulations(*Actor))
						{
							Simulation->SetVariableInt(PainterDensity, 1);
						}
					}
				});
			}
		}

		// The data channel reader: the wake points (the floating bodies' and the swimmers') reach FluidNinja only through it.
		{
			UObject* Holder = nullptr;
			const FBoolProperty* Property = nullptr;
			const bool bReader = ReadBoolSwitch(Ninja, DataChannelReaderProperty, Holder, Property);
			if (!Holder)
			{
				Report.Line(EMessageSeverity::Warning, FString::Printf(TEXT("has no '%s' property: the wake points (Wake on a body, Swimmer Wakes) need FluidNinja's data channel reader on; check it by hand."), *DataChannelReaderProperty.ToString()), &Ninja);
			}
			else if (bReader)
			{
				Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("reads the wake points (%s.%s on)."), *Holder->GetName(), *DataChannelReaderProperty.ToString()), &Ninja);
			}
			else
			{
				const TSharedRef<FTokenizedMessage> Line = Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("ignores the wake points (%s.%s off): bodies with Wake and the swimmers' wakes and plunge rings reach FluidNinja only through its data channel reader."), *Holder->GetName(), *DataChannelReaderProperty.ToString()), &Ninja);
				const TWeakObjectPtr<UObject> WeakHolder(Holder);
				FReport::Fix(Line, FText::Format(LOCTEXT("FixReader", "turn {0} on for {1}"), FText::FromName(DataChannelReaderProperty), FText::FromString(Ninja.GetActorNameOrLabel())), [WeakHolder]
				{
					UObject* Target = WeakHolder.Get();
					const FBoolProperty* Found = Target ? FindFProperty<FBoolProperty>(Target->GetClass(), DataChannelReaderProperty) : nullptr;
					if (Found)
					{
						Target->Modify();
						Found->SetPropertyValue_InContainer(Target, true);
					}
				});
			}
		}

		// The surfaces it draws on: the actors tagged with its second output material's tag, else any actor whose surface material shows its
		// velocity buffer (the subsystem's own route to a simulation). Each must carry a surface tag for the plugin, and its material must have
		// the pressure buffer the waves are read from.
		{
			bool bFoundProperty = false;
			const FName OutputTag = ReadNameProperty(Ninja, SurfaceTagProperty, bFoundProperty);
			TArray<AActor*> Surfaces;
			if (!OutputTag.IsNone())
			{
				for (AActor* Actor : AllActors)
				{
					if (Actor != &Ninja && Actor->ActorHasTag(OutputTag))
					{
						Surfaces.Add(Actor);
					}
				}
			}
			if (Surfaces.IsEmpty())
			{
				for (AActor* Actor : AllActors)
				{
					UMaterialInterface* Material = Actor != &Ninja ? SurfaceMaterial(*Actor) : nullptr;
					UTexture* Texture = nullptr;
					if (Material && Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Settings.VelocityBufferParameter), Texture) && Texture && Texture->GetTypedOuter<AActor>() == &Ninja)
					{
						Surfaces.Add(Actor);
					}
				}
			}
			if (Surfaces.IsEmpty())
			{
				Report.Line(EMessageSeverity::Warning, FString::Printf(TEXT("no surface actor found for it (%s, and no surface material shows its velocity buffer in this world): the plugin floats bodies on the surface actor carrying one of the Surface Tags, so check that one by hand."),
					OutputTag.IsNone() ? (bFoundProperty ? *FString::Printf(TEXT("its %s is None"), *SurfaceTagProperty.ToString()) : *FString::Printf(TEXT("no %s property"), *SurfaceTagProperty.ToString())) : *FString::Printf(TEXT("nothing is tagged %s"), *OutputTag.ToString())), &Ninja);
			}
			for (AActor* Surface : Surfaces)
			{
				if (HasSurfaceTag(*Surface, Settings))
				{
					Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("draws on %s, which carries a surface tag."), *Surface->GetActorNameOrLabel()), &Ninja);
				}
				else
				{
					const FName Tag = FixSurfaceTag(Settings);
					const TSharedRef<FTokenizedMessage> Line = Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("draws on %s, which carries none of the Surface Tags (%s): the Surface Mirror will not see this water and bodies fall through it."),
						*Surface->GetActorNameOrLabel(), *FString::JoinBy(Settings.SurfaceTags, TEXT(", "), [](const FName& Name) { return Name.ToString(); })), &Ninja);
					const TWeakObjectPtr<AActor> WeakSurface(Surface);
					FReport::Fix(Line, FText::Format(LOCTEXT("FixSurfaceTag", "tag {0} with {1}"), FText::FromString(Surface->GetActorNameOrLabel()), FText::FromName(Tag)), [WeakSurface, Tag]
					{
						if (AActor* Actor = WeakSurface.Get())
						{
							Actor->Modify();
							Actor->Tags.AddUnique(Tag);
						}
					});
				}
				UMaterialInterface* Material = SurfaceMaterial(*Surface);
				UTexture* Pressure = nullptr;
				if (!Material)
				{
					Report.Line(EMessageSeverity::Warning, FString::Printf(TEXT("%s has no horizontal mesh with a material: nothing there for the Surface Mirror to draw."), *Surface->GetActorNameOrLabel()), &Ninja);
				}
				else if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Settings.PressureBufferParameter), Pressure))
				{
					Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("%s's material %s has the pressure buffer parameter %s%s: the waves can be read from it."), *Surface->GetActorNameOrLabel(), *Material->GetName(), *Settings.PressureBufferParameter.ToString(),
						Pressure ? *FString::Printf(TEXT(" (%s)"), *Pressure->GetName()) : TEXT("")), &Ninja);
				}
				else
				{
					Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("%s's material %s has no texture parameter %s (Project Settings → FluidNinja Buoyancy → Pressure Buffer Parameter): it is not FluidNinja's SURFACE output material from the waves zip, so this simulation's waves cannot be read and its floaters feel none."),
						*Surface->GetActorNameOrLabel(), *Material->GetName(), *Settings.PressureBufferParameter.ToString()), &Ninja);
				}
			}
		}
	}

	/** (c) The Wave Sims: every ini entry names a NinjaLive in the level, every component sits on one, no actor is set up twice. */
	void CheckWaveSims(FReport& Report, const TArray<AActor*>& Ninjas, const TArray<AActor*>& AllActors, const UFluidNinjaBuoyancySettings& Settings)
	{
		if (!Settings.bWaves)
		{
			Report.Line(EMessageSeverity::Warning, TEXT("Waves are off (Project Settings → FluidNinja Buoyancy → Waves): no simulation makes waves, and the Wave Sims are not used."));
		}
		TMap<AActor*, TArray<FString>> Sources;   // who sets each NinjaLive up
		for (int32 Index = 0; Index < Settings.WaveSims.Num(); ++Index)
		{
			const FFluidNinjaWaveSimSettings& Entry = Settings.WaveSims[Index];
			if (Entry.NinjaLive.IsNone())
			{
				Report.Line(EMessageSeverity::Warning, FString::Printf(TEXT("Wave Sims entry %d names no NinjaLive: it does nothing."), Index));
				continue;
			}
			TArray<AActor*> Named;
			for (AActor* Ninja : Ninjas)
			{
				if (Entry.NinjaLive == Ninja->GetFName() || Entry.NinjaLive.ToString() == Ninja->GetActorNameOrLabel())
				{
					Named.Add(Ninja);
				}
			}
			if (Named.IsEmpty())
			{
				TArray<FString> Names;
				for (const AActor* Ninja : Ninjas)
				{
					Names.Add(Ninja->GetActorNameOrLabel());
				}
				Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("Wave Sims entry '%s' names no NinjaLive in this level (%s): the entry does nothing, so that water runs at the project's Wave Depth with open edges. Rename it to the actor's name or label."),
					*Entry.NinjaLive.ToString(), Names.IsEmpty() ? TEXT("there is none") : *FString::Printf(TEXT("the NinjaLives are %s"), *FString::Join(Names, TEXT(", ")))));
				continue;
			}
			for (AActor* Ninja : Named)
			{
				Sources.FindOrAdd(Ninja).Add(FString::Printf(TEXT("Wave Sims entry '%s'"), *Entry.NinjaLive.ToString()));
				Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("Wave Sims entry '%s' sets it up%s%s%s."), *Entry.NinjaLive.ToString(),
					Entry.bEnabled ? TEXT("") : TEXT(" (waves off)"), Entry.bOverrideDepth ? *FString::Printf(TEXT(", %.0f cm deep"), Entry.DepthCm) : TEXT(""),
					Entry.bOverrideEdgeOpen ? (Entry.bEdgeOpen ? TEXT(", open edges") : TEXT(", walled")) : TEXT("")), Ninja);
			}
		}
		for (AActor* Actor : AllActors)
		{
			TInlineComponentArray<UFluidNinjaWaveSimComponent*> Components(Actor);
			for (const UFluidNinjaWaveSimComponent* Component : Components)
			{
				if (!IsLiveComponent(Component))
				{
					continue;
				}
				if (!Ninjas.Contains(Actor))
				{
					Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("carries a FluidNinja Wave Sim component (%s) but is not a NinjaLive actor: the component sets nothing up. Move it to the NinjaLive it is for."), *Component->GetName()), Actor);
					continue;
				}
				Sources.FindOrAdd(Actor).Add(FString::Printf(TEXT("component %s"), *Component->GetName()));
				Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("is set up by its FluidNinja Wave Sim component%s: %s, %s%s."), Component->bWaves ? TEXT("") : TEXT(" (waves off)"),
					Component->bDepthFromBed ? TEXT("depth from the bed") : *FString::Printf(TEXT("%.0f cm deep"), Component->DepthCm), Component->bWalled ? TEXT("walled") : TEXT("open edges"),
					Component->ScaleDownOverride > 0 ? *FString::Printf(TEXT(", wave grid scale-down %d"), Component->ScaleDownOverride) : TEXT("")), Actor);
			}
		}
		for (const TPair<AActor*, TArray<FString>>& Entry : Sources)
		{
			if (Entry.Value.Num() < 2)
			{
				continue;
			}
			int32 Components = 0;
			for (const FString& Source : Entry.Value)
			{
				Components += Source.StartsWith(TEXT("component")) ? 1 : 0;
			}
			if (Components >= 2)
			{
				Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("carries %d FluidNinja Wave Sim components (%s): keep one."), Components, *FString::Join(Entry.Value, TEXT(", "))), Entry.Key);
			}
			else if (Components == 1)
			{
				Report.Line(EMessageSeverity::Warning, FString::Printf(TEXT("is set up twice (%s): the component wins and the Wave Sims entry is ignored (logged once in Play). Delete the entry."), *FString::Join(Entry.Value, TEXT(", "))), Entry.Key);
			}
			else
			{
				Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("is named by %d Wave Sims entries (%s): the last wins. Keep one."), Entry.Value.Num(), *FString::Join(Entry.Value, TEXT(", "))), Entry.Key);
			}
		}
	}

	/** (d) The water surface tags the Surface Mirror looks for, and what it would draw. */
	void CheckSurfaces(FReport& Report, const TArray<AActor*>& AllActors, const UFluidNinjaBuoyancySettings& Settings)
	{
		int32 Tagged = 0;
		int32 Kept = 0;
		int32 LeftOut = 0;
		for (AActor* Actor : AllActors)
		{
			if (!HasSurfaceTag(*Actor, Settings))
			{
				continue;
			}
			++Tagged;
			const bool bNiagara = Actor->IsA<ANiagaraActor>();
			TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
			int32 Here = 0;
			for (const UPrimitiveComponent* Primitive : Primitives)
			{
				if (!IsLiveComponent(Primitive))
				{
					continue;
				}
				(IsSurfacePrimitive(*Primitive, bNiagara) ? Kept : LeftOut) += 1;
				Here += IsSurfacePrimitive(*Primitive, bNiagara) ? 1 : 0;
			}
			if (Here == 0)
			{
				Report.Line(EMessageSeverity::Warning, TEXT("carries a surface tag but has no horizontal mesh or Niagara sheet for the Surface Mirror to draw."), Actor);
			}
		}
		if (Tagged == 0)
		{
			Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("No actor in this level carries a surface tag (%s): the Surface Mirror sees no water and every floating body falls through. Tag the water surface actors (Docs/UserGuide.md, Surface tags)."),
				*FString::JoinBy(Settings.SurfaceTags, TEXT(", "), [](const FName& Name) { return Name.ToString(); })));
		}
		else
		{
			Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("%d surface actor%s tagged for the Surface Mirror: %d primitive%s drawn, %d left out (steep: a waterfall's face)."), Tagged, Tagged == 1 ? TEXT("") : TEXT("s"), Kept, Kept == 1 ? TEXT("") : TEXT("s"), LeftOut));
		}
	}

	/** (e) The project's physics: every stepping mode is supported; Stable Steps keeps light bodies stable in all of them. */
	void CheckPhysics(FReport& Report, const UFluidNinjaBuoyancySettings& Settings)
	{
		const UPhysicsSettings* Physics = UPhysicsSettings::Get();
		if (!Physics)
		{
			return;
		}
		FString Mode;
		if (Physics->bTickPhysicsAsync)
		{
			Mode = FString::Printf(TEXT("async physics at %.4f s steps (%.0f Hz): one force evaluation per async step (the physics thread's cost per frame is 2 to 4 times one step a frame)"), Physics->AsyncFixedTimeStepSize, Physics->AsyncFixedTimeStepSize > 0.f ? 1.f / Physics->AsyncFixedTimeStepSize : 0.f);
		}
		else if (Physics->bSubstepping)
		{
			Mode = FString::Printf(TEXT("substepping, at most %d substeps of %.4f s: one force evaluation per substep"), Physics->MaxSubsteps, Physics->MaxSubstepDeltaTime);
		}
		else
		{
			Mode = FString::Printf(TEXT("the default, one step a frame capped at Max Physics Delta Time %.4f s (%.0f Hz; frames longer than that run the physics in slow motion)"), Physics->MaxPhysicsDeltaTime, Physics->MaxPhysicsDeltaTime > 0.f ? 1.f / Physics->MaxPhysicsDeltaTime : 0.f);
		}
		Report.Line(EMessageSeverity::Info, FString::Printf(TEXT("Physics: %s. Every mode is supported (Docs/UserGuide.md, Physics and frame rate)."), *Mode));
		if (Settings.bStableSteps)
		{
			Report.Line(EMessageSeverity::Info, TEXT("Stable Steps is on: light bodies (balls, rafts) stay stable at any frame rate."));
		}
		else
		{
			Report.Line(EMessageSeverity::Warning, TEXT("Stable Steps is off (Project Settings → FluidNinja Buoyancy → Physics): light bodies dropped or caught in a wind blow up, even at 120 Hz. Turn it on unless you are measuring the explicit forces."));
		}
		if (!Settings.bEnabled)
		{
			Report.Line(EMessageSeverity::Warning, TEXT("FluidNinja Buoyancy is off (Project Settings → FluidNinja Buoyancy → Enabled): nothing floats."));
		}
	}
}

int32 FluidNinjaBuoyancySetupCheck::Run(UWorld* World)
{
	if (!World && GEditor)
	{
		World = GEditor->GetEditorWorldContext().World();
	}
	const UFluidNinjaBuoyancySettings& Settings = UFluidNinjaBuoyancySettings::Get();
	FReport Report(LogName);
	Report.Log.NewPage(FText::Format(LOCTEXT("PageTitle", "Check Setup: {0} ({1})"), FText::FromString(World ? World->GetMapName() : TEXT("no world")), FText::AsDateTime(FDateTime::Now())));
	if (World && World->IsGameWorld())
	{
		Report.Line(EMessageSeverity::Info, TEXT("Checked in Play: a Fix changes the Play world's copies, which Play ends. Run it on the level (Tools → FluidNinja Buoyancy: Check Setup with Play stopped) to save a fix."));
	}

	const UNiagaraSystem* Core = CheckCore(Report, Settings);

	TArray<AActor*> AllActors;
	TArray<AActor*> Ninjas;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AllActors.Add(*It);
			if (IsNinjaLive(**It, Settings))
			{
				Ninjas.Add(*It);
			}
		}
	}
	if (Ninjas.IsEmpty())
	{
		Report.Line(EMessageSeverity::Error, FString::Printf(TEXT("No NinjaLive actor in this level (nothing has the '%s' property): there is no FluidNinja simulation to float on or to stir."), *Settings.TrackingTagProperty.ToString()));
	}
	for (AActor* Ninja : Ninjas)
	{
		CheckNinjaLive(Report, *Ninja, *World, Settings, Core, AllActors);
	}
	CheckWaveSims(Report, Ninjas, AllActors, Settings);
	CheckSurfaces(Report, AllActors, Settings);
	CheckPhysics(Report, Settings);

	const FText Summary = FText::Format(LOCTEXT("Summary", "FluidNinja Buoyancy setup: {0} failed, {1} warnings, {2} passed"), FText::AsNumber(Report.Failures), FText::AsNumber(Report.Warnings), FText::AsNumber(Report.Passes));
	Report.Log.AddMessage(FTokenizedMessage::Create(Report.Failures > 0 ? EMessageSeverity::Error : Report.Warnings > 0 ? EMessageSeverity::Warning : EMessageSeverity::Info, Summary));
	Report.Log.Open(EMessageSeverity::Info, true);
	Report.Log.Notify(Summary, EMessageSeverity::Info, true);
	UE_LOG(LogFluidNinjaBuoyancyEditor, Display, TEXT("%s (the FluidNinja Buoyancy message log has every line)."), *Summary.ToString());
	return Report.Failures;
}

#undef LOCTEXT_NAMESPACE
