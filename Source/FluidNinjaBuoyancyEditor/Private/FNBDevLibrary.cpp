#include "FNBDevLibrary.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_Niagara.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Level.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/LevelFactory.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "ShaderCompiler.h"
#include "Modules/ModuleManager.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceGrid2DCollection.h"
#include "NiagaraDataInterfaceRenderTarget2D.h"
#include "NiagaraDataInterfaceRW.h"
#include "NiagaraDataInterfaceTexture.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraGraph.h"
#include "NiagaraModule.h"
#include "NiagaraNode.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterBinding.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraShared.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "DataHierarchyViewModelBase.h"
#include "StructUtils/InstancedStruct.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UnrealEdGlobals.h"
#include "UObject/UnrealType.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

// The module itself is FFluidNinjaBuoyancyEditorModule (FluidNinjaBuoyancyEditorModule.cpp); this file only adds the library to it.

int32 UFNBDevLibrary::PasteActorsText(UWorld* World, const FString& Text)
{
	if (!World || !World->GetCurrentLevel())
	{
		return 0;
	}
	TSet<AActor*> Before;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		Before.Add(*It);
	}
	// The level factory directly (the editor's paste would paste components instead while any component is selected).
	ULevel* Level = World->GetCurrentLevel();
	ULevelFactory* Factory = NewObject<ULevelFactory>();
	const TCHAR* Buffer = *Text;
	Factory->FactoryCreateText(ULevel::StaticClass(), Level, Level->GetFName(), RF_Transactional, nullptr, TEXT("paste"), Buffer, Buffer + Text.Len(), GWarn);
	int32 Added = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		Added += Before.Contains(*It) ? 0 : 1;
	}
	return Added;
}

namespace
{
	/** Walks Path ("A.B.C") from Object to the last property and the memory holding it; OutChain (optional) receives every property
	 * on the way, the top-level one first. */
	FProperty* Resolve(UObject* Object, const FString& Path, void*& OutContainer, TArray<FProperty*>* OutChain = nullptr)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."));
		UStruct* Struct = Object ? Object->GetClass() : nullptr;
		void* Container = Object;
		FProperty* Property = nullptr;
		for (int32 Index = 0; Struct && Index < Parts.Num(); ++Index)
		{
			Property = FindFProperty<FProperty>(Struct, FName(*Parts[Index]));
			if (!Property)
			{
				return nullptr;
			}
			if (OutChain)
			{
				OutChain->Add(Property);
			}
			if (Index + 1 < Parts.Num())
			{
				const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
				if (!StructProperty)
				{
					return nullptr;
				}
				Container = StructProperty->ContainerPtrToValuePtr<void>(Container);
				Struct = StructProperty->Struct;
			}
		}
		OutContainer = Container;
		return Property;
	}

	/** The compiled (runtime) copy of an editor data interface, the one the system's scripts store and run with, as
	 * FNiagaraEditorUtilities::GetResolvedRuntimeInstanceForEditorDataInterfaceInstance finds it; null before the first compile.
	 * Only asked for an input node that feeds something: the engine function ensures when it finds no stack for the node. */
	UNiagaraDataInterface* FindCompiledDataInterface(const UNiagaraSystem& System, UNiagaraDataInterface& DataInterface)
	{
		UNiagaraNodeInput* InputNode = DataInterface.GetTypedOuter<UNiagaraNodeInput>();
		const UEdGraph* Graph = InputNode ? InputNode->GetGraph() : nullptr;
		if (!Graph || !Graph->Nodes.Contains(InputNode))
		{
			return nullptr;
		}
		const bool bFeedsSomething = InputNode->Pins.ContainsByPredicate([](const UEdGraphPin* Pin)
		{
			return Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0;
		});
		return bFeedsSomething ? FNiagaraEditorUtilities::GetResolvedRuntimeInstanceForEditorDataInterfaceInstance(System, DataInterface) : nullptr;
	}

	/** After an edit of a data interface inside a system, what FNiagaraSystemViewModel::NotifyDataObjectChanged does for the details
	 * panel (UpdateCompiledDataInterfaces): copy the editor instance into its compiled copy (or refresh the exposed user store for a
	 * user data interface) and refresh the system's compiled data interface data. A data-interface-only edit changes no compile hash
	 * (UNiagaraNodeInput::DataInterface is SkipForCompileHash), so no later non-forced compile would carry it over. */
	void SyncCompiledDataInterface(UObject* Object)
	{
		UNiagaraDataInterface* DataInterface = Cast<UNiagaraDataInterface>(Object);
		UNiagaraSystem* System = DataInterface ? DataInterface->GetTypedOuter<UNiagaraSystem>() : nullptr;
		if (!System)
		{
			return;
		}
		if (UNiagaraDataInterface* Compiled = FindCompiledDataInterface(*System, *DataInterface))
		{
			Compiled->Modify(false);
			DataInterface->CopyTo(Compiled);
		}
		else if (System->GetExposedParameters().FindVariableFromDataInterface(DataInterface))
		{
			System->GetExposedParameters().OnInterfaceChange();
		}
		System->OnCompiledDataInterfaceChanged();
	}

	/** One details-panel style edit of the last property of Chain (top-level property first): one transaction, Modify, PreEditChange
	 * and PostEditChangeChainProperty with the top-level property as the member property (UNiagaraSimulationStageGeneric checks the
	 * member property for nested values such as ParticleIterationStateRange.X), then the compiled data interface refresh. Apply
	 * changes the value and says whether it worked. */
	bool EditProperty(UObject* Object, const TArray<FProperty*>& Chain, TFunctionRef<bool()> Apply)
	{
		FProperty* Leaf = Chain.Last();
		const FScopedTransaction Transaction(NSLOCTEXT("FNBDev", "EditProperty", "Edit property"));
		FEditPropertyChain EditChain;
		for (FProperty* Link : Chain)
		{
			EditChain.AddTail(Link);
		}
		EditChain.SetActivePropertyNode(Leaf);
		EditChain.SetActiveMemberPropertyNode(Chain[0]);
		Object->Modify();
		Object->PreEditChange(EditChain);
		const bool bApplied = Apply();
		FPropertyChangedEvent Event(Leaf, EPropertyChangeType::ValueSet);
		Event.SetActiveMemberProperty(Chain[0]);
		FPropertyChangedChainEvent ChainEvent(EditChain, Event);
		Object->PostEditChangeChainProperty(ChainEvent);
		SyncCompiledDataInterface(Object);
		return bApplied;
	}
}

FString UFNBDevLibrary::GetPropertyText(UObject* Object, const FString& Path)
{
	void* Container = nullptr;
	const FProperty* Property = Resolve(Object, Path, Container);
	FString Out;
	if (Property)
	{
		Property->ExportTextItem_Direct(Out, Property->ContainerPtrToValuePtr<void>(Container), nullptr, Object, PPF_None);
	}
	return Out;
}

bool UFNBDevLibrary::SetPropertyText(UObject* Object, const FString& Path, const FString& Text)
{
	void* Container = nullptr;
	TArray<FProperty*> Chain;
	FProperty* Property = Resolve(Object, Path, Container, &Chain);
	if (!Property)
	{
		return false;
	}
	return EditProperty(Object, Chain, [&]()
	{
		return Property->ImportText_Direct(*Text, Property->ContainerPtrToValuePtr<void>(Container), Object, PPF_None) != nullptr;
	});
}

UNiagaraDataInterface* UFNBDevLibrary::GetNiagaraCompiledDataInterface(UNiagaraSystem* System, UNiagaraDataInterface* EditorDataInterface)
{
	return System && EditorDataInterface ? FindCompiledDataInterface(*System, *EditorDataInterface) : nullptr;
}

bool UFNBDevLibrary::UndoLastTransaction(bool bRedo)
{
	return GEditor && (bRedo ? GEditor->RedoTransaction() : GEditor->UndoTransaction());
}

FString UFNBDevLibrary::DescribeChangedProperties(UObject* Object)
{
	FString Out;
	if (!Object)
	{
		return Out;
	}
	const UObject* Defaults = Object->GetClass()->GetDefaultObject();
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Transient) || Property->Identical_InContainer(Object, Defaults))
		{
			continue;
		}
		FString Value;
		Property->ExportTextItem_InContainer(Value, Object, Defaults, Object, PPF_None);
		Out += Property->GetName() + TEXT(" = ") + Value + TEXT("\n");
	}
	return Out;
}

namespace
{
	FString Errors(const FNiagaraExternalEditContext& Context)
	{
		FString Out;
		for (const FText& Error : Context.Errors)
		{
			Out += Error.ToString() + TEXT("; ");
		}
		return Out;
	}

	FNiagaraExt_StackItemReference InputRef(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input)
	{
		FNiagaraExt_StackItemReference Ref(System, Emitter, Script, Module);
		Ref.InputNameStack.Add(Input);
		return Ref;
	}
}

UNiagaraSystem* UFNBDevLibrary::CreateNiagaraSystem(const FString& Name, const FString& Path, UNiagaraSystem* Template, FString& OutErrors)
{
	FNiagaraExternalEditContext Context;
	UNiagaraSystem* System = UNiagaraExternalEditUtilities::CreateNiagaraSystem(Name, Path, Template, Context);
	OutErrors = Errors(Context);
	return System;
}

FString UFNBDevLibrary::AddNiagaraEmitter(UNiagaraSystem* System, UNiagaraEmitter* Template, FName Name)
{
	if (!System || !Template)
	{
		return TEXT("ERROR: no system or no template emitter");
	}
	FNiagaraExternalEditContext Context(System);
	FNiagaraExt_EmitterTopology Topology;
	UNiagaraExternalEditUtilities::AddEmitter(Template, Name, Topology, Context);
	return Errors(Context);
}

FString UFNBDevLibrary::SetNiagaraEmitterProperties(UNiagaraSystem* System, FName Emitter, const FString& Json)
{
	const FNiagaraExt_StackItemReference Ref(System, Emitter);
	FNiagaraExternalEditContext Context(Ref);
	FNiagaraExt_EmitterData Data;
	Data.PropertyValues = Json;
	UNiagaraExternalEditUtilities::SetEmitterData(Ref, Data, Context);
	return Errors(Context);
}

FString UFNBDevLibrary::SetNiagaraInputHlsl(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, const FString& Hlsl)
{
	const FNiagaraExt_StackItemReference Ref = InputRef(System, Emitter, Script, Module, Input);
	FNiagaraExternalEditContext Context(Ref);
	FNiagaraExt_StackInputValue Value;
	Value.InitializeAs<FNiagaraExt_StackInputData_HlslExpression>();
	Value.GetMutable<FNiagaraExt_StackInputData_HlslExpression>().HlslExpression = Hlsl;
	UNiagaraExternalEditUtilities::SetStackInputData(Ref, Value, Context);
	return Errors(Context);
}

FString UFNBDevLibrary::SetNiagaraInputEnum(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, const FString& Entry)
{
	const FNiagaraExt_StackItemReference Ref = InputRef(System, Emitter, Script, Module, Input);
	FNiagaraExternalEditContext Context(Ref);
	FNiagaraExt_StackInputValue Value;
	UNiagaraExternalEditUtilities::GetStackInputData(Ref, Value, Context);
	FNiagaraExt_StackInputData_Enum* Data = Value.GetMutablePtr<FNiagaraExt_StackInputData_Enum>();
	if (!Data || !Data->Enum)
	{
		return Errors(Context) + TEXT("not an enum input");
	}
	for (int32 Index = 0; Index < Data->Enum->NumEnums(); ++Index)
	{
		const FString Name = Data->Enum->GetNameByIndex(Index).ToString();
		const FString Display = Data->Enum->GetDisplayNameTextByIndex(Index).ToString();
		if (Name.Contains(Entry) || Display.Contains(Entry))
		{
			Data->EnumName = Data->Enum->GetNameByIndex(Index);
			Data->DisplayName = Data->Enum->GetDisplayNameTextByIndex(Index);
			UNiagaraExternalEditUtilities::SetStackInputData(Ref, Value, Context);
			return Errors(Context);
		}
	}
	return TEXT("no enum entry like ") + Entry;
}

FString UFNBDevLibrary::GetNiagaraInputText(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input)
{
	const FNiagaraExt_StackItemReference Ref = InputRef(System, Emitter, Script, Module, Input);
	FNiagaraExternalEditContext Context(Ref);
	FNiagaraExt_StackInputValue Value;
	UNiagaraExternalEditUtilities::GetStackInputData(Ref, Value, Context);
	FString Text;
	if (const UScriptStruct* Type = Value.GetScriptStruct())
	{
		Type->ExportText(Text, Value.GetMemory(), nullptr, nullptr, PPF_None, nullptr);
		Text = Type->GetName() + Text;
	}
	return Text + Errors(Context);
}

FString UFNBDevLibrary::RemoveNiagaraEmitter(UNiagaraSystem* System, FName Emitter)
{
	const FNiagaraExt_StackItemReference Ref(System, Emitter);
	FNiagaraExternalEditContext Context(Ref);
	UNiagaraExternalEditUtilities::RemoveEmitter(Ref, Context);
	return Errors(Context);
}

FString UFNBDevLibrary::SetNiagaraModuleEnabled(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, bool bEnabled)
{
	const FNiagaraExt_StackItemReference Ref(System, Emitter, Script, Module);
	FNiagaraExternalEditContext Context(Ref);
	UNiagaraExternalEditUtilities::SetModuleEnabled(Ref, bEnabled, Context);
	return Errors(Context);
}

namespace
{
	template <typename T>
	FString StructText(const T& Value)
	{
		FString Text;
		T::StaticStruct()->ExportText(Text, &Value, nullptr, nullptr, PPF_None, nullptr);
		return Text;
	}

