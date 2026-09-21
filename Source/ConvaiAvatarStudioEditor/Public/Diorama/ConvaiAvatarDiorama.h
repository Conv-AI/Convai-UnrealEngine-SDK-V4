// Copyright Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/ConvaiAvatarUploadDefaults.h"
#include "UObject/SoftObjectPath.h"

class FJsonObject;
class UWorld;

/** One actor of the diorama, as an issue names it and a click finds it. */
struct FConvaiAvatarDioramaActor
{
    /** The label the creator sees in the Outliner. */
    FString Label;

    FSoftObjectPath Path;

    FGuid Guid;
};

/**
 * What a scan read off the source level, with the avatar left out of every fact but TexturesCount:
 * the studio spawns its own, so the diorama is judged without it.
 *
 * Filled by the half that reads the world, judged by Evaluate, and written down by
 * MakeRecord - which takes five things from it and nothing else. AvatarActorNames and
 * CameraActorName are handles for the copy step, never written anywhere.
 */
struct FConvaiAvatarDioramaFacts
{
    /** Long package name of the level scanned. */
    FString SourceLevel;

    bool bPartitioned = false;

    int32 Sublevels = 0;

    /** The map, its built data or any external actor package has unsaved changes. */
    bool bDirty = false;

    /** The Entry Point as the creator names it, e.g. "BP_Receptionist". */
    FString AvatarName;

    TArray<FName> AvatarActorNames;

    /** Where the placed avatar stands, in level space. Meaningful only when exactly one is placed. */
    FVector AvatarLocation = FVector::ZeroVector;

    FRotator AvatarRotation = FRotator::ZeroRotator;

    FVector AvatarScale = FVector::OneVector;

    /** Placed diorama actors after the skips: never the avatar, nor a child actor of anything. */
    int32 Actors = 0;

    /** Movable and Stationary lights. Static ones are apart because they do not render in the studio. */
    TArray<FConvaiAvatarDioramaActor> DynamicLights;

    TArray<FConvaiAvatarDioramaActor> StaticLights;

    TArray<FConvaiAvatarDioramaActor> ShadowCastingLights;

    bool bSkyLight = false;

    bool bSkyAtmosphere = false;

    bool bHeightFog = false;

    bool bVolumetricCloud = false;

    int32 StaticMeshComponents = 0;

    int32 StaticMeshInstances = 0;

    /** LOD0, summed. */
    int32 Triangles = 0;

    /** Every texture the diorama's materials use, plus the avatar's - the one fact the avatar counts in. */
    int32 TexturesCount = 0;

    /**
     * The largest built edge in pixels among the DIORAMA's textures, and which texture it is.
     *
     * The avatar's are left out of this and of TexturesMemoryMb, unlike TexturesCount: an avatar
     * publishes with whatever textures it has when no diorama is involved, so measuring them against
     * a diorama limit would refuse a creator over something ticking the box does not change.
     */
    int32 TexturesMaxBuiltPx = 0;

    FSoftObjectPath LargestTexture;

    /** Where the largest texture is used, e.g. "M_Desk on SM_Desk". Empty when unknown. */
    FString LargestTextureContext;

    double TexturesMemoryMb = 0.0;

    TArray<FConvaiAvatarDioramaActor> Cameras;

    /** The camera the studio is handed: the only one, or the one tagged Convai.Diorama.Camera. None otherwise. */
    FName CameraActorName;

    TArray<FConvaiAvatarDioramaActor> AutoActivatingCameras;

    TArray<FConvaiAvatarDioramaActor> AspectConstrainedCameras;

    bool bNavMeshBounds = false;

    int32 ConvaiObjects = 0;

    /** Diorama actors holding a hard reference to the placed avatar. */
    TArray<FConvaiAvatarDioramaActor> AvatarReferencers;

    /** Diorama actors attached to the avatar in the Outliner. */
    TArray<FConvaiAvatarDioramaActor> AttachedToAvatar;

