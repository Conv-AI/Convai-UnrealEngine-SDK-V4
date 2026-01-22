/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiProjectInfoCollector.h
 *
 * Collects project and plugin information.
 */

#pragma once

#include "CoreMinimal.h"
#include "IConvaiInfoCollector.h"

/**
 * Collects engine, project, and plugin metadata.
 */
class FConvaiProjectInfoCollector : public IConvaiInfoCollector
{
public:
	FConvaiProjectInfoCollector() = default;
	virtual ~FConvaiProjectInfoCollector() = default;

	// IConvaiInfoCollector interface
	virtual TSharedPtr<FJsonObject> CollectInfo() const override;
	virtual FString GetCollectorName() const override { return TEXT("ProjectInfo"); }
	virtual bool IsAvailable() const override { return true; }

private:
	/** Collect Unreal Engine information */
	TSharedPtr<FJsonObject> CollectEngineInfo() const;

	/** Collect Project information */
	TSharedPtr<FJsonObject> CollectProjectInfo() const;

	/** Collect Convai Plugin information */
	TSharedPtr<FJsonObject> CollectConvaiPluginInfo() const;

	/** Collect list of installed plugins */
	TArray<TSharedPtr<FJsonValue>> CollectInstalledPlugins() const;

	/** Collect relevant project settings */
	TSharedPtr<FJsonObject> CollectProjectSettings() const;

	/** Get Build Configuration (Debug, Development, Shipping, etc.) */
	FString GetBuildConfiguration() const;
};