	/** One resolver for every type keyword (see the header). bTextureIsObject: "Texture" means the UTexture object type (user
	 * variables) instead of the Texture Sample data interface (stack and graph pins). */
	bool TypeFromKeyword(const FString& Type, FNiagaraTypeDefinition& Out, bool bTextureIsObject = false)
	{
		const FString Key = Type.TrimStartAndEnd();
		auto Is = [&Key](const TCHAR* Name) { return Key.Equals(Name, ESearchCase::IgnoreCase); };
		if (Is(TEXT("float"))) { Out = FNiagaraTypeDefinition::GetFloatDef(); return true; }
		if (Is(TEXT("int")) || Is(TEXT("int32"))) { Out = FNiagaraTypeDefinition::GetIntDef(); return true; }
		if (Is(TEXT("bool"))) { Out = FNiagaraTypeDefinition::GetBoolDef(); return true; }
		if (Is(TEXT("vec2")) || Is(TEXT("vector2"))) { Out = FNiagaraTypeDefinition::GetVec2Def(); return true; }
		if (Is(TEXT("vec3")) || Is(TEXT("vector"))) { Out = FNiagaraTypeDefinition::GetVec3Def(); return true; }
		if (Is(TEXT("vec4")) || Is(TEXT("vector4"))) { Out = FNiagaraTypeDefinition::GetVec4Def(); return true; }
		if (Is(TEXT("color")) || Is(TEXT("linearcolor"))) { Out = FNiagaraTypeDefinition::GetColorDef(); return true; }
		if (Is(TEXT("position"))) { Out = FNiagaraTypeDefinition::GetPositionDef(); return true; }
		if (Is(TEXT("matrix")) || Is(TEXT("matrix4")) || Is(TEXT("mat4"))) { Out = FNiagaraTypeDefinition::GetMatrix4Def(); return true; }
		if (Is(TEXT("quat"))) { Out = FNiagaraTypeDefinition::GetQuatDef(); return true; }
		if (Is(TEXT("Grid2DCollection"))) { Out = FNiagaraTypeDefinition(UNiagaraDataInterfaceGrid2DCollection::StaticClass()); return true; }
		if (Is(TEXT("RenderTarget2D"))) { Out = FNiagaraTypeDefinition(UNiagaraDataInterfaceRenderTarget2D::StaticClass()); return true; }
		if (Is(TEXT("Texture")))
		{
			Out = bTextureIsObject ? FNiagaraTypeDefinition(UTexture::StaticClass()) : FNiagaraTypeDefinition(UNiagaraDataInterfaceTexture::StaticClass());
			return true;
		}
		if (Is(TEXT("TextureObject"))) { Out = FNiagaraTypeDefinition(UTexture::StaticClass()); return true; }
		if (Is(TEXT("RenderTargetObject"))) { Out = FNiagaraTypeDefinition(UTextureRenderTarget::StaticClass()); return true; }
		if (Key.StartsWith(TEXT("/")))
		{
			// Only classes Niagara types can be made of: data interfaces, and textures / render targets for user object parameters.
			UClass* Class = FindObject<UClass>(nullptr, *Key);
			if (Class && (Class->IsChildOf(UNiagaraDataInterface::StaticClass()) || Class->IsChildOf(UTexture::StaticClass())))
			{
				Out = FNiagaraTypeDefinition(Class);
				return true;
			}
		}
		return false;
	}

	/** An enum value's name, looked up by the enum's path (no link dependency on the enum's module). */
	FString EnumText(const TCHAR* EnumPath, int64 Value)
	{
		const UEnum* Enum = FindObject<UEnum>(nullptr, EnumPath);
		return Enum ? Enum->GetNameStringByValue(Value) : FString::Printf(TEXT("%lld"), Value);
	}
}

FString UFNBDevLibrary::DescribeNiagara(UNiagaraSystem* System, FName Emitter, FName Script)
{
	if (Emitter.IsNone())
	{
		FNiagaraExternalEditContext Context(System);
		FNiagaraExt_SystemSummary Summary;
		UNiagaraExternalEditUtilities::GetSystemSummary(System, Summary, Context);
		FNiagaraExt_UserVariables Users;
		UNiagaraExternalEditUtilities::GetUserVariables(System, Users, Context);
		FString Out = TEXT("SUMMARY ") + StructText(Summary) + TEXT("\nUSER");
		for (const FNiagaraExt_UserVariable& Variable : Users.UserVariables)
		{
			// "<name> : <type> = <value struct><export text>", the default as the system stores it.
			FString Default;
			if (const UScriptStruct* ValueType = Variable.DefaultValue.GetScriptStruct())
			{
				ValueType->ExportText(Default, Variable.DefaultValue.GetMemory(), nullptr, nullptr, PPF_None, nullptr);
				Default = TEXT(" = ") + ValueType->GetName() + Default;
			}
			Out += TEXT("\n  ") + Variable.Name.ToString() + TEXT(" : ") + Variable.Type.GetName() + Default;
		}
		return Out + TEXT("\n") + Errors(Context);
	}
	const FNiagaraExt_StackItemReference Ref(System, Emitter, Script);
	FNiagaraExternalEditContext Context(Ref);
	FNiagaraExt_ScriptStackTopology Topology;
	UNiagaraExternalEditUtilities::GetScriptStackTopology(Ref, Topology, Context);
	FString Out;
	for (const FNiagaraExt_ModuleTopology& Module : Topology.Modules)
	{
		Out += FString::Printf(TEXT("MODULE %s (%s)%s%s\n"), *Module.ModuleName.ToString(), *GetPathNameSafe(Module.ModuleScript),
			Module.Enabled ? TEXT("") : TEXT(" DISABLED"), Module.bIsSetParametersModule ? TEXT(" SETPARAMS") : TEXT(""));
		for (const FNiagaraExt_StackInputTopology& Input : Module.Inputs)
		{
			Out += FString::Printf(TEXT("    %s : %s%s\n"), *Input.Name.ToString(), *Input.Type.GetName(), Input.bIsVisible ? TEXT("") : TEXT(" (hidden)"));
		}
	}
	return Out + Errors(Context);
}

FString UFNBDevLibrary::AddNiagaraModuleAfter(UNiagaraSystem* System, FName Emitter, FName Script, FName AfterModule, const FString& ModulePath)
{
	const UNiagaraScript* ModuleAsset = LoadObject<UNiagaraScript>(nullptr, *ModulePath);
	if (!ModuleAsset)
	{
		return TEXT("ERROR: no module asset ") + ModulePath;
	}
	const FNiagaraExt_StackItemReference Ref(System, Emitter, Script, AfterModule);
	FNiagaraExternalEditContext Context(Ref);
	FNiagaraExt_ModuleTopology Topology;
	UNiagaraExternalEditUtilities::AddModule(Ref, ModuleAsset, Topology, Context);
	const FString Problems = Errors(Context);
	return Problems.IsEmpty() ? Topology.ModuleName.ToString() : TEXT("ERROR: ") + Problems;
}

FString UFNBDevLibrary::AddNiagaraSetParameterAfter(UNiagaraSystem* System, FName Emitter, FName Script, FName AfterModule, FName Parameter, const FString& Type)
{
	FNiagaraExt_SetParameterEntry Entry;
	Entry.Variable.Name = Parameter;
	if (!TypeFromKeyword(Type, Entry.Variable.Type))
	{
		return TEXT("ERROR: unknown type ") + Type;
	}
	const FNiagaraExt_StackItemReference Ref(System, Emitter, Script, AfterModule);
	FNiagaraExternalEditContext Context(Ref);
	FNiagaraExt_ModuleTopology Topology;
	UNiagaraExternalEditUtilities::AddSetParametersModule(Ref, {Entry}, Topology, Context);
	const FString Problems = Errors(Context);
	return Problems.IsEmpty() ? Topology.ModuleName.ToString() : TEXT("ERROR: ") + Problems;
}

FString UFNBDevLibrary::SetNiagaraInputLinked(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, FName Linked)
{
	const FNiagaraExt_StackItemReference Ref = InputRef(System, Emitter, Script, Module, Input);
	FNiagaraExternalEditContext Context(Ref);
	FNiagaraExt_StackInputTopology Topology;
	UNiagaraExternalEditUtilities::GetStackInputTopology(Ref, Topology, Context);
	FNiagaraExt_StackInputValue Value;
	Value.InitializeAs<FNiagaraExt_StackInputData_Linked>();
	FNiagaraExt_StackInputData_Linked& Data = Value.GetMutable<FNiagaraExt_StackInputData_Linked>();
	Data.LinkedVariable.Name = Linked;
	Data.LinkedVariable.Type = Topology.Type;
	UNiagaraExternalEditUtilities::SetStackInputData(Ref, Value, Context);
	return Errors(Context);
}

FString UFNBDevLibrary::AddNiagaraUserFloat(UNiagaraSystem* System, FName Name, float Default)
{
	FNiagaraExternalEditContext Context(System);
	FNiagaraExt_UserVariable Variable;
	Variable.Name = Name;
	Variable.Type = FNiagaraTypeDefinition::GetFloatDef();
	FNiagaraVariant Variant;
	Variant.SetBytes(reinterpret_cast<const uint8*>(&Default), sizeof(float));
	Variable.DefaultValue.Set(Variable.Type, Variant);
	UNiagaraExternalEditUtilities::AddUserVariable(System, Variable, Context);
	return Errors(Context);
}

namespace
{
	/** Up to Count floats from "1, 2, 3" or "(1,2,3)"; missing components are 0. */
	TArray<float> ParseFloats(const FString& Text, int32 Count)
	{
		FString Clean = Text.Replace(TEXT("("), TEXT("")).Replace(TEXT(")"), TEXT(""));
		TArray<FString> Parts;
		Clean.ParseIntoArray(Parts, TEXT(","));
		TArray<float> Values;
		Values.SetNumZeroed(Count);
		for (int32 Index = 0; Index < Count && Index < Parts.Num(); ++Index)
		{
			Values[Index] = FCString::Atof(*Parts[Index].TrimStartAndEnd());
		}
		return Values;
	}

	template <typename T>
	void SetVariantValue(FNiagaraVariant& Variant, const T& Value)
	{
		Variant.SetBytes(reinterpret_cast<const uint8*>(&Value), sizeof(T));
	}
}

