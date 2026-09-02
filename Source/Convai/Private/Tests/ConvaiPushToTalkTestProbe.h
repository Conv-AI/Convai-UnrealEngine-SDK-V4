// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#if WITH_TESTS

#include "CoreMinimal.h"
#include "ConvaiPlayerComponent.h"
#include "ConvaiPushToTalkTestProbe.generated.h"

/**
 * Stands in for BP_ConvaiPlayerComponent, which declares Enable Push To Talk as a Blueprint
 * variable. The native side finds that switch by name, so the test needs a subclass that
 * carries one the same way a generated Blueprint class does.
 */
UCLASS()
class UConvaiPushToTalkTestPlayerComponent : public UConvaiPlayerComponent
{
	GENERATED_BODY()

public:
	// Named exactly as the Blueprint variable is: no b prefix, because that is what the editor
	// made of "Enable Push To Talk" and it is what IsPushToTalkEnabled looks for.
	UPROPERTY()
	bool EnablePushToTalk = false;
};

#endif // WITH_TESTS
