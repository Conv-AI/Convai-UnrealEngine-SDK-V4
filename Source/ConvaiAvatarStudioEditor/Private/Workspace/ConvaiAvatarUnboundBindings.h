// Copyright Convai Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

/** A cloud avatar created from a local draft that could not be bound to it before its binding receipt was written. */
struct FConvaiAvatarUnboundBinding
{
	FString DraftId;
	FString CloudId;
	FString PluginName;
	/** The controller's AuthFingerprint when the cloud avatar was created: a hash of the API key or sign-in token Convai used,
	 * saved as "account". Only a lookup made with that credential may prove the avatar gone, since another account's answers
	 * not-found for an avatar that exists. Empty in a pair saved before it was recorded. */
	FString Account;
};

/** One file per draft under Saved/ConvaiAvatarStudio/Assets/Unbound, so the pair outlives an editor restart. */
namespace ConvaiAvatarWorkspacePrivate
{
	bool SaveUnboundBinding(const FConvaiAvatarUnboundBinding& Binding, FString& OutError);
	/** Every pair that reads back whole. Any other .json file there is skipped and named in OutUnreadable. A pair file is named
	 * after its draft (UnboundBindingFileName), so an unreadable one still says which draft must not be offered to resume. A
	 * folder redirected out of the project is not read at all and sets bOutFolderUnreadable: then any draft may have a pair. */
	TArray<FConvaiAvatarUnboundBinding> LoadUnboundBindings(TArray<FString>* OutUnreadable = nullptr, bool* bOutFolderUnreadable = nullptr);
	FString UnboundBindingsDirectory();
	FString UnboundBindingFileName(const FString& DraftId);
	FString UnboundBindingFile(const FString& DraftId);
	void RemoveUnboundBinding(const FString& DraftId);
#if WITH_DEV_AUTOMATION_TESTS
	/** Test seam: while set, the folder every pair is read from, saved to and deleted from, in place of the project's own. */
	extern FString UnboundBindingsDirectoryOverride;
#endif
}