FString UFNBDevLibrary::AddUserVariable(UNiagaraSystem* System, FName Name, const FString& Type, const FString& DefaultValue)
{
	if (!System)
	{
		return TEXT("ERROR: no system");
	}
	FNiagaraTypeDefinition TypeDef;
	if (!TypeFromKeyword(Type, TypeDef, true))
	{
		return TEXT("ERROR: unknown type ") + Type;
	}
	if (TypeDef.IsDataInterface())
	{
		return TEXT("ERROR: data interface user parameters are not supported here; use an object type (Texture, RenderTargetObject)");
	}
	FNiagaraVariant Variant;
	const FString Text = DefaultValue.TrimStartAndEnd();
	if (TypeDef.IsUObject())
	{
		UObject* Object = nullptr;
		if (!Text.IsEmpty())
		{
			Object = LoadObject<UObject>(nullptr, *Text);
			if (!Object || !Object->IsA(TypeDef.GetClass()))
			{
				return FString::Printf(TEXT("ERROR: no %s at %s"), *TypeDef.GetClass()->GetName(), *Text);
			}
		}
		Variant.SetUObject(Object);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
	{
		FNiagaraBool Value;
		Value.SetValue(Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text == TEXT("1"));
		SetVariantValue(Variant, Value);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
	{
		FNiagaraInt32 Value;
		Value.Value = FCString::Atoi(*Text);
		SetVariantValue(Variant, Value);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
	{
		SetVariantValue(Variant, FCString::Atof(*Text));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
	{
		const TArray<float> V = ParseFloats(Text, 2);
		SetVariantValue(Variant, FVector2f(V[0], V[1]));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def() || TypeDef == FNiagaraTypeDefinition::GetPositionDef())
	{
		const TArray<float> V = ParseFloats(Text, 3);
		SetVariantValue(Variant, FVector3f(V[0], V[1], V[2]));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
	{
		const TArray<float> V = ParseFloats(Text, 4);
		SetVariantValue(Variant, FVector4f(V[0], V[1], V[2], V[3]));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
	{
		const TArray<float> V = ParseFloats(Text, 4);
		SetVariantValue(Variant, FLinearColor(V[0], V[1], V[2], V[3]));
	}
	else
	{
		return TEXT("ERROR: no default parser for ") + Type;
	}

	FNiagaraExternalEditContext Context(System);
	FNiagaraExt_UserVariable Variable;
	Variable.Name = Name;
	Variable.Type = TypeDef;
	Variable.DefaultValue.Set(TypeDef, Variant);
	if (!Variable.DefaultValue.IsValid())
	{
		return TEXT("ERROR: the default value does not match the type ") + Type;
	}
	UNiagaraExternalEditUtilities::AddUserVariable(System, Variable, Context);
	const FString Problems = Errors(Context);
	if (!Problems.IsEmpty())
	{
		return TEXT("ERROR: ") + Problems;
	}
	// The exposed store adds the "User." namespace; report the name exactly as stored, so bindings can use it.
	const FString Plain = Name.ToString();
	for (const FNiagaraVariableWithOffset& Parameter : System->GetExposedParameters().ReadParameterVariables())
	{
		const FString Stored = Parameter.GetName().ToString();
		if ((Stored == Plain || Stored == TEXT("User.") + Plain) && Parameter.GetType() == TypeDef)
		{
			return Stored;
		}
	}
	return TEXT("ERROR: the user variable was not added (") + Plain + TEXT(")");
}

FString UFNBDevLibrary::ListProperties(UObject* Object, const FString& Filter)
{
	FString Out;
	if (!Object)
	{
		return Out;
	}
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (!Filter.IsEmpty() && !Property->GetName().Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		FString Value;
		Property->ExportTextItem_InContainer(Value, Object, nullptr, Object, PPF_None);
		Out += Property->GetName() + TEXT(" : ") + Property->GetCPPType() + TEXT(" = ") + Value.Left(300) + TEXT("\n");
	}
	return Out;
}

FString UFNBDevLibrary::CompileNiagaraSystem(UNiagaraSystem* System, bool bWait, bool bForce)
{
	if (!System)
	{
		return TEXT("ERROR: no system");
	}
	// The inspection context first: building its system view model can itself request a compile, which the waits below then include.
	FNiagaraExternalEditContext Context(System);
	// The script compiles queue their GPU shader jobs, which the editor starts from a tickable (FNiagaraShaderQueueTickable) that
	// nothing ticks while a script or commandlet runs; WaitForCompilationComplete only finishes jobs already started. So: finish the
	// script compiles, start the queued shader jobs (what that tickable does each tick), finish those, and repeat until nothing is
	// outstanding (a compile can request another).
	INiagaraModule& NiagaraModule = FModuleManager::LoadModuleChecked<INiagaraModule>(TEXT("Niagara"));
	auto WaitForAll = [System, &NiagaraModule]()
	{
		const double Start = FPlatformTime::Seconds();
		do
		{
			System->WaitForCompilationComplete(/*bIncludingGPUShaders*/ true, /*bShowProgress*/ false);
			NiagaraModule.ProcessShaderCompilationQueue();
			System->WaitForCompilationComplete(/*bIncludingGPUShaders*/ true, /*bShowProgress*/ false);
		}
		while (System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders*/ true) && FPlatformTime::Seconds() - Start < 600.0);
	};
	// With bWait, first finish the compiles that edits already requested (each stage change requests one). RequestCompile judges what
	// is out of date against the results stored so far, so a request made while an older snapshot is still compiling can be judged up
	// to date, and that older snapshot's results then land last (a removed stage's code stays in the compiled GPU script).
	if (bWait)
	{
		WaitForAll();
	}
	System->RequestCompile(bForce);
	if (bWait)
	{
		WaitForAll();
	}
	FNiagaraExt_SystemCompileState State;
	UNiagaraExternalEditUtilities::GetSystemCompileState(System, State, Context);
	static const TCHAR* StatusNames[] = {TEXT("Unknown"), TEXT("Dirty"), TEXT("Error"), TEXT("UpToDate"), TEXT("BeingCreated"),
		TEXT("UpToDateWithWarnings"), TEXT("ComputeUpToDateWithWarnings")};
	static const TCHAR* SeverityNames[] = {TEXT("Log"), TEXT("Display"), TEXT("Warning"), TEXT("Error")};
	auto Status = [](ENiagaraExt_ScriptCompileStatus Value)
	{
		const int32 Index = static_cast<int32>(Value);
		return Index >= 0 && Index < UE_ARRAY_COUNT(StatusNames) ? StatusNames[Index] : TEXT("?");
	};
	bool bErrors = State.bHasErrors;
	bool bWarnings = State.bHasWarnings;
	bool bCompiling = State.bIsCompiling;
	FString Body;
	for (const FNiagaraExt_ScriptCompileInfo& Script : State.Scripts)
	{
		Body += FString::Printf(TEXT("SCRIPT %s/%s %s%s\n"), *Script.EmitterName.ToString(), *Script.ScriptName.ToString(),
			Status(Script.LastCompileStatus), Script.ErrorSummary.IsEmpty() ? TEXT("") : *(TEXT(" : ") + Script.ErrorSummary));
		for (const FNiagaraExt_CompileEvent& Event : Script.CompileEvents)
		{
			if (Event.Severity == ENiagaraExt_CompileEventSeverity::Warning || Event.Severity == ENiagaraExt_CompileEventSeverity::Error)
			{
				Body += FString::Printf(TEXT("    %s %s\n"), SeverityNames[static_cast<int32>(Event.Severity)], *Event.Message);
			}
		}
	}
	// GPU shader compile errors live on the compute script's shader script, not in the compile state above; read them the way
	// FNiagaraSystemViewModel does (a message containing "err0r", any case, is an error; anything else a warning). A compiler line
	// "<file>:<line>: error" (case-sensitive) counts as an error too.
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		const UNiagaraScript* GpuScript = Data ? Data->GetGPUComputeScript() : nullptr;
		const FNiagaraShaderScript* ShaderScript = GpuScript && Data->SimTarget == ENiagaraSimTarget::GPUComputeSim ? GpuScript->GetRenderThreadScript() : nullptr;
		if (!ShaderScript)
		{
			continue;
		}
		const bool bFinished = ShaderScript->IsCompilationFinished();
		bCompiling |= !bFinished;
		int32 GpuErrors = 0;
		FString Lines;
		for (const FString& Message : bFinished ? ShaderScript->GetCompileErrors() : TArray<FString>())
		{
			const bool bError = Message.Contains(TEXT("err0r"), ESearchCase::IgnoreCase) || Message.Contains(TEXT(": error"), ESearchCase::CaseSensitive);
			GpuErrors += bError ? 1 : 0;
			bErrors |= bError;
			bWarnings |= !bError;
			Lines += FString::Printf(TEXT("    GPU %s %s\n"), bError ? TEXT("Error") : TEXT("Warning"), *Message.TrimStartAndEnd());
		}
		// A shader map that failed earlier and comes back from the cache brings no messages, so its state counts too.
		const FNiagaraShaderMap* ShaderMap = ShaderScript->GetGameThreadShaderMap();
		const bool bMapValid = ShaderMap && ShaderMap->IsValid();
		if (bFinished && !bMapValid)
		{
			bErrors = true;
			GpuErrors = FMath::Max(GpuErrors, 1);
		}
		Body += FString::Printf(TEXT("GPUSHADER %s finished=%d shadermap=%s errors=%d\n"), *Handle.GetName().ToString(), bFinished ? 1 : 0,
			!ShaderMap ? TEXT("missing") : bMapValid ? TEXT("valid") : TEXT("failed"), GpuErrors) + Lines;
	}
	return FString::Printf(TEXT("STATUS %s errors=%d warnings=%d compiling=%d\n"), bErrors && !State.bHasErrors ? TEXT("Error") : Status(State.AggregateStatus),
		bErrors ? 1 : 0, bWarnings ? 1 : 0, bCompiling ? 1 : 0) + Body + Errors(Context);
}

// --- Emitters, graphs and stacks -----------------------------------------------------------------------------------------------

namespace
{
	const FNiagaraEmitterHandle* FindHandle(UNiagaraSystem* System, FName Emitter, FString& OutError)
	{
		if (!System)
		{
			OutError = TEXT("No system.");
			return nullptr;
		}
		TArray<FString> Names;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (Handle.GetName() == Emitter)
			{
				if (!Handle.GetEmitterData() || !Handle.GetInstance().Emitter)
				{
					OutError = FString::Printf(TEXT("Emitter %s has no editable emitter data."), *Emitter.ToString());
					return nullptr;
				}
				return &Handle;
			}
			Names.Add(Handle.GetName().ToString());
		}
		OutError = FString::Printf(TEXT("%s has no emitter %s (emitters: %s)."), *System->GetName(), *Emitter.ToString(), *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	UNiagaraScriptSource* EmitterSource(const FNiagaraEmitterHandle& Handle)
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		return Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
	}

	/** "#N" with N all digits: true and the index; anything else false. */
	bool ParseStageIndex(const FString& Text, int32& OutIndex)
	{
		const FString Digits = Text.StartsWith(TEXT("#")) ? Text.Mid(1) : FString();
		if (Digits.IsEmpty() || Digits.Len() > 6)
		{
			return false;
		}
		for (const TCHAR Char : Digits)
		{
			if (!FChar::IsDigit(Char))
			{
				return false;
			}
		}
		OutIndex = FCString::Atoi(*Digits);
		return true;
	}

	/** Indices of the stages named Name. */
	TArray<int32> StagesNamed(const FNiagaraEmitterHandle& Handle, const FString& Name)
	{
		TArray<int32> Indices;
		const TArray<UNiagaraSimulationStageBase*>& Stages = Handle.GetEmitterData()->GetSimulationStages();
		for (int32 Index = 0; Index < Stages.Num(); ++Index)
		{
			if (Stages[Index] && Stages[Index]->SimulationStageName.ToString() == Name)
			{
				Indices.Add(Index);
			}
		}
		return Indices;
	}

	/** The simulation stage named Name, or the one at index "#N"; null with OutError set when there is none, or when several stages
	 * share the name (then address it by index). */
	UNiagaraSimulationStageBase* FindStage(const FNiagaraEmitterHandle& Handle, const FString& Name, FString& OutError)
	{
		const TArray<UNiagaraSimulationStageBase*>& Stages = Handle.GetEmitterData()->GetSimulationStages();
		int32 Index = INDEX_NONE;
		if (Name.StartsWith(TEXT("#")))
		{
			if (!ParseStageIndex(Name, Index) || !Stages.IsValidIndex(Index) || !Stages[Index])
			{
				OutError = FString::Printf(TEXT("No simulation stage at %s (the emitter has %d; \"#N\" takes digits only)."), *Name, Stages.Num());
				return nullptr;
			}
			return Stages[Index];
		}
		const TArray<int32> Matches = StagesNamed(Handle, Name);
		if (Matches.Num() == 1)
		{
			return Stages[Matches[0]];
		}
		if (Matches.Num() > 1)
		{
			TArray<FString> Indices;
			for (const int32 Match : Matches)
			{
				Indices.Add(FString::Printf(TEXT("#%d"), Match));
			}
			OutError = FString::Printf(TEXT("%d simulation stages are named %s (%s); address one by its index."), Matches.Num(), *Name, *FString::Join(Indices, TEXT(", ")));
			return nullptr;
		}
		TArray<FString> Names;
		for (const UNiagaraSimulationStageBase* Stage : Stages)
		{
			Names.Add(Stage ? Stage->SimulationStageName.ToString() : TEXT("<null>"));
		}
		OutError = FString::Printf(TEXT("No simulation stage named %s (stages: %s)."), *Name, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	/** Refuses stage edits while the system is open in an asset editor: its stack and script view models would keep stale stage
	 * groups (the editor's own add and delete actions reset them; these helpers cannot reach them). */
	FString AssetEditorRefusal(UNiagaraSystem* System)
	{
		UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (System && Editors && Editors->FindEditorForAsset(System, /*bFocusIfOpen*/ false))
		{
			return FString::Printf(TEXT("%s is open in an asset editor; close it before adding or removing simulation stages."), *System->GetName());
		}
		return FString();
	}

	/** The data interface parameters a simulation stage can iterate, as FNiagaraDataInterfaceBindingCustomization::GetNames lists them
	 * (data interfaces derived from UNiagaraDataInterfaceRWBase in the Emitter, System, Output, Particles, Transient or StackContext
	 * namespace, read or written on the emitter's parameter maps). Collected from the map get / set pins of Graphs and of every
	 * graph their function calls reach, and from the targets of Set Parameters modules. */
	TSet<FName> IterationSourceCandidates(TArray<const UEdGraph*> Pending)
	{
		TSet<FName> Names;
		TSet<const UEdGraph*> Visited;
		auto Consider = [&Names](FName Name, const FNiagaraTypeDefinition& Type)
		{
			const UClass* Class = Type.IsDataInterface() ? Type.GetClass() : nullptr;
			const FNiagaraParameterHandle Handle(Name);
			const bool bNamespace = Handle.IsEmitterHandle() || Handle.IsSystemHandle() || Handle.IsOutputHandle() || Handle.IsParticleAttributeHandle()
				|| Handle.IsTransientHandle() || Handle.IsStackContextHandle();
			if (Class && bNamespace && Class->IsChildOf(UNiagaraDataInterfaceRWBase::StaticClass()))
			{
				Names.Add(Name);
			}
		};
		while (Pending.Num() > 0)
		{
			const UEdGraph* Graph = Pending.Pop();
			if (!Graph || Visited.Contains(Graph))
			{
				continue;
			}
			Visited.Add(Graph);
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (const UNiagaraNodeAssignment* Assignment = Cast<UNiagaraNodeAssignment>(Node))
				{
					for (const FNiagaraVariable& Target : Assignment->GetAssignmentTargets())
					{
						Consider(Target.GetName(), Target.GetType());
					}
				}
				if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node))
				{
					Pending.Add(Call->GetCalledGraph());
				}
				const FString ClassName = Node->GetClass()->GetName();
				const bool bGet = ClassName == TEXT("NiagaraNodeParameterMapGet");
				const bool bSet = ClassName == TEXT("NiagaraNodeParameterMapSet");
				if (!bGet && !bSet)
				{
					continue;
				}
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->PinType.PinSubCategory == TEXT("ParameterPin") && Pin->Direction == (bGet ? EGPD_Output : EGPD_Input))
					{
						Consider(Pin->PinName, UEdGraphSchema_Niagara::PinToTypeDefinition(Pin));
					}
				}
			}
		}
		return Names;
	}

	/** Destroys every node of Graph that is not in Before (the rollback of a stage add that failed half way). */
	void DestroyNodesAddedSince(UEdGraph& Graph, const TSet<UEdGraphNode*>& Before)
	{
		TArray<UEdGraphNode*> Added;
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (Node && !Before.Contains(Node))
			{
				Added.Add(Node);
			}
		}
		for (UEdGraphNode* Node : Added)
		{
			Node->Modify();
			Node->DestroyNode();
		}
	}

	/** FNiagaraStackGraphUtilities::GetParameterMapInputPin / GetParameterMapOutputPin (not exported): the node's first pin of
	 * that direction whose type is the parameter map. */
	UEdGraphPin* MapPin(UEdGraphNode& Node, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** UNiagaraGraph::FindOutputNode (not exported): exact usage and usage id, so one stage never matches another. */
	UNiagaraNodeOutput* FindOutputNode(const UNiagaraGraph& Graph, ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node);
			if (Output && Output->GetUsage() == Usage && Output->GetUsageId() == UsageId)
			{
				return Output;
			}
		}
		return nullptr;
	}

	/** FNiagaraStackGraphUtilities::GetOrderedModuleNodes (not exported): the function calls on the parameter-map chain into Output,
	 * first module first. */
	TArray<UNiagaraNodeFunctionCall*> OrderedModules(UNiagaraNodeOutput& Output)
	{
		TArray<UNiagaraNodeFunctionCall*> Modules;
		UEdGraphNode* Previous = &Output;
		while (Previous)
		{
			UEdGraphPin* Input = MapPin(*Previous, EGPD_Input);
			if (Input && Input->LinkedTo.Num() == 1 && Input->LinkedTo[0])
			{
				UEdGraphNode* Current = Input->LinkedTo[0]->GetOwningNode();
				if (UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Current))
				{
					Modules.Insert(Call, 0);
				}
				Previous = Current;
			}
			else
			{
				Previous = nullptr;
			}
		}
		return Modules;
	}

	/** FNiagaraStackGraphUtilities::ResetGraphForOutput (not exported), line for line: finds or creates the output node for the usage,
	 * creates a fresh parameter-map input node and links it straight into the output. The system-script branch (RebuildEmitterNodes)
	 * is left out: it only runs for SystemSpawnScript / SystemUpdateScript. */
	UNiagaraNodeOutput* ResetGraphForOutput(UNiagaraGraph& NiagaraGraph, ENiagaraScriptUsage ScriptUsage, FGuid ScriptUsageId)
	{
		NiagaraGraph.Modify();
		UNiagaraNodeOutput* OutputNode = FindOutputNode(NiagaraGraph, ScriptUsage, ScriptUsageId);
		UEdGraphPin* OutputNodeInputPin = OutputNode != nullptr ? MapPin(*OutputNode, EGPD_Input) : nullptr;
		if (OutputNode != nullptr && OutputNodeInputPin == nullptr)
		{
			NiagaraGraph.RemoveNode(OutputNode);
			OutputNode = nullptr;
		}

		if (OutputNode == nullptr)
		{
			FGraphNodeCreator<UNiagaraNodeOutput> OutputNodeCreator(NiagaraGraph);
			OutputNode = OutputNodeCreator.CreateNode();
			OutputNode->SetUsage(ScriptUsage);
			OutputNode->SetUsageId(ScriptUsageId);
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
			OutputNodeCreator.Finalize();
			OutputNodeInputPin = MapPin(*OutputNode, EGPD_Input);
		}
		else
		{
			OutputNode->Modify();
		}

		FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(NiagaraGraph);
		UNiagaraNodeInput* InputNode = InputNodeCreator.CreateNode();
		InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
		InputNodeCreator.Finalize();

		UEdGraphPin* InputNodeOutputPin = MapPin(*InputNode, EGPD_Output);
		if (!OutputNodeInputPin || !InputNodeOutputPin)
		{
			return nullptr;
		}
		// FNiagaraStackGraphUtilities::BreakAllPinLinks / MakeLinkTo.
		OutputNodeInputPin->BreakAllPinLinks(true);
		OutputNodeInputPin->MakeLinkTo(InputNodeOutputPin);
		OutputNodeInputPin->GetOwningNode()->PinConnectionListChanged(OutputNodeInputPin);
		InputNodeOutputPin->GetOwningNode()->PinConnectionListChanged(InputNodeOutputPin);
		return OutputNode;
	}

	/** Script usage and usage id from a script name: an ENiagaraScriptUsage name, "Stage:<SimulationStageName>" or "Stage:#N". */
	bool ResolveScript(const FNiagaraEmitterHandle& Handle, FName Script, ENiagaraScriptUsage& OutUsage, FGuid& OutUsageId, FString& OutError)
	{
		const FString Name = Script.ToString();
		if (Name.StartsWith(TEXT("Stage:")))
		{
			UNiagaraSimulationStageBase* Stage = FindStage(Handle, Name.Mid(6), OutError);
			if (!Stage)
			{
				return false;
			}
			if (!Stage->Script)
			{
				OutError = FString::Printf(TEXT("Simulation stage %s has no stage script."), *Name.Mid(6));
				return false;
			}
			OutUsage = ENiagaraScriptUsage::ParticleSimulationStageScript;
			OutUsageId = Stage->Script->GetUsageId();
			return true;
		}
		const UEnum* Enum = FindObject<UEnum>(nullptr, TEXT("/Script/Niagara.ENiagaraScriptUsage"));
		const int64 Value = Enum ? Enum->GetValueByNameString(Name) : INDEX_NONE;
		if (Value == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Unknown script %s (an ENiagaraScriptUsage name or Stage:<name>)."), *Name);
			return false;
		}
		OutUsage = static_cast<ENiagaraScriptUsage>(Value);
		OutUsageId = FGuid();
		return true;
	}

	/** An input node's data interface (UNiagaraNodeInput::GetDataInterface is not exported). */
	UNiagaraDataInterface* InputNodeDataInterface(UEdGraphNode* Node)
	{
		UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
		const FObjectProperty* Property = InputNode ? FindFProperty<FObjectProperty>(UNiagaraNodeInput::StaticClass(), TEXT("DataInterface")) : nullptr;
		return Property ? Cast<UNiagaraDataInterface>(Property->GetObjectPropertyValue_InContainer(InputNode)) : nullptr;
	}

	/** The override pin of a module input, found without changing anything (FNiagaraStackGraphUtilities::GetStackFunctionOverrideNode
	 * and GetStackFunctionInputOverridePin, not exported): the input pin named OverrideName on the parameter-map set linked into the
	 * module's input map pin. */
	UEdGraphPin* FindOverridePin(UNiagaraNodeFunctionCall& Call, FName OverrideName)
	{
		UEdGraphPin* CallInput = MapPin(Call, EGPD_Input);
		UEdGraphNode* OverrideNode = CallInput && CallInput->LinkedTo.Num() == 1 && CallInput->LinkedTo[0] ? CallInput->LinkedTo[0]->GetOwningNode() : nullptr;
		if (!OverrideNode || !OverrideNode->GetClass()->GetName().Equals(TEXT("NiagaraNodeParameterMapSet")))
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : OverrideNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == OverrideName)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** A module input's own default data interface, as UNiagaraStackFunctionInput reads it (FindParameterMapDefaultValuePin and
	 * GetDefaultDataInterfaceValueFromDefaultPin, not exported): the default pin behind each parameter-map get output named InputName
	 * in the module graph. A default pin linked to an input node gives that node's data interface; an unlinked one (or none) means
	 * the class defaults (OutDefault null). False with OutError set when a default pin is linked to anything else (a linked default,
	 * which a local data interface would replace) or when two map gets disagree. */
	bool FindModuleDefaultDataInterface(const UNiagaraGraph* ModuleGraph, FName InputName, UNiagaraDataInterface*& OutDefault, FString& OutError)
	{
		OutDefault = nullptr;
		if (!ModuleGraph)
		{
			return true;
		}
		bool bFound = false;
		for (UEdGraphNode* Node : ModuleGraph->Nodes)
		{
			if (!Node || !Node->GetClass()->GetName().Equals(TEXT("NiagaraNodeParameterMapGet")))
			{
				continue;
			}
			FMapProperty* MapProperty = FindFProperty<FMapProperty>(Node->GetClass(), TEXT("PinOutputToPinDefaultPersistentId"));
			if (!MapProperty)
			{
				continue;
			}
			FScriptMapHelper Defaults(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(Node));
			for (const UEdGraphPin* Output : Node->Pins)
			{
				if (!Output || Output->Direction != EGPD_Output || Output->PinName != InputName)
				{
					continue;
				}
				const FGuid* DefaultId = reinterpret_cast<const FGuid*>(Defaults.FindValueFromHash(&Output->PersistentGuid));
				const UEdGraphPin* DefaultPin = nullptr;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (DefaultId && Pin && Pin->Direction == EGPD_Input && Pin->PersistentGuid == *DefaultId)
					{
						DefaultPin = Pin;
					}
				}
				UNiagaraDataInterface* Default = nullptr;
				if (DefaultPin && DefaultPin->LinkedTo.Num() > 0)
				{
					Default = DefaultPin->LinkedTo.Num() == 1 && DefaultPin->LinkedTo[0] ? InputNodeDataInterface(DefaultPin->LinkedTo[0]->GetOwningNode()) : nullptr;
					if (!Default)
					{
						OutError = FString::Printf(TEXT("The module's default for %s is linked to another value, not a local data interface; a local one would replace that link."), *InputName.ToString());
						return false;
					}
				}
				const bool bSame = Default == OutDefault || (Default && OutDefault && Default->Equals(OutDefault));
				if (bFound && !bSame)
				{
					OutError = FString::Printf(TEXT("The module reads %s in several places with different defaults."), *InputName.ToString());
					return false;
				}
				bFound = true;
				OutDefault = Default;
			}
		}
		return true;
	}

	FString IterationSourceText(ENiagaraIterationSource Source)
	{
		switch (Source)
		{
		case ENiagaraIterationSource::Particles: return TEXT("Particles");
		case ENiagaraIterationSource::DataInterface: return TEXT("DataInterface");
		case ENiagaraIterationSource::DirectSet: return TEXT("DirectSet");
		default: return FString::Printf(TEXT("%d"), static_cast<int32>(Source));
		}
	}
}