    /** What the studio will not use, by label: player starts, foreign pawns, a game mode. */
    TArray<FString> Unsupported;

    bool bHasLevelBlueprint = false;

    /** The record's has_custom_lighting. Static lights alone do not count: they never render there. */
    bool HasCustomLighting() const;

    /** The record's has_custom_camera. */
    bool HasCustomCamera() const;
};

/** One line of the scan's issue list. */
struct FConvaiAvatarDioramaIssue
{
    EConvaiAvatarDioramaSeverity Severity = EConvaiAvatarDioramaSeverity::Info;

    /** The asset or actor a click selects. Empty when there is no one thing to point at. */
    FSoftObjectPath Target;

    /** Set when Target is an actor, so the click can select it. */
    FGuid ActorGuid;

    /** The whole line the creator reads. */
    FString Reason;

    /** The value measured and the limit it broke. Both zero for a rule that has no number. */
    double Measured = 0.0;

    double Limit = 0.0;
};

/** A scan result or the reason the scan could not run. */
struct FConvaiAvatarDioramaReport
{
    /** Why the scan did not run, phrased for the creator. Empty when it did. */
    FString Refusal;

    /** False when the Policy has not been read, which is when Evaluate is not run at all. */
    bool bLimitsRead = false;

    FConvaiAvatarDioramaFacts Facts;

    TArray<FConvaiAvatarDioramaIssue> Issues;

    int32 Count(EConvaiAvatarDioramaSeverity Severity) const;
};

struct FConvaiAvatarDioramaRecord
{
    int32 Schema = 1;
    FString Level;
    FTransform AvatarTransform = FTransform::Identity;
    bool bHasCustomLighting = false;
    bool bHasCustomCamera = false;
};

/**
 * The diorama a level becomes when it is published with an Avatar.
 *
 * Everything that DECIDES here is pure and is handed the facts, so the deciding is testable without
 * a level, a policy read or a project on disk. Inspect is the one function that reads the live
 * world, and it only reads: it never judges, and never dirties the level it measures.
 */
namespace ConvaiAvatarDiorama
{
/**
 * Reads the facts off the open level - the persistent level only, since that is all the studio
 * streams. AvatarClass may be null, which reads as no avatar placed and is Evaluate's to say.
 *
 * Compilation of the meshes and textures it touches is finished first, so the numbers are the built
 * ones and not a placeholder's - and only of those it touches, because this runs on a tick.
 */
FConvaiAvatarDioramaFacts Inspect(UWorld* World, UClass* AvatarClass);

/**
 * Judges the facts against the limits.
 *
 * One issue per limit the policy names, at the severity it names; a limit it does not name is not
 * checked. The rules the policy cannot express are fixed here with their severities. Nothing here
 * tells the creator the avatar is taken out: the studio spawns it where it was found, and that is
 * what the Info line says.
 */
TArray<FConvaiAvatarDioramaIssue> Evaluate(const FConvaiAvatarDioramaFacts& Facts, const FConvaiAvatarDioramaLimits& Limits);

/** The five-key wire object. Pitch and roll are zero so the studio keeps the level upright. */
TSharedRef<FJsonObject> MakeRecord(const FConvaiAvatarDioramaFacts& Facts, const FString& LevelPackage);

/** Persist an existing record without reapplying the scan's upright projection. */
TSharedRef<FJsonObject> MakeRecord(const FConvaiAvatarDioramaRecord& Record);

/** Invalid records leave OutRecord untouched. */
bool ReadRecord(const TSharedPtr<FJsonObject>& Json, FConvaiAvatarDioramaRecord& OutRecord);

/** An absent local record removes any stage echoed by the service. */
void ApplyRecordTo(TSharedRef<FJsonObject> Metadata, const TOptional<FConvaiAvatarDioramaRecord>& Record);

FText StatusLine(const FConvaiAvatarDioramaReport& Report);

/** Only evaluated errors count; refusals and missing limits have their own status. */
bool HasErrors(const FConvaiAvatarDioramaReport& Report);
}
