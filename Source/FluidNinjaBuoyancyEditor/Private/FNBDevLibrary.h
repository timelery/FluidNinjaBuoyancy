#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FNBDevLibrary.generated.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UMaterialInterface;
class UNiagaraDataInterface;
class UNiagaraEmitter;
class UNiagaraScript;
class UNiagaraSimulationStageBase;
class UNiagaraSystem;

/**
 * Editor-only helpers the plugin's Python tools call: Niagara and material editing for Tools/CorePatch/apply_fluidninja_waves.py (the
 * FluidNinja core patch: apply, verify, inspect) and for the development host's content build. They live in the plugin's editor module
 * so that the core patch runs from any project that has the plugin built; nothing of them reaches a cooked game.
 *
 * Type keywords (case-insensitive) used by every "Type" argument: float, int, bool, vec2, vec3, vec4, color, position, matrix, quat;
 * data interfaces Grid2DCollection, RenderTarget2D, Texture (the Texture Sample DI); objects TextureObject (UTexture) and
 * RenderTargetObject (UTextureRenderTarget); or the class path of a data interface or texture class, such as
 * /Script/Niagara.NiagaraDataInterfaceGrid2DCollection (any other class is refused).
 * AddUserVariable reads "Texture" as the UTexture object parameter a Texture DI binds to (TextureUserParameter).
 *
 * Script names: an ENiagaraScriptUsage name (EmitterSpawnScript, ParticleUpdateScript, ...). The helpers that walk the graph
 * themselves (GetNiagaraInputDataInterface, LinkNiagaraModuleInput) also take "Stage:<SimulationStageName>" or "Stage:#N" (the stage
 * at index N); the ExternalEdit-based helpers (DescribeNiagara, AddNiagaraModuleAfter, SetNiagaraInput*) cannot address simulation stages.
 * A stage name that several stages share is an error wherever a stage is named; address such a stage as "#N".
 *
 * Data interface edits (SetPropertyText, SetNiagaraBinding on an object from GetNiagaraInputDataInterface) also update the
 * compiled copy the system runs with, as the details panel does, so they need no forced compile. Stage edits (AddSimulationStage,
 * RemoveSimulationStage) need the system's asset editor closed.
 */