UEdGraph* UFNBDevLibrary::GetScriptGraph(UNiagaraScript* Script)
{
	UNiagaraScriptSource* Source = Script ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
	return Source ? Source->NodeGraph : nullptr;
}

int32 UFNBDevLibrary::GetModuleUsageBitmask(UNiagaraScript* Script)
{
	const FVersionedNiagaraScriptData* Data = Script ? Script->GetLatestScriptData() : nullptr;
	return Data ? Data->ModuleUsageBitmask : -1;
}

FString UFNBDevLibrary::SetModuleUsageBitmask(UNiagaraScript* Script, int32 Bitmask)
{
	FVersionedNiagaraScriptData* Data = Script ? Script->GetLatestScriptData() : nullptr;
	if (!Data)
	{
		return TEXT("No script data.");
	}
	if (Data->ModuleUsageBitmask == Bitmask)
	{
		return FString();
	}
	const FScopedTransaction Transaction(NSLOCTEXT("FNBDev", "SetModuleUsageBitmask", "Set Module Usage Bitmask"));
	Script->Modify();
	Data->ModuleUsageBitmask = Bitmask;
	Script->MarkPackageDirty();
	return FString();
}

FString UFNBDevLibrary::CompileMaterialErrors(UMaterialInterface* Material)
{
	UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
	if (!Base)
	{
		return TEXT("ERRORS: no material");
	}
	// A commandlet keeps no rendering resources, so compile a resource of our own for the Windows SM6 platform (as cooking does),
	// synchronously, and read its errors. A material instance compiles its static permutation.
	UMaterialInstance* Instance = Cast<UMaterialInstance>(Material);
	const EShaderPlatform Platform = SP_PCD3D_SM6;
	FMaterialResource* Resource = Instance ? Instance->AllocatePermutationResource() : Base->AllocateResource();
	if (!Resource)
	{
		return TEXT("NO RESOURCE");
	}
	Resource->SetMaterial(Base, Instance, Platform);
	const bool bCached = Resource->CacheShaders(EMaterialShaderPrecompileMode::Synchronous);
	Resource->FinishCompilation();
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	const TArray<FString> Errors = Resource->GetCompileErrors();
	const bool bMap = Resource->GetGameThreadShaderMap() != nullptr;
	FMaterial::DeferredDelete(Resource);
	if (Errors.IsEmpty() && bCached && bMap)
	{
		return TEXT("OK");
	}
	return FString::Printf(TEXT("ERRORS: cached=%d shadermap=%d\n"), bCached ? 1 : 0, bMap ? 1 : 0) + FString::Join(Errors, TEXT("\n"));
}

UEdGraph* UFNBDevLibrary::GetEmitterGraph(UNiagaraSystem* System, FName Emitter)
{
	FString Error;
	const FNiagaraEmitterHandle* Handle = FindHandle(System, Emitter, Error);
	UNiagaraScriptSource* Source = Handle ? EmitterSource(*Handle) : nullptr;
	return Source ? Source->NodeGraph : nullptr;
}

namespace
{
	/** The module call named Module in Script's stack of Emitter (Script: an ENiagaraScriptUsage name, or "Stage:<name>" / "Stage:#N")
	 * and its input named Input, with or without "Module.", as the stack lists them. False with OutError set when any is missing. */
	bool FindModuleInput(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, UNiagaraGraph*& OutGraph,
		UNiagaraNodeFunctionCall*& OutCall, FNiagaraVariable& OutInput, FString& OutError)
	{
		OutError.Reset();
		OutGraph = nullptr;
		OutCall = nullptr;
		const FNiagaraEmitterHandle* Handle = FindHandle(System, Emitter, OutError);
		UNiagaraScriptSource* Source = Handle ? EmitterSource(*Handle) : nullptr;
		UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
		if (!Graph)
		{
			OutError = OutError.IsEmpty() ? TEXT("The emitter has no graph.") : OutError;
			return false;
		}
		ENiagaraScriptUsage Usage;
		FGuid UsageId;
		if (!ResolveScript(*Handle, Script, Usage, UsageId, OutError))
		{
			return false;
		}
		UNiagaraNodeOutput* Output = FindOutputNode(*Graph, Usage, UsageId);
		if (!Output)
		{
			OutError = FString::Printf(TEXT("The emitter graph has no output node for %s."), *Script.ToString());
			return false;
		}
		UNiagaraNodeFunctionCall* Call = nullptr;
		TArray<FString> ModuleNames;
		for (UNiagaraNodeFunctionCall* Candidate : OrderedModules(*Output))
		{
			ModuleNames.Add(Candidate->GetFunctionName());
			if (Candidate->GetFunctionName() == Module.ToString())
			{
				Call = Candidate;
			}
		}
		if (!Call)
		{
			OutError = FString::Printf(TEXT("%s has no module %s (modules: %s)."), *Script.ToString(), *Module.ToString(), *FString::Join(ModuleNames, TEXT(", ")));
			return false;
		}

		// The module's inputs as the stack lists them ("Module.<name>"; a Set Parameters module names them after the target parameter).
		TArray<FNiagaraVariable> Inputs;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*Call, Inputs, FCompileConstantResolver(), FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
		const FString Wanted = Input.ToString();
		const FNiagaraVariable* Found = Inputs.FindByPredicate([&Wanted](const FNiagaraVariable& Variable)
		{
			const FString Name = Variable.GetName().ToString();
			return Name == Wanted || Name == TEXT("Module.") + Wanted;
		});
		if (!Found)
		{
			TArray<FString> InputNames;
			for (const FNiagaraVariable& Variable : Inputs)
			{
				InputNames.Add(Variable.GetName().ToString());
			}
			OutError = FString::Printf(TEXT("Module %s has no input %s (inputs: %s)."), *Module.ToString(), *Wanted, *FString::Join(InputNames, TEXT(", ")));
			return false;
		}
		OutGraph = Graph;
		OutCall = Call;
		OutInput = *Found;
		return true;
	}
}

