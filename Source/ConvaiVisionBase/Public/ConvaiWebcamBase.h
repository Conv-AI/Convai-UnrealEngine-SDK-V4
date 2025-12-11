// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "VisionInterface.h"
#include "ConvaiWebcamBase.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiWebcamLog, Log, All);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnFrameReady);

UCLASS(Abstract)
class CONVAIVISIONBASE_API UConvaiWebcamBase : public USceneComponent, public IConvaiVisionInterface
{
	GENERATED_BODY()

public:	
	UConvaiWebcamBase(){}
	UConvaiWebcamBase(const FObjectInitializer& ObjectInitializer);
	
protected:
	
	//~ Begin ActorComponent Interface
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	//~ Begin ActorComponent Interface

public:
	// VisionInterface functions start
	/**
	 * Starts the vision component.
	 * This will transition the state from Stopped or Paused to Capturing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Vision")
	virtual void Start();
	
	/**
	 * Stops the vision component.
	 * This will transition the state from Capturing or Paused to Stopped.
	 */
	UFUNCTION(BlueprintCallable, Category = "Convai|Vision")
	virtual void Stop();

	UFUNCTION(BlueprintCallable, Category = "Convai|Vision")
	virtual UTexture* GetImageTexture(ETextureSourceType& TextureSourceType) override;
	
	UFUNCTION(BlueprintCallable, Category = "Convai|Vision")
	virtual void SetMaxFPS(int MaxFPS) override;

	UFUNCTION(BlueprintCallable,BlueprintPure, Category = "Convai|Vision")
	virtual int GetMaxFPS() const override;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Vision")
	virtual FString GetLastErrorMessage() const override;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Vision")
	virtual int GetLastErrorCode() const override;

	virtual void SetState(EVisionState NewState);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Convai|Vision")
	virtual EVisionState GetState() const override;

	virtual bool IsCompressedDataAvailable() const override;
	virtual bool GetCompressedData(int& width, int& height, TArray<uint8>& data) override;
	virtual bool CaptureCompressed(int& width, int& height, TArray<uint8>& data, float ForceCompressionRatio) override;
	virtual bool CaptureRaw(int& width, int& height, TArray<uint8>& data) override;

	// VisionInterface functions END

	//-----------Events-----------
	UPROPERTY(BlueprintAssignable, Category = "Convai|Vision")
	FOnFrameReady OnFrameReady;
	//-----------END Events----------- 


	//-----------Variables-----------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Vision")
	FString Identifier;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Convai|Vision", meta = (DisplayName = "Maximum FPS", BlueprintSetter = "SetMaxFPS"))
	int m_MaxFPS;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, category = "Convai|Vision")
	bool bUpdateOnFetch;
	//-----------END Variables-----------

protected:
	FTimerHandle FrameStoppedTimerHandle;
	bool bFirstFrame;	

	virtual void SetErrorCodeAndMessage(int ErrorCode, const FString& ErrorMessage, bool bPrintToLog = true);
	virtual void ProcessFrameStopped();
	virtual void CleanTimers();
	virtual void CleanDelegates();
	virtual bool CanStart();
	virtual bool CanStop();

private:
	EVisionState CurrentState;

	FString LastErrorMessage;
	int LastErrorCode;
};