UCLASS()
class UFNBDevLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Pastes actors from T3D text (as copied in the editor) into World's current level; returns how many actors it added. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static int32 PasteActorsText(UWorld* World, const FString& Text);

	/** A property (path through structs, e.g. "BodyInstance.MassInKgOverride") of Object as export text. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString GetPropertyText(UObject* Object, const FString& Path);

	/** Sets a property (path through structs) of Object from import text, as one details-panel edit (a transaction, the change event
	 * with the top-level property as the member property); a data interface inside a system also gets its compiled copy updated.
	 * False if the path or text is wrong. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static bool SetPropertyText(UObject* Object, const FString& Path, const FString& Text);

	/** The compiled (runtime) copy of an editor data interface (one from GetNiagaraInputDataInterface): the object the system's
	 * compiled scripts store and run with. Null before the system has compiled, or when the data interface feeds no stack. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static UNiagaraDataInterface* GetNiagaraCompiledDataInterface(UNiagaraSystem* System, UNiagaraDataInterface* EditorDataInterface);

	/** Undoes the editor's last transaction (bRedo: redoes the last undone one), as Edit > Undo does; false when there was none. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static bool UndoLastTransaction(bool bRedo = false);

	/** Every property of Object that differs from its class default, as "name = export text" lines. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString DescribeChangedProperties(UObject* Object);

	/** A new Niagara system asset copied from Template; errors (if any) in OutErrors. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static UNiagaraSystem* CreateNiagaraSystem(const FString& Name, const FString& Path, UNiagaraSystem* Template, FString& OutErrors);

	/** Adds a copy of the emitter asset Template to System under Name; returns errors, empty when it worked. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString AddNiagaraEmitter(UNiagaraSystem* System, UNiagaraEmitter* Template, FName Name);

	/** Sets emitter properties from JSON (FVersionedNiagaraEmitterData fields, e.g. {"SimTarget": "GPUComputeSim"}); returns errors. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString SetNiagaraEmitterProperties(UNiagaraSystem* System, FName Emitter, const FString& Json);

	/** A module input set to an HLSL expression ("float4(1, 1, 1, 0.8)", "36", "true"); returns errors, empty when it worked. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString SetNiagaraInputHlsl(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, const FString& Hlsl);

	/** An enum module input set to the entry whose name or display name contains Entry; returns errors, empty when it worked. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString SetNiagaraInputEnum(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, const FString& Entry);

	/** A module input's current value as text (its kind and value). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString GetNiagaraInputText(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input);

	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString RemoveNiagaraEmitter(UNiagaraSystem* System, FName Emitter);

	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString SetNiagaraModuleEnabled(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, bool bEnabled);

	/** Text dump: with no Emitter, the system summary and its user variables ("name : type = default"); with Emitter and Script,
	 * that stack's modules and their inputs (name, type) in execution order. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString DescribeNiagara(UNiagaraSystem* System, FName Emitter, FName Script);

	/** Adds the module asset at ModulePath to the stack right after AfterModule; returns the new module's name (or "ERROR: ..."). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString AddNiagaraModuleAfter(UNiagaraSystem* System, FName Emitter, FName Script, FName AfterModule, const FString& ModulePath);

	/** Adds a Set Parameters module right after AfterModule that sets Parameter (Type: any type keyword, data interfaces included);
	 * returns the new module's name (or "ERROR: ..."). Its input is named after the parameter. A data interface's properties are set
	 * on the object GetNiagaraInputDataInterface returns. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString AddNiagaraSetParameterAfter(UNiagaraSystem* System, FName Emitter, FName Script, FName AfterModule, FName Parameter, const FString& Type);

	/** A module input linked to a parameter (e.g. "User.VelocityDensityBuffer"), with the input's own type; returns errors. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString SetNiagaraInputLinked(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, FName Linked);

	/** Adds a float user variable (name without the "User." prefix) with a default; returns errors. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString AddNiagaraUserFloat(UNiagaraSystem* System, FName Name, float Default);

	/** Adds a user variable of Type (bool, int, float, vec2, vec3, vec4, color, position, Texture = UTexture object, RenderTargetObject)
	 * with DefaultValue as text ("true", "3", "0.5", "1,2", "1,2,3,4", or an asset path / empty for objects). Returns the exposed name
	 * as the system stores it (e.g. "User.WaveSimEnabled"), or "ERROR: ...". */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString AddUserVariable(UNiagaraSystem* System, FName Name, const FString& Type, const FString& DefaultValue);

	/** The local data interface object of a module input (for example the "Emitter.WaveGrid" input of a Set Parameters module).
	 * Without bCreate it only looks (never changes or dirties the asset) and returns null with OutError set when the input has no
	 * local data interface yet. With bCreate it creates one in a transaction, seeded with the module's own default as the stack does
	 * (the default pin's data interface, or the class defaults); an input whose module default is linked to another value is refused.
	 * Set its properties with SetPropertyText and its bindings with SetNiagaraBinding. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static UNiagaraDataInterface* GetNiagaraInputDataInterface(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, FString& OutError,
		bool bCreate = false);

	/** Points a Niagara binding property of Object at Parameter the way the details panel does (one transaction; a data interface's
	 * compiled copy updated): a user parameter binding (TextureUserParameter, RenderTargetUserParameter: name only, the type is kept),
	 * a data interface binding (a simulation stage's DataInterface) or an attribute binding on a simulation stage (EnabledBinding).
	 * Returns errors, empty when it worked; an unsupported property changes nothing. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString SetNiagaraBinding(UObject* Object, const FString& Property, FName Parameter);

	/** Every property of Object's class whose name contains Filter (case-insensitive; empty: all): "name : type = value" lines. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString ListProperties(UObject* Object, const FString& Filter);

	/** Requests a compile of the system (bForce: even when nothing changed) and, with bWait, waits for it, GPU shaders included.
	 * With bWait it first finishes the compiles earlier edits requested, so a stale in-flight snapshot cannot land after this one.
	 * Returns "STATUS <aggregate> errors=<0|1> warnings=<0|1> compiling=<0|1>", one line per script with its warnings and errors, and
	 * per GPU emitter "GPUSHADER <emitter> finished=<0|1> shadermap=<valid|failed|missing> errors=<n>" with the shader compiler's
	 * messages as "GPU Error ..." (containing "err0r" in any case, or ": error") or "GPU Warning ..." lines (a failed or missing shader
	 * map counts as an error: a failed result served from the cache brings no messages). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString CompileNiagaraSystem(UNiagaraSystem* System, bool bWait = true, bool bForce = false);

	/** The last GPU HLSL translation of a GPU emitter's compute script (all its simulation stages), empty when there is none. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev")
	static FString GetNiagaraGpuHlsl(UNiagaraSystem* System, FName Emitter);

	// --- Simulation stages ---------------------------------------------------------------------------------------------------

	/** Adds a Generic Simulation Stage named StageName to Emitter, as the editor's "add simulation stage" action does (a stage script
	 * keyed by the stage's merge id and its own output node in the emitter graph), placed right after the stage named AfterStage
	 * ("#N": at index N, digits only; empty: last). IterationSource: a data interface parameter to iterate (e.g. "Emitter.WaveGrid";
	 * None: particles); it must be one the editor offers, a UNiagaraDataInterfaceRWBase type read or written on the emitter's maps.
	 * EnabledBinding: a bool parameter that switches the stage (e.g. "User.WaveSimEnabled"; None: always on). Module: a module script
	 * allowed in simulation stages, added to the stage's stack (may be null). The system's asset editor must be closed.
	 * Everything is checked first; the stage is configured and then announced once (one compile request). Returns the stage (OutError
	 * then empty, or a "WARNING: ..." line for an Emitter / System enabled binding that only resolves after a compile: the stage binds
	 * it by name, but the editor's exists flag is cached when set, so re-set it with SetNiagaraBinding after a compile to see exists=1),
	 * or null with OutError set and nothing changed. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Stages")
	static UNiagaraSimulationStageBase* AddSimulationStage(UNiagaraSystem* System, FName Emitter, FName StageName, const FString& AfterStage,
		FName IterationSource, FName EnabledBinding, UNiagaraScript* Module, FString& OutError, int32 NumIterations = 1);

	/** Removes the simulation stage named StageName ("#N": the stage at index N) and its stack nodes, as the editor's delete does;
	 * refuses a stage inherited from a parent emitter, and works only with the system's asset editor closed. Returns errors. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Stages")
	static FString RemoveSimulationStage(UNiagaraSystem* System, FName Emitter, FName StageName);

	/** The emitter's simulation stages in execution order (for get_object_properties / GetPropertyText). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Stages")
	static TArray<UNiagaraSimulationStageBase*> GetSimulationStages(UNiagaraSystem* System, FName Emitter);

	/** One line per simulation stage (index, name, class, enabled, iteration source, data interface, enabled binding, iterations,
	 * stage script and usage id) followed by its modules in stack order. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Stages")
	static FString DescribeSimulationStages(UNiagaraSystem* System, FName Emitter);

	// --- Graph editing (module and emitter graphs) ---------------------------------------------------------------------------

	/** The node graph of a script asset (a module, function or dynamic input). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static UEdGraph* GetScriptGraph(UNiagaraScript* Script);

	/** A module script's ModuleUsageBitmask (bit 1 << ENiagaraScriptUsage per stack it may be added to); -1 without script data. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static int32 GetModuleUsageBitmask(UNiagaraScript* Script);

	/** Sets a module script's ModuleUsageBitmask (latest version) in a transaction; returns errors, empty when it worked. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static FString SetModuleUsageBitmask(UNiagaraScript* Script, int32 Bitmask);

	/** Compiles a material or material instance (its static permutation) into a resource of its own for Windows SM6, synchronously
	 * (works in commandlets, which keep no rendering resources), and returns "OK" or "ERRORS: ..." with the compile errors. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Material")
	static FString CompileMaterialErrors(UMaterialInterface* Material);

	/** The node graph shared by all of an emitter's stacks. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static UEdGraph* GetEmitterGraph(UNiagaraSystem* System, FName Emitter);

	/** Nodes of a graph. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static TArray<UEdGraphNode*> GetNodes(UEdGraph* Graph);

	/** One line per pin: "<in|out> <name> <category>/<sub-object> -> <node>.<pin>, ...". */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static TArray<FString> DescribePins(UEdGraphNode* Node);

	/** Adds a Custom HLSL node (as the graph palette does) with Hlsl as its code at (X, Y); returns it, or null with OutError set. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static UEdGraphNode* AddCustomHlslNode(UEdGraph* Graph, const FString& Hlsl, int32 X, int32 Y, FString& OutError);

	/** Appends inputs and outputs (type keywords) to a custom HLSL node's Signature and rebuilds its pins; existing links survive.
	 * Returns errors, empty when it worked. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static FString AddCustomHlslPins(UEdGraphNode* Node, const TArray<FString>& InputNames, const TArray<FString>& InputTypes,
		const TArray<FString>& OutputNames, const TArray<FString>& OutputTypes);

	/** Sets a custom HLSL node's code (the CustomHlsl property, through its change event so the node refreshes and recompiles);
	 * returns errors, empty when it worked. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static FString SetCustomHlsl(UEdGraphNode* Node, const FString& Hlsl);

	/** Adds a parameter output (for example "Module.PerchMode" or "Emitter.WaveGrid") to a parameter-map get node, with its hidden
	 * default-value pin (none for data interfaces), and marks the graph changed so the next non-forced compile picks it up.
	 * Returns errors (empty when added; "EXISTS" when the node already has it). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static FString AddMapGetOutput(UEdGraphNode* MapGet, const FString& ParameterName, const FString& TypeName, const FString& DefaultValue);

	/** Adds a parameter input (for example "Particles.TakeOffTimer") to a parameter-map set node, so the node writes it, and marks the
	 * graph changed so the next non-forced compile picks it up. Returns errors (empty when added; "EXISTS" when the node already has it). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static FString AddMapSetInput(UEdGraphNode* MapSet, const FString& ParameterName, const FString& TypeName);

	/** Links FromNode.FromPin (an output) to ToNode.ToPin (an input), through the graph schema; returns errors, empty when
	 * linked (or already linked). An input pin that already has a link loses it (the Niagara schema breaks the other link). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static FString LinkPins(UEdGraphNode* FromNode, const FString& FromPin, UEdGraphNode* ToNode, const FString& ToPin);

	/** Refreshes a Niagara node from external changes, as the editor does when a called script is saved: a function call node picks up
	 * the inputs its function gained or lost, a custom HLSL node rebuilds its pins from its signature. Returns errors, empty when it ran. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static FString RefreshNiagaraNode(UEdGraphNode* Node);

	/** One line naming a node by what it is, not by its object name: its class, object name and title, then a function call's function
	 * name, called script, signature name, pins and specifiers (Attribute=Composite); a custom HLSL node's signature and, on the lines
	 * that follow, its code; an operator's op name; an input node's variable, type and usage. For finding the vendor nodes an edit
	 * hangs on (Tools/CorePatch/TRACKING.md, the inspect step). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static FString DescribeNiagaraNode(UEdGraphNode* Node);

	/** Links a module input to a parameter ("User.TrackVelocityRelativeToWater", "System.TransformMatrices2D.CameraFacing") the way the
	 * stack's link action does, with the input's own type. Unlike SetNiagaraInputLinked it walks the emitter graph itself, so Script may
	 * be "Stage:<name>" or "Stage:#N" and the module may sit in a simulation stage. An input that already has an override (a local value,
	 * a data interface or a link) is refused: the stack's remover is not exported, so clear it in the editor first. Returns errors,
	 * empty when linked; the system needs a compile afterwards (CompileNiagaraSystem). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Graph")
	static FString LinkNiagaraModuleInput(UNiagaraSystem* System, FName Emitter, FName Script, FName Module, FName Input, FName Parameter);

	/** Every user parameter of System, one per line: "User.Name<TAB>type<TAB>description", the description empty when it has none (it is
	 * the tooltip the Niagara editor and a Niagara component's User Parameters show). Only looks: nothing is created or dirtied. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Docs")
	static FString DescribeUserVariables(UNiagaraSystem* System);

	/** Sets the description (tooltip) of System's user parameter Name ("User.Wave.Damping" or "Wave.Damping"). Marks the system modified;
	 * save it afterwards. Returns errors, empty when set. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Docs")
	static FString SetUserVariableDescription(UNiagaraSystem* System, FName Name, const FString& Description);

	/** Puts System's user parameters Names ("User.Wave.Damping" or "Wave.Damping") into the category CategoryName of its user parameter
	 * hierarchy: what the Niagara editor's User Parameters hierarchy editor arranges, and the group a Niagara component's details panel
	 * (a NinjaLive's User Parameters) shows them under. The category is made at the hierarchy's root when it is missing and gets
	 * CategoryTooltip; with SectionName set, it is shown under that section (made when missing, with SectionTooltip), one of the
	 * section buttons over the User Parameters. Each parameter leaves wherever it sat before and joins the category in the order given;
	 * running it again changes nothing. Marks the system modified; save it afterwards. Returns errors, empty when done (nothing changes
	 * when a name is not a user parameter). */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Docs")
	static FString CategorizeUserParameters(UNiagaraSystem* System, FName SectionName, const FString& SectionTooltip, FName CategoryName,
		const FString& CategoryTooltip, const TArray<FName>& Names);

	/** System's user parameter hierarchy, one line per element: its sections, then each root element (a category, with the section it
	 * shows under, or a parameter) and, indented by a tab, the parameters in each category. Only looks. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Docs")
	static FString DescribeUserParameterHierarchy(UNiagaraSystem* System);

	/** Every member variable Blueprint declares, one per line: "Name<TAB>type<TAB>flags<TAB>tooltip<TAB>range"; flags "edit" when it is
	 * Instance Editable, "expose" when Expose on Spawn, "bp" when BlueprintReadOnly is off; range lists ClampMin, ClampMax, UIMin, UIMax
	 * where set. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Docs")
	static FString DescribeBlueprintVariables(UBlueprint* Blueprint);

	/** Sets one metadata key of a Blueprint member variable: "tooltip", "ClampMin", "ClampMax", "UIMin", "UIMax", "Units". An empty Value
	 * removes the key. Returns errors; compile and save the Blueprint afterwards. */
	UFUNCTION(BlueprintCallable, Category = "FNB Dev|Docs")
	static FString SetBlueprintVariableMetaData(UBlueprint* Blueprint, FName Variable, FName Key, const FString& Value);
};