UNiagaraDataInterface* UFNBDevLibrary::GetNiagaraInputDataInterface(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, FString& OutError, bool bCreate)
{
	UNiagaraGraph* Graph = nullptr;
	UNiagaraNodeFunctionCall* Call = nullptr;
	FNiagaraVariable FoundInput;
	if (!FindModuleInput(System, Emitter, Script, Module, Input, Graph, Call, FoundInput, OutError))
	{
		return nullptr;
	}
	const FNiagaraVariable* Found = &FoundInput;
	const FString Wanted = Input.ToString();
	UClass* InterfaceClass = Found->GetType().GetClass();
	if (!Found->GetType().IsDataInterface() || !InterfaceClass)
	{
		OutError = FString::Printf(TEXT("Input %s is a %s, not a data interface."), *Wanted, *Found->GetType().GetName());
		return nullptr;
	}

	// Look the override up first, changing nothing: a plain read must never dirty the asset.
	const FNiagaraParameterHandle Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(FNiagaraParameterHandle(Found->GetName()), Call);
	UEdGraphPin* OverridePin = FindOverridePin(*Call, Aliased.GetParameterHandleString());
	if (OverridePin && OverridePin->LinkedTo.Num() == 1 && OverridePin->LinkedTo[0])
	{
		UNiagaraDataInterface* Existing = InputNodeDataInterface(OverridePin->LinkedTo[0]->GetOwningNode());
		if (!Existing)
		{
			OutError = FString::Printf(TEXT("Input %s is linked to something other than a local data interface."), *Wanted);
		}
		return Existing;
	}
	if (OverridePin && OverridePin->LinkedTo.Num() > 1)
	{
		OutError = FString::Printf(TEXT("Input %s has %d links."), *Wanted, OverridePin->LinkedTo.Num());
		return nullptr;
	}
	if (!bCreate)
	{
		OutError = FString::Printf(TEXT("Input %s has no local data interface yet (pass bCreate to create one from the module's default)."), *Wanted);
		return nullptr;
	}

	// Create it the way the stack does when its placeholder is first edited: a local data interface seeded with the module's own
	// default (UNiagaraStackFunctionInput copies DefaultInputValues.DataObject into the placeholder). An input whose default is bound
	// to another parameter is a linked input there, never a local data interface, so it is refused.
	UNiagaraGraph* CalledGraph = Call->GetCalledGraph();
	const UNiagaraScriptVariable* ScriptVariable = CalledGraph ? CalledGraph->GetScriptVariable(Found->GetName()) : nullptr;
	if (ScriptVariable && ScriptVariable->DefaultMode == ENiagaraDefaultMode::Binding && ScriptVariable->DefaultBinding.IsValid())
	{
		OutError = FString::Printf(TEXT("The module binds input %s's default to %s; a local data interface would replace that link."), *Wanted,
			*ScriptVariable->DefaultBinding.GetName().ToString());
		return nullptr;
	}
	UNiagaraDataInterface* ModuleDefault = nullptr;
	if (!FindModuleDefaultDataInterface(CalledGraph, Found->GetName(), ModuleDefault, OutError))
	{
		return nullptr;
	}
	if (ModuleDefault && !ModuleDefault->IsA(InterfaceClass))
	{
		OutError = FString::Printf(TEXT("The module's default for %s is a %s, not a %s."), *Wanted, *ModuleDefault->GetClass()->GetName(), *InterfaceClass->GetName());
		return nullptr;
	}
	const FScopedTransaction Transaction(NSLOCTEXT("FNBDev", "CreateInputDataInterface", "Set a module input's data interface"));
	Graph->Modify();
	UEdGraphPin& NewOverridePin = OverridePin ? *OverridePin : FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Call, Aliased,
		Found->GetType(), ScriptVariable ? ScriptVariable->Metadata.GetVariableGuid() : FGuid(), FGuid());
	UNiagaraDataInterface* Created = nullptr;
	FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(NewOverridePin, InterfaceClass, Aliased.GetParameterHandleString().ToString(), Created);
	if (!Created)
	{
		OutError = FString::Printf(TEXT("Could not create the %s for input %s."), *InterfaceClass->GetName(), *Wanted);
		return nullptr;
	}
	if (ModuleDefault)
	{
		ModuleDefault->CopyTo(Created);
	}
	Graph->NotifyGraphChanged();
	return Created;
}

FString UFNBDevLibrary::SetNiagaraBinding(UObject* Object, const FString& Property, FName Parameter)
{
	void* Container = nullptr;
	TArray<FProperty*> Chain;
	FProperty* Found = Resolve(Object, Property, Container, &Chain);
	FStructProperty* StructProperty = CastField<FStructProperty>(Found);
	if (!StructProperty)
	{
		return FString::Printf(TEXT("%s has no struct property %s."), *GetNameSafe(Object), *Property);
	}
	// Which binding it is decides everything, before anything is touched.
	void* Value = StructProperty->ContainerPtrToValuePtr<void>(Container);
	const UScriptStruct* Struct = StructProperty->Struct;
	const UNiagaraSimulationStageBase* Stage = Cast<UNiagaraSimulationStageBase>(Object);
	TFunction<void()> Apply;
	if (Struct == FNiagaraUserParameterBinding::StaticStruct())
	{
		// As FNiagaraUserParameterBindingCustomization::ChangeSource: the name only; the type the data interface set stays.
		Apply = [Value, Parameter]() { static_cast<FNiagaraUserParameterBinding*>(Value)->Parameter.SetName(Parameter); };
	}
	else if (Struct == FNiagaraVariableDataInterfaceBinding::StaticStruct())
	{
		// As FNiagaraDataInterfaceBindingCustomization::ChangeSource.
		Apply = [Value, Parameter]() { static_cast<FNiagaraVariableDataInterfaceBinding*>(Value)->BoundVariable.SetName(Parameter); };
	}
	else if (Struct == FNiagaraVariableAttributeBinding::StaticStruct())
	{
		// As FNiagaraVariableAttributeBindingCustomization::ChangeSource for a simulation stage (emitter source mode).
		if (!Stage)
		{
			return TEXT("Attribute bindings are only set here on simulation stages.");
		}
		Apply = [Value, Parameter, Stage]()
		{
			static_cast<FNiagaraVariableAttributeBinding*>(Value)->SetValue(Parameter, Stage->GetOuterEmitterBase(), ENiagaraRendererSourceDataMode::Emitter);
		};
	}
	else
	{
		return FString::Printf(TEXT("%s is a %s, not a Niagara binding."), *Property, *Struct->GetName());
	}
	// The details panel's ChangeSource: one transaction, the change announced, and a data interface's compiled copy refreshed.
	EditProperty(Object, Chain, [&Apply]() { Apply(); return true; });
	return FString();
}

FString UFNBDevLibrary::GetNiagaraGpuHlsl(UNiagaraSystem* System, FName Emitter)
{
	FString Error;
	const FNiagaraEmitterHandle* Handle = FindHandle(System, Emitter, Error);
	const UNiagaraScript* Script = Handle ? Handle->GetEmitterData()->GetGPUComputeScript() : nullptr;
	return Script ? Script->GetVMExecutableData().LastHlslTranslationGPU : FString();
}

// --- Simulation stages ---------------------------------------------------------------------------------------------------------

