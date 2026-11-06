// ConvaiLogger.h
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Containers/Queue.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"  
#include "HAL/PlatformFile.h"         
#include "HAL/ThreadSafeBool.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include <atomic>
#include "ConvaiLogger.generated.h"

// UE 5.3 defines UE_VALIDATE_FORMAT_STRING only where logging is compiled in,
// not under NO_LOGGING; 5.4 and later always define it.
#ifndef UE_VALIDATE_FORMAT_STRING
#define UE_VALIDATE_FORMAT_STRING(Format, ...)
#endif

// CONVAI_LOG writes a line to the engine log and to ConvaiLogs. The engine log
// takes what the category does not suppress; ConvaiLogs takes Log and above,
// plus whatever the category is raised to (-LogCmds="ConvaiSubsystemLog Verbose").
// Both are decided before formatting: a line neither takes never evaluates its
// arguments, and a taken line is formatted once. Under NO_LOGGING (Shipping)
// UE_LOG_ACTIVE is 0, so ConvaiLogs takes Log and above.
// The format is checked against its arguments at compile time as UE_LOG checks
// it: UE_LOG here only sees "%s", and before 5.6 FString::Printf does not check.
// do/while(0) makes it one statement: a brace-less `if (x) CONVAI_LOG(...)` that
// guarded only half of it crashed teardown on most launches for months (ISSUES I7).
#define CONVAI_LOG(Category, Verbosity, Format, ...) \
do \
{ \
	UE_VALIDATE_FORMAT_STRING(Format, ##__VA_ARGS__); \
	if ((::ELogVerbosity::Verbosity & ::ELogVerbosity::VerbosityMask) <= ::ELogVerbosity::Log || UE_LOG_ACTIVE(Category, Verbosity)) \
	{ \
		const FString ConvaiLogMacroLine = FString::Printf(Format, ##__VA_ARGS__); \
		UE_LOG(Category, Verbosity, TEXT("%s"), *ConvaiLogMacroLine); \
		FConvaiLogger::Get().Log(TEXT(#Category) TEXT(" : ") TEXT(#Verbosity) TEXT(" : ") + ConvaiLogMacroLine); \
	} \
} while (0) ;

// Runtime-category variant: use when the category is a FLogCategoryBase& parameter, not a symbol.
#define CONVAI_LOG_REF(CategoryRef, Verbosity, Format, ...) \
do \
{ \
	UE_VALIDATE_FORMAT_STRING(Format, ##__VA_ARGS__); \
	if ((::ELogVerbosity::Verbosity & ::ELogVerbosity::VerbosityMask) <= ::ELogVerbosity::Log || !(CategoryRef).IsSuppressed(::ELogVerbosity::Verbosity)) \
	{ \
		const FString ConvaiLogMacroLine = FString::Printf(Format, ##__VA_ARGS__); \
		UE_LOG_REF(CategoryRef, Verbosity, TEXT("%s"), *ConvaiLogMacroLine); \
		FConvaiLogger::Get().Log((CategoryRef).GetCategoryName().ToString() + TEXT(" : ") TEXT(#Verbosity) TEXT(" : ") + ConvaiLogMacroLine); \
	} \
} while (0) ;

/**
 * Asynchronous, file-based logger singleton: ConvaiLogs in the project's Saved
 * directory, one open file at a time.
 */
class CONVAI_API FConvaiLogger final : public FRunnable
{
public:
	static FConvaiLogger& Get();
	void Log(const FString& Message);

	// FRunnable interface
	virtual uint32 Run() override;
	virtual void Stop() override;
	FString GetLogFilePath();

private:
	struct FQueuedLine
	{
		FDateTime Time;
		FString Message;
	};

	FConvaiLogger();
	/** Writes to InFolder and rolls to a new file past InMaxFileBytes. With
	 *  bPruneOldFiles, start-up deletes all but the newest files; without
	 *  bStartThread, Log writes each line itself. */
	FConvaiLogger(const FString& InFolder, int64 InMaxFileBytes, bool bPruneOldFiles, bool bStartThread);
	virtual ~FConvaiLogger() override;

	static FString DefaultFolder();
	/** Stops the writer, then flushes and closes the file; Log takes no line after it. */
	void Close();
	void ShutdownThread();
	void PruneOldFiles() const;
	void WriteQueued(bool bFlushNow);
	void WriteLocked(const FString& Text);
	void FlushLocked(bool bFlushNow);
	static void AppendLine(FString& Out, const FDateTime& Time, const FString& Message);
	static FString CreateLogFilePath(const FString& ExtraSuffix = TEXT(""),
									 const FString& OverridePort = TEXT(""),
									 const FString& OverrideDir = TEXT(""));

	FRunnableThread* Thread;
	TQueue<FQueuedLine, EQueueMode::Mpsc> MessageQueue;
	FEvent* WakeEvent;
	FThreadSafeBool bStopping;
	/** Read by Log even after static destruction: Get() keeps returning the
	 *  destroyed instance, and a trivially destroyed atomic still reads true. */
	std::atomic<bool> bClosed{false};
	const FString Folder;
	const int64 MaxFileBytes;

	/** Guards the file, its path and size: the writer thread takes it, or every
	 *  logging thread when there is no writer thread. */
	FCriticalSection FileMutex;
	TUniquePtr<IFileHandle> File;
	FString LogFilePath;
	int64 FileBytes = 0;
	double NextFlushSeconds = 0.0;
	bool bUnflushed = false;
	bool bReportedOpenFailure = false;

#if WITH_TESTS
	friend struct FConvaiLoggerTestAccess;
	/** While a test has set a marker, the lines Log is handed that contain it:
	 *  other threads keep logging during a test, so a test marks its own lines. */
	FThreadSafeBool bTapOn;
	FCriticalSection TapMutex;
	FString TapMarker;
	TArray<FString> TapLines;
	int32 FileOpensForTests = 0;
#endif
};


// BP function lib for logging

UENUM(BlueprintType)
enum class EC_LogLevel : uint8
{
	Verbose  UMETA(DisplayName="Verbose"),
	Log      UMETA(DisplayName="Log"),
	Warning  UMETA(DisplayName="Warning"),
	Error    UMETA(DisplayName="Error"),
	Fatal    UMETA(DisplayName="Fatal")
};

UCLASS()
class CONVAI_API UConvaiBlueprintLogger : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
		/**
	 * Logs a message with context of which Blueprint object called this.
	 * @param WorldContextObject  – automatically filled by Blueprint
	 * @param Verbosity           – choose your verbosity level
	 * @param Message             – the text you want to log
	 */
	UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldContextObject", DisplayName="ConvaiLog"), Category="Convai|Logging")
	static void C_ConvaiLog(UObject* WorldContextObject, EC_LogLevel Verbosity, const FString& Message);
};
