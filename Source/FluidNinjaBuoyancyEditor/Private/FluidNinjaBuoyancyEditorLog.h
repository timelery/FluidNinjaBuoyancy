#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Internationalization/Text.h"
#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFluidNinjaBuoyancyEditor, Log, All);

/** What the editor tells the user after applying a preset, from the context menu or the component's Preset panel alike. */
namespace FluidNinjaBuoyancyEditorReport
{
	/** One object the preset was applied to: its name for the user, whether the library succeeded, and the library's report. */
	struct FResult
	{
		FString Target;
		bool bApplied = false;
		FString Report;
	};

	/**
	 * Logs every result and shows one notification: "Applied <preset> to N <what>" when every object took it, otherwise the failures'
	 * reports one per line, as a warning. What is the noun for the objects: "actor(s)", "component(s)".
	 */
	void Show(const FString& PresetName, const FText& What, const TArray<FResult>& Results);
}