UNiagaraSimulationStageBase* UFNBDevLibrary::AddSimulationStage(UNiagaraSystem* System, FName Emitter, FName StageName, const FString& AfterStage,
	FName IterationSource, FName EnabledBinding, UNiagaraScript* Module, FString& OutError, int32 NumIterations)
{
	OutError.Reset();
	OutError = AssetEditorRefusal(System);
	if (!OutError.IsEmpty())
	{
		return nullptr;
	}
	const FNiagaraEmitterHandle* Handle = FindHandle(System, Emitter, OutError);
	if (!Handle)
	{
		return nullptr;
	}
	const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
	UNiagaraEmitter* EmitterObject = VersionedEmitter.Emitter;
	FVersionedNiagaraEmitterData* Data = Handle->GetEmitterData();
	UNiagaraScriptSource* Source = EmitterSource(*Handle);
	UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
	if (!Graph)
	{
		OutError = TEXT("The emitter has no graph.");
		return nullptr;
	}

	// --- Everything is checked before anything changes. ---
	if (StageName.IsNone() || StageName.ToString().StartsWith(TEXT("#")) || StagesNamed(*Handle, StageName.ToString()).Num() > 0)
	{
		OutError = FString::Printf(TEXT("The stage needs a new unique name that does not start with # (%s)."), *StageName.ToString());
		return nullptr;
	}
	if (NumIterations < 1)
	{
		OutError = FString::Printf(TEXT("NumIterations must be at least 1 (%d)."), NumIterations);
		return nullptr;
	}
	UNiagaraGraph* ModuleGraph = nullptr;
	if (Module)
	{
		const FVersionedNiagaraScriptData* ModuleData = Module->GetLatestScriptData();
		if (Module->GetUsage() != ENiagaraScriptUsage::Module || !ModuleData
			|| !UNiagaraScript::IsSupportedUsageContextForBitmask(ModuleData->ModuleUsageBitmask, ENiagaraScriptUsage::ParticleSimulationStageScript))
		{
			OutError = FString::Printf(TEXT("%s is not a module script that may run in a simulation stage (its usage bitmask has no ParticleSimulationStageScript)."),
				*Module->GetPathName());
			return nullptr;
		}
		const UNiagaraScriptSource* ModuleSource = Cast<UNiagaraScriptSource>(Module->GetLatestSource());
		ModuleGraph = ModuleSource ? ModuleSource->NodeGraph : nullptr;
	}

	// Where it goes: right after the named stage, at index "#N", or last.
	int32 TargetIndex = INDEX_NONE;
	const TArray<UNiagaraSimulationStageBase*>& Stages = Data->GetSimulationStages();
	if (AfterStage.StartsWith(TEXT("#")))
	{
		if (!ParseStageIndex(AfterStage, TargetIndex) || TargetIndex > Stages.Num())
		{
			OutError = FString::Printf(TEXT("AfterStage %s is not an index #0..#%d (digits only)."), *AfterStage, Stages.Num());
			return nullptr;
		}
	}
	else if (!AfterStage.IsEmpty())
	{
		const UNiagaraSimulationStageBase* After = FindStage(*Handle, AfterStage, OutError);
		if (!After)
		{
			OutError = TEXT("Cannot place the stage: ") + OutError;
			return nullptr;
		}
		TargetIndex = Stages.IndexOfByKey(After) + 1;
	}
	if (TargetIndex == Stages.Num())
	{
		TargetIndex = INDEX_NONE;
	}

	// The iteration source must be a data interface the editor would offer (RW-derived, read or written on the emitter's maps).
	if (!IterationSource.IsNone())
	{
		const TSet<FName> Candidates = IterationSourceCandidates({Graph, ModuleGraph});
		if (!Candidates.Contains(IterationSource))
		{
			TArray<FString> Names;
			for (const FName& Candidate : Candidates)
			{
				Names.Add(Candidate.ToString());
			}
			Names.Sort();
			OutError = FString::Printf(TEXT("%s is not an iterable data interface of this emitter (iterable: %s)."), *IterationSource.ToString(),
				Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none"));
			return nullptr;
		}
	}

	// The enabled binding, resolved exactly as it will be on the stage. A user parameter resolves now (the exposed parameters); an
	// Emitter or System value only resolves against the last compile of the system scripts, so for those a miss is a warning. The
	// stage binds by name either way; only the editor's exists flag is cached when the binding is set (a compile or a reload does
	// not refresh it).
	FString Warnings;
	FNiagaraVariableAttributeBinding EnabledProbe;
	if (!EnabledBinding.IsNone())
	{
		const FNiagaraVariableBase BoolDefault(FNiagaraTypeDefinition::GetBoolDef(), NAME_None);
		EnabledProbe.Setup(BoolDefault, FNiagaraVariable(BoolDefault), ENiagaraRendererSourceDataMode::Emitter);
		EnabledProbe.SetValue(EnabledBinding, VersionedEmitter.ToBase(), ENiagaraRendererSourceDataMode::Emitter);
		if (!EnabledProbe.DoesBindingExistOnSource())
		{
			if (FNiagaraParameterHandle(EnabledBinding).IsUserHandle())
			{
				OutError = FString::Printf(TEXT("The enabled binding %s is not a bool user parameter of %s."), *EnabledBinding.ToString(), *System->GetName());
				return nullptr;
			}
			Warnings = FString::Printf(TEXT("WARNING: the enabled binding %s does not resolve yet (an Emitter or System value resolves once the system has "
				"compiled with it; the exists flag is cached now, so to confirm it, compile, set it again with SetNiagaraBinding and look for exists=1 in "
				"DescribeSimulationStages)."), *EnabledBinding.ToString());
		}
	}

	// --- Changes, as FNiagaraStackEmitterStageAddUtilities::ExecuteAddAction (EStageAddMode::SimulationStage). ---
	FScopedTransaction ScopedTransaction(NSLOCTEXT("FNBDev", "AddSimulationStage", "Add new simulation stage"));
	TSet<UEdGraphNode*> NodesBefore;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		NodesBefore.Add(Node);
	}
	EmitterObject->Modify();
	UNiagaraSimulationStageGeneric* Stage = NewObject<UNiagaraSimulationStageGeneric>(EmitterObject, UNiagaraSimulationStageGeneric::StaticClass(), NAME_None, RF_Transactional);
	Stage->Script = NewObject<UNiagaraScript>(Stage, MakeUniqueObjectName(Stage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")), EObjectFlags::RF_Transactional);
	Stage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
	Stage->Script->SetUsageId(Stage->GetMergeId());
	Stage->Script->SetLatestSource(Source);
	// The settings go in before the stage joins the emitter and are announced once at the end: every change event of a stage
	// requests a full system compile, which would otherwise translate each half-configured state.
	Stage->SimulationStageName = StageName;
	if (!IterationSource.IsNone())
	{
		Stage->IterationSource = ENiagaraIterationSource::DataInterface;
		Stage->DataInterface.BoundVariable.SetName(IterationSource);  // As FNiagaraDataInterfaceBindingCustomization::ChangeSource.
	}
	if (NumIterations != 1)
	{
		Stage->NumIterations.SetDefaultValueEditorOnly(NumIterations);
	}
	EmitterObject->AddSimulationStage(Stage, VersionedEmitter.Version);
	if (TargetIndex != INDEX_NONE)
	{
		EmitterObject->MoveSimulationStageToIndex(Stage, TargetIndex, VersionedEmitter.Version);
	}
	if (!EnabledBinding.IsNone())
	{
		// As FNiagaraVariableAttributeBindingCustomization::ChangeSource (emitter source mode); needs the stage's emitter version.
		Stage->EnabledBinding.SetValue(EnabledBinding, Stage->GetOuterEmitterBase(), ENiagaraRendererSourceDataMode::Emitter);
	}

	// A failure from here on takes the stage and every node it added out again, and drops the transaction.
	auto RollBack = [&](const FString& Problem)
	{
		DestroyNodesAddedSince(*Graph, NodesBefore);
		EmitterObject->RemoveSimulationStage(Stage, VersionedEmitter.Version);
		ScopedTransaction.Cancel();
		OutError = Problem;
		return nullptr;
	};
	UNiagaraNodeOutput* Output = ResetGraphForOutput(*Graph, ENiagaraScriptUsage::ParticleSimulationStageScript, Stage->Script->GetUsageId());
	if (!Output)
	{
		return RollBack(TEXT("Could not build the stage's output node; nothing was added."));
	}
	if (Module)
	{
		Graph->Modify();
		if (!FNiagaraStackGraphUtilities::AddScriptModuleToStack(Module, *Output))
		{
			return RollBack(FString::Printf(TEXT("Could not add %s to the stage; nothing was added."), *Module->GetPathName()));
		}
	}
	Graph->NotifyGraphChanged();
	Stage->OnChanged().Broadcast();
	Stage->RequestRecompile();
	OutError = Warnings;
	return Stage;
}

FString UFNBDevLibrary::RemoveSimulationStage(UNiagaraSystem* System, FName Emitter, FName StageName)
{
	FString Error = AssetEditorRefusal(System);
	if (!Error.IsEmpty())
	{
		return Error;
	}
	const FNiagaraEmitterHandle* Handle = FindHandle(System, Emitter, Error);
	if (!Handle)
	{
		return Error;
	}
	UNiagaraSimulationStageBase* Stage = FindStage(*Handle, StageName.ToString(), Error);
	if (!Stage)
	{
		return Error;
	}
	UNiagaraScriptSource* Source = EmitterSource(*Handle);
	if (!Stage->Script || !Source || !Source->NodeGraph)
	{
		return FString::Printf(TEXT("Simulation stage %s has no stage script or the emitter has no graph."), *StageName.ToString());
	}
	// As UNiagaraStackSimulationStageGroup::HasBaseSimulationStage: a stage inherited from a parent emitter is only deleted there.
	const FVersionedNiagaraEmitter Parent = Handle->GetEmitterData()->GetParent();
	const FVersionedNiagaraEmitterData* ParentData = Parent.Emitter ? Parent.GetEmitterData() : nullptr;
	if (ParentData && ParentData->GetSimulationStageById(Stage->Script->GetUsageId()))
	{
		return FString::Printf(TEXT("Simulation stage %s is inherited from the parent emitter %s; delete it there."), *StageName.ToString(), *Parent.Emitter->GetPathName());
	}
	// As UNiagaraStackSimulationStageGroup::Delete.
	const FVersionedNiagaraEmitter VersionedEmitter = Handle->GetInstance();
	FScopedTransaction Transaction(NSLOCTEXT("FNBDev", "DeleteSimulationStage", "Delete simulation stage"));
	VersionedEmitter.Emitter->Modify();
	Source->NodeGraph->Modify();
	TArray<UNiagaraNode*> StageNodes;
	Source->NodeGraph->BuildTraversal(StageNodes, ENiagaraScriptUsage::ParticleSimulationStageScript, Stage->Script->GetUsageId());
	for (UNiagaraNode* Node : StageNodes)
	{
		Node->Modify();
	}
	VersionedEmitter.Emitter->RemoveSimulationStage(Stage, VersionedEmitter.Version);
	for (UNiagaraNode* Node : StageNodes)
	{
		Node->DestroyNode();
	}
	Source->NodeGraph->NotifyGraphChanged();
	UNiagaraSystem::RequestCompileForEmitter(VersionedEmitter);
	return FString();
}

TArray<UNiagaraSimulationStageBase*> UFNBDevLibrary::GetSimulationStages(UNiagaraSystem* System, FName Emitter)
{
	FString Error;
	const FNiagaraEmitterHandle* Handle = FindHandle(System, Emitter, Error);
	return Handle ? TArray<UNiagaraSimulationStageBase*>(Handle->GetEmitterData()->GetSimulationStages()) : TArray<UNiagaraSimulationStageBase*>();
}

FString UFNBDevLibrary::DescribeSimulationStages(UNiagaraSystem* System, FName Emitter)
{
	FString Error;
	const FNiagaraEmitterHandle* Handle = FindHandle(System, Emitter, Error);
	if (!Handle)
	{
		return TEXT("ERROR: ") + Error;
	}
	UNiagaraScriptSource* Source = EmitterSource(*Handle);
	FString Out;
	int32 Index = 0;
	for (UNiagaraSimulationStageBase* Stage : Handle->GetEmitterData()->GetSimulationStages())
	{
		if (!Stage)
		{
			Out += FString::Printf(TEXT("STAGE %d <null>\n"), Index++);
			continue;
		}
		const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage);
		const FGuid UsageId = Stage->Script ? Stage->Script->GetUsageId() : FGuid();
		Out += FString::Printf(TEXT("STAGE %d '%s' (%s) enabled=%d"), Index++, *Stage->SimulationStageName.ToString(), *Stage->GetClass()->GetName(), Stage->bEnabled ? 1 : 0);
		if (Generic)
		{
			// Iterations: the bound parameter when there is one, else the constant.
			const FName IterationsParameter = Generic->NumIterations.ResolvedParameter.GetName();
			const FString Iterations = !IterationsParameter.IsNone() ? IterationsParameter.ToString()
				: Generic->NumIterations.GetDefaultValueArray().Num() == sizeof(int32) ? FString::FromInt(Generic->NumIterations.GetDefaultValue<int32>()) : TEXT("?");
			Out += FString::Printf(TEXT(" iterate=%s di=%s enabledBinding=%s(exists=%d) iterations=%s"), *IterationSourceText(Generic->IterationSource),
				*Generic->DataInterface.BoundVariable.GetName().ToString(), *Generic->EnabledBinding.GetParamMapBindableVariable().GetName().ToString(),
				Generic->EnabledBinding.DoesBindingExistOnSource() ? 1 : 0, *Iterations);
		}
		Out += FString::Printf(TEXT(" script=%s usage=%s id=%s\n"), *GetNameSafe(Stage->Script),
			Stage->Script ? *EnumText(TEXT("/Script/Niagara.ENiagaraScriptUsage"), static_cast<int64>(Stage->Script->GetUsage())) : TEXT("-"), *UsageId.ToString());
		UNiagaraNodeOutput* Output = Source && Source->NodeGraph ? FindOutputNode(*Source->NodeGraph, ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId) : nullptr;
		if (!Output)
		{
			Out += TEXT("    NO OUTPUT NODE\n");
			continue;
		}
		for (UNiagaraNodeFunctionCall* Call : OrderedModules(*Output))
		{
			Out += FString::Printf(TEXT("    MODULE %s (%s)%s\n"), *Call->GetFunctionName(), *GetPathNameSafe(Call->FunctionScript),
				Call->IsNodeEnabled() ? TEXT("") : TEXT(" DISABLED"));
		}
	}
	return Out;
}

// --- Graph editing -----------------------------------------------------------------

namespace
{
	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && Pin->PinName.ToString() == Name)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool IsAddPin(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == TEXT("Misc") && Pin->PinName == TEXT("Add");
	}

	bool IsCustomHlsl(const UEdGraphNode* Node)
	{
		return Node && Node->IsA<UNiagaraNodeCustomHlsl>();
	}

	/** After pins were added by hand: a new node change id and the graph marked for synchronization (a plain NotifyGraphChanged()
	 * leaves the graph's change id alone, so a non-forced compile would call the script up to date), then the editor refresh. */
	void MarkPinsChanged(UEdGraphNode& Node)
	{
		CastChecked<UNiagaraNode>(&Node)->MarkNodeRequiresSynchronization(TEXT("FNBDev map pin"), true);
		if (UEdGraph* Graph = Node.GetGraph())
		{
			Graph->NotifyGraphChanged();
		}
	}
}

TArray<UEdGraphNode*> UFNBDevLibrary::GetNodes(UEdGraph* Graph)
{
	return Graph ? TArray<UEdGraphNode*>(Graph->Nodes) : TArray<UEdGraphNode*>();
}

TArray<FString> UFNBDevLibrary::DescribePins(UEdGraphNode* Node)
{
	TArray<FString> Lines;
	if (!Node)
	{
		return Lines;
	}
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		TArray<FString> Links;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			Links.Add(FString::Printf(TEXT("%s.%s"), *Linked->GetOwningNode()->GetName(), *Linked->PinName.ToString()));
		}
		Lines.Add(FString::Printf(TEXT("%s %s %s/%s%s%s"), Pin->Direction == EGPD_Output ? TEXT("out") : TEXT("in"), *Pin->PinName.ToString(),
			*Pin->PinType.PinCategory.ToString(), Pin->PinType.PinSubCategoryObject.IsValid() ? *Pin->PinType.PinSubCategoryObject->GetName() : TEXT(""),
			Pin->bHidden ? TEXT(" (hidden)") : TEXT(""), Links.Num() ? *(TEXT(" -> ") + FString::Join(Links, TEXT(", "))) : TEXT("")));
	}
	return Lines;
}

UEdGraphNode* UFNBDevLibrary::AddCustomHlslNode(UEdGraph* Graph, const FString& Hlsl, int32 X, int32 Y, FString& OutError)
{
	if (!Cast<UNiagaraGraph>(Graph))
	{
		OutError = TEXT("Not a Niagara graph.");
		return nullptr;
	}
	// As the graph palette's "Custom Hlsl" action (FNiagaraSchemaAction_NewNode::PerformAction on its template node).
	const FScopedTransaction Transaction(NSLOCTEXT("FNBDev", "AddCustomHlsl", "Niagara Editor: New Node"));
	Graph->Modify();
	UNiagaraNodeCustomHlsl* Node = NewObject<UNiagaraNodeCustomHlsl>(Graph, NAME_None, RF_Transactional);
	if (FStrProperty* Property = FindFProperty<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl")))
	{
		Property->SetPropertyValue_InContainer(Node, Hlsl);
	}
	Graph->AddNode(Node, true, false);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	return Node;
}

FString UFNBDevLibrary::AddCustomHlslPins(UEdGraphNode* Node, const TArray<FString>& InputNames, const TArray<FString>& InputTypes,
	const TArray<FString>& OutputNames, const TArray<FString>& OutputTypes)
{
	FString OutError;
	if (!IsCustomHlsl(Node))
	{
		return TEXT("Not a Niagara custom HLSL node.");
	}
	if (InputNames.Num() != InputTypes.Num() || OutputNames.Num() != OutputTypes.Num())
	{
		return TEXT("Each pin needs a name and a type.");
	}
	FStructProperty* Property = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Signature"));
	if (!Property || !Property->Struct || Property->Struct->GetFName() != TEXT("NiagaraFunctionSignature"))
	{
		return TEXT("The node has no Signature property.");
	}
	FNiagaraFunctionSignature* Signature = Property->ContainerPtrToValuePtr<FNiagaraFunctionSignature>(Node);
	Node->Modify();
	auto Append = [&OutError](auto& Into, const TArray<FString>& Names, const TArray<FString>& Types)
	{
		using FElement = typename std::decay_t<decltype(Into)>::ElementType;
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Into.ContainsByPredicate([&](const FElement& Variable) { return Variable.GetName().ToString() == Names[Index]; }))
			{
				continue;
			}
			FNiagaraTypeDefinition Type;
			if (!TypeFromKeyword(Types[Index], Type))
			{
				OutError = FString::Printf(TEXT("Unknown pin type %s."), *Types[Index]);
				return false;
			}
			Into.Add(FElement(Type, FName(*Names[Index])));
		}
		return true;
	};
	if (!Append(Signature->Inputs, InputNames, InputTypes) || !Append(Signature->Outputs, OutputNames, OutputTypes))
	{
		return OutError;
	}
	// The node rebuilds its pins from the Signature on refresh (what SetCustomHlsl triggers); links of kept pins survive.
	UNiagaraNode* NiagaraNode = CastChecked<UNiagaraNode>(Node);
	NiagaraNode->RefreshFromExternalChanges();
	NiagaraNode->MarkNodeRequiresSynchronization(TEXT("FNBDev custom HLSL pins"), true);
	return FString();
}

