/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * ConvaiVisionRenderTargetActions.h
 *
 * Asset type actions and factory for Convai Vision render targets.
 */

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"
#include "Factories/Factory.h"
#include "ConvaiVisionRenderTargetActions.generated.h"

/** Asset type actions for Convai Vision render targets. */
class FConvaiVisionRenderTargetActions : public FAssetTypeActions_Base
{
public:
	// IAssetTypeActions interface
	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass *GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	// End of IAssetTypeActions interface
};

/** Factory for creating TextureRenderTarget2D assets with Convai Vision default properties. */
UCLASS()
class CONVAIEDITOR_API UConvaiVisionRenderTargetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UConvaiVisionRenderTargetFactory();

	// UFactory interface
	virtual UObject *FactoryCreateNew(UClass *Class, UObject *InParent, FName Name, EObjectFlags Flags, UObject *Context, FFeedbackContext *Warn) override;
	// End of UFactory interface

	/** Default width for render targets */
	UPROPERTY(EditAnywhere, Category = "Render Target")
	int32 DefaultSizeX = 512;

	/** Default height for render targets */
	UPROPERTY(EditAnywhere, Category = "Render Target")
	int32 DefaultSizeY = 512;

	/** Default clear color for render targets */
	UPROPERTY(EditAnywhere, Category = "Render Target")
	FLinearColor DefaultClearColor = FLinearColor::Black;
};
