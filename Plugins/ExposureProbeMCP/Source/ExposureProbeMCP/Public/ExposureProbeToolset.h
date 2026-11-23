#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ExposureProbeToolset.generated.h"

/**
 * Exposure Probe audit tools for remote verification over the Toolset Registry (MCP).
 * Every tool returns a JSON string. Positions are UE world coordinates in centimetres.
 *
 * Typical flow: SpawnProbes -> FullRecompute -> edit the level -> UpdateNow -> GetRays / GetLastReport.
 */
UCLASS(BlueprintType)
class EXPOSUREPROBEMCP_API UExposureProbeToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("0.3.0"); }

	/** Place probes. ProbesJson is an array of {"name": "T0", "team": "T" or "CT", "x": .., "y": .., "z": .., "pose": "auto" | "stand" | "crouch"}.
	 * Probes with the same name are replaced. Returns {"spawned": [...]} . */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString SpawnProbes(const FString& ProbesJson);

	/** Delete every Exposure Probe actor in the editor level. Returns {"deleted": N}. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString ClearProbes();

	/** Cast every ray of the current probe set and return the report. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString FullRecompute(bool bParallel = true);

	/** Diff actor geometry against the last update and recast only the rays crossing what changed. Returns the report. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString UpdateNow();

	/** Last report: summary, changed pairs, evidence rays, timings, all pairs. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString GetLastReport();

	/** Every ray of the current table: {"rays": [{"key": "T0->CT3/5", "visible": true, "blocker": "", "start": [..], "end": [..]}]}.
	 * Validity rays (embedded-point mask) have keys starting with "valid:". */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString GetRays(bool bIncludeSegments = false);

	/** Probe feet, pose and head clearance as used by the solver. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString GetProbes();

	/** Mark a probe pair such as "T0-CT3" as design intent; saved on the T probe with the level. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString MarkDesignIntent(const FString& Edge, const FString& Note = TEXT(""));

	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString ClearDesignIntent(const FString& Edge);

	/** Viewport drawing: Mode is "off", "changed" or "all"; FocusEdge limits it to one pair.
	 * Style is "body" (one link per pair, seen body regions filled) or "rays" (every ray from the eye to the sample points); empty keeps the current style. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString SetDrawMode(const FString& Mode, const FString& FocusEdge = TEXT(""), const FString& Style = TEXT(""));

	/** Turn automatic updates after edits on or off. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString SetAutoUpdate(bool bEnabled);

	/** Plugin settings as JSON (property name -> exported text). */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString GetSettings();

	/** Set plugin settings from {"PropertyName": "value as exported text", ...}, e.g. {"UnitPreset": "Unreal", "SampleBearings": "8"}.
	 * Applied in order; UnitPreset fills the body dimensions of the preset. Not saved to the config file. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString SetSettings(const FString& SettingsJson);

	/** Viewport layers, any combination: Layers is a comma list of "probes", "heatmap", "oneway" (empty turns all off);
	 * Show is "a" or "delta" for the heatmap. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString SetView(const FString& Layers, const FString& Show = TEXT(""));

	/** One-way floor cells: count, the strongest spots, and with probes selected the cells that peek them ("threat") or that they peek ("advantage"). */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString GetOneWay(int32 MaxSpots = 20);

	/** Isolate one legend row of the floor layers: "None", "SeenMore", "SeesMore", "Peek1", "Peek2", "Peek4", "Peek8", "NewSpot", "PeekedBy", "Threat", "Advantage".
	 * Returns the cell count of every row and the strongest cells of the chosen one. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString SetFloorFilter(const FString& Filter);

	/** Write the regression report against the active baseline (or the last edit) as Markdown and JSON; returns the paths and the JSON. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString ExportReport();

	/** Sample the floor and compute the heatmap. Returns sample count, spacing, rays, seconds, A range and the strongest spots. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString ComputeHeatmap();

	/** Heatmap samples: {"samples": [{"x","y","z","a","vout","vin","crouch","delta"}], ...}. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString GetHeatmap(bool bIncludeSamples = false);

	/** Save the current pairs (and an up-to-date heatmap) as a named baseline of this level. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString SaveBaseline(const FString& Name);

	/** Compare against a saved baseline (empty name stops). Returns changed pairs, new and resolved one-way pairs. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString SetBaseline(const FString& Name);

	/** Saved baselines of this level, newest first. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString ListBaselines();

	/** Smokes for a probe (empty name: the first selected): CC, the landing points, RT and the detour. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString ComputeSmoke(const FString& Observer = TEXT(""));

	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString ClearSmoke();

	/** The smoke plan on show: observer, CC, RT, whether an edit made it out of date, and the summary. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString GetSmoke();

	/** Place smokes. SmokesJson is an array of {"x", "y", "z"} (cm), optional "size": "shared" | "cs2" | "brimstone" | "omen" | "astra" | "custom"
	 * and "radius", "lift", "duration" (map units, seconds) for custom. Each snaps to the nearest floor cell. Returns their names and landing points. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString PlaceSmokes(const FString& SmokesJson);

	/** Placed smokes: name, landing point, centre, radius (map units), duration, size choice. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString GetPlacedSmokes();

	/** Move a placed smoke; it snaps to the nearest floor cell as after a drag. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString MoveSmoke(const FString& Name, float X, float Y, float Z);

	/** Delete every placed smoke. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString ClearPlacedSmokes();

	/** The placed smokes against a probe's sightlines (empty name: the first selected): cut share, each smoke's share, RT. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString EvaluateSmokes(const FString& Observer = TEXT(""));

	/** Turn the plan on show into placed smokes, then evaluate them. */
	UFUNCTION(meta = (AICallable), Category = "ExposureProbe")
	static FString PlacePlanSmokes();

};