FString UFNBDevLibrary::SetCustomHlsl(UEdGraphNode* Node, const FString& Hlsl)
{
	if (!IsCustomHlsl(Node))
	{
		return TEXT("Not a Niagara custom HLSL node.");
	}
	FStrProperty* Property = FindFProperty<FStrProperty>(Node->GetClass(), TEXT("CustomHlsl"));
	if (!Property)
	{
		return TEXT("The node has no CustomHlsl property.");
	}
	// UNiagaraNodeCustomHlsl::SetCustomHlsl is not exported; its change event does the same refresh and recompile request.
	Node->Modify();
	Node->PreEditChange(Property);
	Property->SetPropertyValue_InContainer(Node, Hlsl);
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Node->PostEditChangeProperty(Event);
	return FString();
}

FString UFNBDevLibrary::AddMapGetOutput(UEdGraphNode* MapGet, const FString& ParameterName, const FString& TypeName, const FString& DefaultValue)
{
	if (!MapGet || !MapGet->GetClass()->GetName().Equals(TEXT("NiagaraNodeParameterMapGet")))
	{
		return TEXT("Not a Niagara parameter map get node.");
	}
	if (FindPin(MapGet, ParameterName, EGPD_Output))
	{
		return TEXT("EXISTS");
	}
	FNiagaraTypeDefinition Type;
	if (!TypeFromKeyword(TypeName, Type))
	{
		return FString::Printf(TEXT("Unknown pin type %s."), *TypeName);
	}
	FMapProperty* MapProperty = FindFProperty<FMapProperty>(MapGet->GetClass(), TEXT("PinOutputToPinDefaultPersistentId"));
	if (!Type.IsDataInterface() && !MapProperty)
	{
		return TEXT("The node has no PinOutputToPinDefaultPersistentId map.");
	}
	MapGet->Modify();
	FEdGraphPinType PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(Type);
	PinType.PinSubCategory = TEXT("ParameterPin");
	UEdGraphPin* Output = MapGet->CreatePin(EGPD_Output, PinType, FName(*ParameterName));
	Output->PersistentGuid = FGuid::NewGuid();
	Output->PinFriendlyName = FText::FromString(ParameterName);
	if (!Type.IsDataInterface())
	{
		UEdGraphPin* Default = MapGet->CreatePin(EGPD_Input, PinType, NAME_None);
		Default->PersistentGuid = FGuid::NewGuid();
		Default->bHidden = true;
		Default->bNotConnectable = true;
		Default->bDefaultValueIsReadOnly = true;
		Default->DefaultValue = DefaultValue;
		Default->AutogeneratedDefaultValue = DefaultValue;
		Default->PinToolTip = FString::Printf(TEXT("Default value for %s if no other module has set it previously in the stack."), *ParameterName);
		FScriptMapHelper Helper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(MapGet));
		const FGuid Key = Output->PersistentGuid;
		const FGuid Value = Default->PersistentGuid;
		Helper.AddPair(&Key, &Value);
	}
	// Keep the add pin last among the outputs, as the node itself does.
	for (int32 Index = 0; Index < MapGet->Pins.Num(); ++Index)
	{
		if (IsAddPin(MapGet->Pins[Index]))
		{
			UEdGraphPin* Add = MapGet->Pins[Index];
			MapGet->Pins.RemoveAt(Index);
			MapGet->Pins.Add(Add);
			break;
		}
	}
	MarkPinsChanged(*MapGet);
	return FString();
}

FString UFNBDevLibrary::AddMapSetInput(UEdGraphNode* MapSet, const FString& ParameterName, const FString& TypeName)
{
	if (!MapSet || !MapSet->GetClass()->GetName().Equals(TEXT("NiagaraNodeParameterMapSet")))
	{
		return TEXT("Not a Niagara parameter map set node.");
	}
	if (FindPin(MapSet, ParameterName, EGPD_Input))
	{
		return TEXT("EXISTS");
	}
	FNiagaraTypeDefinition Type;
	if (!TypeFromKeyword(TypeName, Type))
	{
		return FString::Printf(TEXT("Unknown pin type %s."), *TypeName);
	}
	MapSet->Modify();
	FEdGraphPinType PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(Type);
	PinType.PinSubCategory = TEXT("ParameterPin");
	UEdGraphPin* Input = MapSet->CreatePin(EGPD_Input, PinType, FName(*ParameterName));
	Input->PersistentGuid = FGuid::NewGuid();
	Input->PinFriendlyName = FText::FromString(ParameterName);
	// Keep the input add pin after the parameters, as the node itself does.
	MapSet->Pins.Remove(Input);
	const int32 AddIndex = MapSet->Pins.IndexOfByPredicate([](const UEdGraphPin* Pin) { return Pin->Direction == EGPD_Input && IsAddPin(Pin); });
	MapSet->Pins.Insert(Input, AddIndex == INDEX_NONE ? MapSet->Pins.Num() : AddIndex);
	MarkPinsChanged(*MapSet);
	return FString();
}

FString UFNBDevLibrary::LinkPins(UEdGraphNode* FromNode, const FString& FromPin, UEdGraphNode* ToNode, const FString& ToPin)
{
	UEdGraphPin* From = FromNode ? FindPin(FromNode, FromPin, EGPD_Output) : nullptr;
	UEdGraphPin* To = ToNode ? FindPin(ToNode, ToPin, EGPD_Input) : nullptr;
	if (!From || !To)
	{
		return FString::Printf(TEXT("Pin not found: %s"), !From ? *FromPin : *ToPin);
	}
	if (From->LinkedTo.Contains(To))
	{
		return FString();
	}
	const UEdGraphSchema* Schema = ToNode->GetSchema();
	if (!Schema || !Schema->TryCreateConnection(From, To))
	{
		return FString::Printf(TEXT("The schema refused %s -> %s."), *FromPin, *ToPin);
	}
	return FString();
}

FString UFNBDevLibrary::RefreshNiagaraNode(UEdGraphNode* Node)
{
	UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node);
	if (!NiagaraNode)
	{
		return TEXT("Not a Niagara node.");
	}
	NiagaraNode->Modify();
	NiagaraNode->RefreshFromExternalChanges();
	NiagaraNode->MarkNodeRequiresSynchronization(TEXT("FNBDev refresh"), true);
	return FString();
}

FString UFNBDevLibrary::DescribeNiagaraNode(UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}
	FString Out = FString::Printf(TEXT("%s %s '%s'"), *Node->GetClass()->GetName(), *Node->GetName(),
		*Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Replace(TEXT("\n"), TEXT(" | ")));
	auto SignatureText = [](const FNiagaraFunctionSignature& Signature)
	{
		TArray<FString> Inputs;
		for (const FNiagaraVariable& Variable : Signature.Inputs)
		{
			Inputs.Add(Variable.GetName().ToString() + TEXT(":") + Variable.GetType().GetName());
		}
		TArray<FString> Outputs;
		for (const FNiagaraVariableBase& Variable : Signature.Outputs)
		{
			Outputs.Add(Variable.GetName().ToString() + TEXT(":") + Variable.GetType().GetName());
		}
		TArray<FString> Specifiers;
		for (const TPair<FName, FName>& Specifier : Signature.FunctionSpecifiers)
		{
			Specifiers.Add(Specifier.Key.ToString() + TEXT("=") + Specifier.Value.ToString());
		}
		FString Text = FString::Printf(TEXT(" signature=%s inputs=[%s] outputs=[%s]"), *Signature.Name.ToString(), *FString::Join(Inputs, TEXT(", ")), *FString::Join(Outputs, TEXT(", ")));
		if (Specifiers.Num())
		{
			Text += TEXT(" specifiers=[") + FString::Join(Specifiers, TEXT(", ")) + TEXT("]");
		}
		return Text;
	};
	// A custom HLSL node is a function call node too, so it comes first. Its code is a private property (read as SetCustomHlsl writes it).
	if (const UNiagaraNodeCustomHlsl* Custom = Cast<UNiagaraNodeCustomHlsl>(Node))
	{
		FString Code;
		if (const FStrProperty* Property = FindFProperty<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl")))
		{
			Code = Property->GetPropertyValue_InContainer(Custom);
		}
		return Out + TEXT(" custom-hlsl") + SignatureText(Custom->Signature) + TEXT("\nHLSL:\n") + Code;
	}
	if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node))
	{
		return Out + FString::Printf(TEXT(" function=%s script=%s"), *Call->GetFunctionName(), *GetPathNameSafe(Call->FunctionScript)) + SignatureText(Call->Signature);
	}
	if (const UNiagaraNodeOp* Op = Cast<UNiagaraNodeOp>(Node))
	{
		return Out + TEXT(" op=") + Op->OpName.ToString();
	}
	if (const UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
	{
		return Out + FString::Printf(TEXT(" input=%s:%s usage=%s"), *InputNode->Input.GetName().ToString(), *InputNode->Input.GetType().GetName(),
			*UEnum::GetValueAsString(InputNode->Usage));
	}
	return Out;
}

FString UFNBDevLibrary::LinkNiagaraModuleInput(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, FName Parameter)
{
	FString Error;
	UNiagaraGraph* Graph = nullptr;
	UNiagaraNodeFunctionCall* Call = nullptr;
	FNiagaraVariable Found;
	if (!FindModuleInput(System, Emitter, Script, Module, Input, Graph, Call, Found, Error))
	{
		return Error;
	}
	if (Found.GetType().IsDataInterface())
	{
		return FString::Printf(TEXT("Input %s is a data interface; bind it through GetNiagaraInputDataInterface and SetNiagaraBinding."), *Input.ToString());
	}
	// As UNiagaraStackFunctionInput::SetLinkedParameterValue: the override pin (made when there is none) linked to a parameter-map get of
	// Parameter with the input's own type. An override already there would have to be removed first, and the stack's remover
	// (RemoveNodesForStackFunctionInputOverridePin) is not exported, so such an input is refused rather than left with two values.
	const FNiagaraParameterHandle Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(FNiagaraParameterHandle(Found.GetName()), Call);
	if (const UEdGraphPin* Existing = FindOverridePin(*Call, Aliased.GetParameterHandleString()))
	{
		if (Existing->LinkedTo.Num() > 0 || !Existing->DefaultValue.IsEmpty())
		{
			return FString::Printf(TEXT("Input %s already has an override (a local value, data interface or link); clear it in the editor first."), *Input.ToString());
		}
	}
	UNiagaraGraph* CalledGraph = Call->GetCalledGraph();
	const UNiagaraScriptVariable* ScriptVariable = CalledGraph ? CalledGraph->GetScriptVariable(Found.GetName()) : nullptr;
	const FScopedTransaction Transaction(NSLOCTEXT("FNBDev", "LinkModuleInput", "Link a module input to a parameter"));
	Graph->Modify();
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Call, Aliased, Found.GetType(),
		ScriptVariable ? ScriptVariable->Metadata.GetVariableGuid() : FGuid(), FGuid());
	const FNiagaraVariableBase Linked(Found.GetType(), Parameter);
	TSet<FNiagaraVariableBase> Known;
	Known.Add(Linked);
	FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(OverridePin, Linked, Known);
	if (OverridePin.LinkedTo.Num() != 1)
	{
		return FString::Printf(TEXT("The link to %s was not made (the override pin has %d links)."), *Parameter.ToString(), OverridePin.LinkedTo.Num());
	}
	Graph->NotifyGraphChanged();
	return FString();
}

namespace
{
	/** System's exposed user parameter called Name, with or without the "User." namespace. */
	bool FindUserParameter(UNiagaraSystem& System, FName Name, FNiagaraVariable& Out)
	{
		FString Wanted = Name.ToString();
		if (!Wanted.StartsWith(TEXT("User.")))
		{
			Wanted = TEXT("User.") + Wanted;
		}
		TArray<FNiagaraVariable> Parameters;
		System.GetExposedParameters().GetParameters(Parameters);
		for (const FNiagaraVariable& Parameter : Parameters)
		{
			if (Parameter.GetName().ToString() == Wanted)
			{
				Out = Parameter;
				return true;
			}
		}
		return false;
	}
}

FString UFNBDevLibrary::DescribeUserVariables(UNiagaraSystem* System)
{
	if (!System)
	{
		return TEXT("ERROR: no system");
	}
	TArray<FNiagaraVariable> Parameters;
	System->GetExposedParameters().GetParameters(Parameters);
	FString Out;
	for (const FNiagaraVariable& Parameter : Parameters)
	{
		FString Description;
		if (const UNiagaraScriptVariable* ScriptVariable = FNiagaraEditorUtilities::UserParameters::FindScriptVariableForUserParameter(Parameter, *System))
		{
			Description = ScriptVariable->Metadata.Description.ToString().Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT(""));
		}
		Out += FString::Printf(TEXT("%s\t%s\t%s\n"), *Parameter.GetName().ToString(), *Parameter.GetType().GetName(), *Description);
	}
	return Out;
}

FString UFNBDevLibrary::SetUserVariableDescription(UNiagaraSystem* System, FName Name, const FString& Description)
{
	if (!System)
	{
		return TEXT("ERROR: no system");
	}
	FNiagaraVariable Parameter;
	if (!FindUserParameter(*System, Name, Parameter))
	{
		return FString::Printf(TEXT("ERROR: %s has no user parameter %s"), *System->GetName(), *Name.ToString());
	}
	UNiagaraScriptVariable* ScriptVariable = FNiagaraEditorUtilities::UserParameters::GetScriptVariableForUserParameter(Parameter, *System);
	if (!ScriptVariable)
	{
		return FString::Printf(TEXT("ERROR: no script variable for %s"), *Parameter.GetName().ToString());
	}
	const FScopedTransaction Transaction(NSLOCTEXT("FNBDev", "SetUserVariableDescription", "Set User Parameter Description"));
	System->Modify();
	ScriptVariable->Modify();
	ScriptVariable->Metadata.Description = FText::FromString(Description);
	System->MarkPackageDirty();
	return FString();
}

