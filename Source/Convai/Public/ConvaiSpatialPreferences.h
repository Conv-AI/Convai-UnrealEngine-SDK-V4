// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ConvaiDefinitions.h"
#include "ConvaiSpatialPreferences.generated.h"

/**
 * Per-chatbot preferences for how much of the scene's spatial layout this
 * chatbot is told about, and whether it should react when that layout changes.
 *
 * The spatial system always computes everything once (centrally); these toggles
 * just decide what THIS chatbot receives. They split into two independent
 * categories:
 *
 *   • Surroundings — where things are relative to THIS chatbot ("the crate is
 *     close by, in front of you"; "the player is behind you, facing toward you").
 *     Covers objects, other characters, and the player.
 *
 *   • Relations — how other things sit relative to EACH OTHER ("the crate is on
 *     top of the pressure plate"; "the gun is to the right of the crate"). Does
 *     not involve this chatbot's own position.
 *
 * Each category also has its own "respond" setting: Never (default) feeds the
 * info silently; Auto/Always also nudges the AI to react when it changes — handy
 * if, say, you want the bot to comment when the player walks up to something.
 */
USTRUCT(BlueprintType)
struct CONVAI_API FConvaiSpatialAwarenessPreferences
{
	GENERATED_BODY()

	/**
	 * Tell this chatbot where objects, other characters, and the player are
	 * relative to itself. Turn OFF if this chatbot shouldn't be aware of its
	 * surroundings (e.g. a disembodied narrator).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surroundings")
	bool bReceiveSurroundings = true;

	/**
	 * How the chatbot reacts when its surroundings change. Never = update
	 * silently (default); Auto/Always = also prompt the AI to respond to the
	 * change (e.g. greet the player who just approached).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surroundings",
		meta = (EditCondition = "bReceiveSurroundings"))
	EC_RunLLMOption SurroundingsResponse = EC_RunLLMOption::Never;

	/**
	 * WHEN a surroundings change reaches the chatbot (only relevant when
	 * Surroundings Response is Auto/Always): Send Normally batches into the next
	 * scheduled send; Wait Until Conversation Is Idle holds it while anyone is
	 * talking so the character can't interrupt itself to comment on it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surroundings",
		meta = (EditCondition = "bReceiveSurroundings"))
	EConvaiContextDelivery SurroundingsDelivery = EConvaiContextDelivery::SendNormally;

	/**
	 * Tell this chatbot how nearby things relate to one another ("the box is on
	 * top of the plate"). Independent of Surroundings — you can have one without
	 * the other.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relations")
	bool bReceiveRelations = true;

	/**
	 * How the chatbot reacts when those relations change. Never = update silently
	 * (default); Auto/Always = also prompt the AI to respond to the change.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relations",
		meta = (EditCondition = "bReceiveRelations"))
	EC_RunLLMOption RelationsResponse = EC_RunLLMOption::Never;

	/**
	 * WHEN a relations change reaches the chatbot (only relevant when Relations
	 * Response is Auto/Always): Send Normally batches into the next scheduled
	 * send; Wait Until Conversation Is Idle holds it while anyone is talking so
	 * the character can't interrupt itself to comment on it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relations",
		meta = (EditCondition = "bReceiveRelations"))
	EConvaiContextDelivery RelationsDelivery = EConvaiContextDelivery::SendNormally;
};