namespace
{
	/** The system's user parameter hierarchy: what the Niagara editor's User Parameters hierarchy editor arranges, and what a Niagara
	 * component's User Parameters (a NinjaLive's details panel) groups its rows by. */
	UHierarchyRoot* UserParameterHierarchy(UNiagaraSystem& System)
	{
		UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System.GetEditorData());
		return EditorData ? EditorData->UserParameterHierarchy.Get() : nullptr;
	}

	/** A private reflected property of an element, by name (the hierarchy's item, category and metadata classes are not exported). */
	template<typename PropertyType>
	PropertyType* ElementProperty(const UStruct* Owner, const TCHAR* Name)
	{
		return Owner ? FindFProperty<PropertyType>(Owner, Name) : nullptr;
	}

	FString ElementLabel(const UHierarchyElement* Element)
	{
		if (const UHierarchyCategory* Category = Cast<UHierarchyCategory>(Element))
		{
			return FString::Printf(TEXT("category '%s'"), *Category->GetCategoryName().ToString());
		}
		if (const FObjectPropertyBase* Variable = ElementProperty<FObjectPropertyBase>(Element->GetClass(), TEXT("UserParameterScriptVariable")))
		{
			if (const UNiagaraScriptVariable* ScriptVariable = Cast<UNiagaraScriptVariable>(Variable->GetObjectPropertyValue_InContainer(Element)))
			{
				return FString::Printf(TEXT("parameter %s"), *ScriptVariable->Variable.GetName().ToString());
			}
		}
		return Element->GetClass()->GetName();
	}

	/** The section a category is shown under (its section association metadata), or null. */
	const UHierarchySection* CategorySection(const UHierarchyCategory& Category)
	{
		const UScriptStruct* AssociationStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/DataHierarchyEditor.DataHierarchyElementMetaData_SectionAssociation"));
		const FMapProperty* MetaDataProperty = ElementProperty<FMapProperty>(UHierarchyElement::StaticClass(), TEXT("MetaData"));
		if (!AssociationStruct || !MetaDataProperty)
		{
			return nullptr;
		}
		FScriptMapHelper Map(MetaDataProperty, MetaDataProperty->ContainerPtrToValuePtr<void>(&Category));
		for (FScriptMapHelper::FIterator It(Map); It; ++It)
		{
			const FObjectPropertyBase* KeyProperty = CastField<FObjectPropertyBase>(Map.GetKeyProperty());
			if (KeyProperty && KeyProperty->GetObjectPropertyValue(Map.GetKeyPtr(It)) == AssociationStruct)
			{
				const FInstancedStruct& Value = *reinterpret_cast<const FInstancedStruct*>(Map.GetValuePtr(It));
				const FWeakObjectProperty* SectionProperty = ElementProperty<FWeakObjectProperty>(AssociationStruct, TEXT("Section"));
				return SectionProperty && Value.GetMemory() ? Cast<UHierarchySection>(SectionProperty->GetObjectPropertyValue_InContainer(Value.GetMemory())) : nullptr;
			}
		}
		return nullptr;
	}

	/** Puts Category under Section (its section association metadata), as the hierarchy editor does when a category is dropped on a section. */
	bool AssociateWithSection(UHierarchyCategory& Category, const UHierarchySection& Section)
	{
		UScriptStruct* AssociationStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/DataHierarchyEditor.DataHierarchyElementMetaData_SectionAssociation"));
		const FMapProperty* MetaDataProperty = ElementProperty<FMapProperty>(UHierarchyElement::StaticClass(), TEXT("MetaData"));
		const FWeakObjectProperty* SectionProperty = ElementProperty<FWeakObjectProperty>(AssociationStruct, TEXT("Section"));
		if (!AssociationStruct || !MetaDataProperty || !SectionProperty)
		{
			return false;
		}
		FInstancedStruct Value;
		Value.InitializeAs(AssociationStruct);
		SectionProperty->SetObjectPropertyValue_InContainer(Value.GetMutableMemory(), const_cast<UHierarchySection*>(&Section));
		FScriptMapHelper Map(MetaDataProperty, MetaDataProperty->ContainerPtrToValuePtr<void>(&Category));
		TObjectPtr<UStruct> Key = AssociationStruct;
		Map.AddPair(&Key, &Value);
		return true;
	}

	/** Takes the hierarchy item for the user parameter with this script variable out of wherever it sits (root or a category). */
	void RemoveUserParameterItem(UHierarchyElement& Parent, const UNiagaraScriptVariable& ScriptVariable)
	{
		TArray<TObjectPtr<UHierarchyElement>>& Children = Parent.GetChildrenMutable();
		for (int32 Index = Children.Num() - 1; Index >= 0; --Index)
		{
			UHierarchyElement* Child = Children[Index];
			if (!Child)
			{
				continue;
			}
			const FObjectPropertyBase* Variable = ElementProperty<FObjectPropertyBase>(Child->GetClass(), TEXT("UserParameterScriptVariable"));
			const UObject* Held = Variable ? Variable->GetObjectPropertyValue_InContainer(Child) : nullptr;
			const bool bSameGuid = Child->GetPersistentIdentity().Guids.Contains(ScriptVariable.Metadata.GetVariableGuid());
			if (Held == &ScriptVariable || (Variable && bSameGuid))
			{
				Child->Modify();
				Children.RemoveAt(Index);
			}
			else
			{
				RemoveUserParameterItem(*Child, ScriptVariable);
			}
		}
	}
}

FString UFNBDevLibrary::CategorizeUserParameters(UNiagaraSystem* System, FName SectionName, const FString& SectionTooltip, FName CategoryName,
	const FString& CategoryTooltip, const TArray<FName>& Names)
{
	if (!System || CategoryName.IsNone())
	{
		return TEXT("ERROR: no system or no category name");
	}
	UHierarchyRoot* Root = UserParameterHierarchy(*System);
	UClass* ItemClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraHierarchyUserParameter"));
	if (!Root || !ItemClass)
	{
		return TEXT("ERROR: the system has no user parameter hierarchy, or NiagaraEditor's hierarchy item class is missing");
	}
	const FObjectPropertyBase* ItemVariable = ElementProperty<FObjectPropertyBase>(ItemClass, TEXT("UserParameterScriptVariable"));
	const FTextProperty* CategoryTooltipProperty = ElementProperty<FTextProperty>(UHierarchyCategory::StaticClass(), TEXT("Tooltip"));
	if (!ItemVariable || !CategoryTooltipProperty)
	{
		return TEXT("ERROR: the hierarchy classes changed (UserParameterScriptVariable / Tooltip not found)");
	}
	// Every parameter first, so a misspelt name changes nothing.
	TArray<UNiagaraScriptVariable*> ScriptVariables;
	TArray<FString> Errors;
	for (const FName Name : Names)
	{
		FNiagaraVariable Parameter;
		UNiagaraScriptVariable* ScriptVariable = FindUserParameter(*System, Name, Parameter)
			? FNiagaraEditorUtilities::UserParameters::GetScriptVariableForUserParameter(Parameter, *System) : nullptr;
		if (!ScriptVariable)
		{
			Errors.Add(FString::Printf(TEXT("ERROR: %s has no user parameter %s"), *System->GetName(), *Name.ToString()));
		}
		ScriptVariables.Add(ScriptVariable);
	}
	if (!Errors.IsEmpty())
	{
		return FString::Join(Errors, TEXT("\n"));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("FNBDev", "CategorizeUserParameters", "Categorize User Parameters"));
	System->Modify();
	Root->Modify();

	// The section (one per name), then the category (one per name, at the root, under the section).
	UHierarchySection* Section = nullptr;
	if (!SectionName.IsNone())
	{
		for (UHierarchySection* Existing : Root->GetSectionDataMutable())
		{
			if (Existing && Existing->GetSectionName() == SectionName)
			{
				Section = Existing;
			}
		}
		if (!Section)
		{
			Section = NewObject<UHierarchySection>(Root);
			Section->SetFlags(RF_Transactional);
			Section->SetSectionName(SectionName);
			Root->GetSectionDataMutable().Add(Section);
		}
		Section->Modify();
		Section->SetTooltip(FText::FromString(SectionTooltip));
	}
	UHierarchyCategory* Category = nullptr;
	for (const TObjectPtr<UHierarchyElement>& Child : Root->GetChildren())
	{
		UHierarchyCategory* Existing = Cast<UHierarchyCategory>(Child.Get());
		if (Existing && Existing->GetCategoryName() == CategoryName)
		{
			Category = Existing;
		}
	}
	if (!Category)
	{
		Category = Root->AddChild<UHierarchyCategory>();
		Category->SetCategoryName(CategoryName);
	}
	Category->Modify();
	// A category's identity from its name (the hierarchy editor's own is a random GUID), so the same edit saves the same file anywhere.
	Category->SetIdentity(FHierarchyElementIdentity({FGuid::NewDeterministicGuid(FString::Printf(TEXT("FluidNinjaBuoyancy.UserParameterCategory.%s"),
		*CategoryName.ToString()))}, {TEXT("Category")}));
	CategoryTooltipProperty->SetPropertyValue_InContainer(Category, FText::FromString(CategoryTooltip));
	if (Section && CategorySection(*Category) != Section && !AssociateWithSection(*Category, *Section))
	{
		return TEXT("ERROR: could not put the category under its section (the section association metadata changed)");
	}

	// Each parameter into the category, in the order given. An item already there is kept (a second run changes nothing); one elsewhere
	// (the root, another category) leaves that place.
	TArray<TObjectPtr<UHierarchyElement>> Ordered;
	for (UNiagaraScriptVariable* ScriptVariable : ScriptVariables)
	{
		UHierarchyElement* Item = nullptr;
		for (const TObjectPtr<UHierarchyElement>& Child : Category->GetChildren())
		{
			if (Child && ItemVariable->GetObjectPropertyValue_InContainer(Child.Get()) == ScriptVariable)
			{
				Item = Child.Get();
			}
		}
		if (Item)
		{
			Category->GetChildrenMutable().Remove(Item);
		}
		RemoveUserParameterItem(*Root, *ScriptVariable);
		if (!Item)
		{
			Item = NewObject<UHierarchyElement>(Category, ItemClass);
			Item->SetFlags(RF_Transactional);
			ItemVariable->SetObjectPropertyValue_InContainer(Item, ScriptVariable);
			Item->SetIdentity(FHierarchyElementIdentity({ScriptVariable->Metadata.GetVariableGuid()}, {}));
		}
		Ordered.Add(Item);
	}
	// Anything else the category held (an item a user dropped in) stays, after ours.
	Ordered.Append(Category->GetChildrenMutable());
	Category->GetChildrenMutable() = MoveTemp(Ordered);
	System->MarkPackageDirty();
	return FString();
}

FString UFNBDevLibrary::DescribeUserParameterHierarchy(UNiagaraSystem* System)
{
	const UHierarchyRoot* Root = System ? UserParameterHierarchy(*System) : nullptr;
	if (!Root)
	{
		return TEXT("ERROR: no system or no user parameter hierarchy");
	}
	FString Out;
	for (const UHierarchySection* Section : Root->GetSectionData())
	{
		Out += FString::Printf(TEXT("section '%s'\n"), Section ? *Section->GetSectionName().ToString() : TEXT("None"));
	}
	for (const TObjectPtr<UHierarchyElement>& Child : Root->GetChildren())
	{
		if (!Child)
		{
			continue;
		}
		const UHierarchyCategory* Category = Cast<UHierarchyCategory>(Child.Get());
		const UHierarchySection* Section = Category ? CategorySection(*Category) : nullptr;
		Out += FString::Printf(TEXT("%s%s\n"), *ElementLabel(Child), Section ? *FString::Printf(TEXT(" (section '%s')"), *Section->GetSectionName().ToString()) : TEXT(""));
		for (const TObjectPtr<UHierarchyElement>& Item : Child->GetChildren())
		{
			if (Item)
			{
				Out += FString::Printf(TEXT("\t%s\n"), *ElementLabel(Item));
			}
		}
	}
	return Out;
}

FString UFNBDevLibrary::DescribeBlueprintVariables(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return TEXT("ERROR: no Blueprint");
	}
	FString Out;
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		TArray<FString> Flags;
		if (!(Variable.PropertyFlags & CPF_DisableEditOnInstance))
		{
			Flags.Add(TEXT("edit"));
		}
		if (Variable.PropertyFlags & CPF_ExposeOnSpawn)
		{
			Flags.Add(TEXT("expose"));
		}
		if (!(Variable.PropertyFlags & CPF_BlueprintReadOnly))
		{
			Flags.Add(TEXT("bp"));
		}
		TArray<FString> Range;
		for (const TCHAR* Key : {TEXT("ClampMin"), TEXT("ClampMax"), TEXT("UIMin"), TEXT("UIMax"), TEXT("Units")})
		{
			if (Variable.HasMetaData(Key))
			{
				Range.Add(FString::Printf(TEXT("%s=%s"), Key, *Variable.GetMetaData(Key)));
			}
		}
		const FString Tooltip = Variable.HasMetaData(FBlueprintMetadata::MD_Tooltip) ? Variable.GetMetaData(FBlueprintMetadata::MD_Tooltip) : FString();
		Out += FString::Printf(TEXT("%s\t%s\t%s\t%s\t%s\n"), *Variable.VarName.ToString(), *UEdGraphSchema_K2::TypeToText(Variable.VarType).ToString(),
			*FString::Join(Flags, TEXT(",")), *Tooltip.Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT("")), *FString::Join(Range, TEXT(" ")));
	}
	return Out;
}

FString UFNBDevLibrary::SetBlueprintVariableMetaData(UBlueprint* Blueprint, FName Variable, FName Key, const FString& Value)
{
	if (!Blueprint)
	{
		return TEXT("ERROR: no Blueprint");
	}
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Variable) == INDEX_NONE)
	{
		return FString::Printf(TEXT("ERROR: %s has no member variable %s"), *Blueprint->GetName(), *Variable.ToString());
	}
	if (Value.IsEmpty())
	{
		FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, Variable, nullptr, Key);
	}
	else
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, Variable, nullptr, Key, Value);
	}
	return FString();
}
