
#include "RestAPI/ConvaiAPIBase.h"
#include "ConvaiUtils.h"
#include "../Convai.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "String/LexFromString.h"

DEFINE_LOG_CATEGORY(ConvaiBaseHttpLogs);

namespace ConvaiMultipart
{
	void AppendUtf8(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Text)
	{
		FTCHARToUTF8 Converted(*Text);
		DataToSend.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	void AppendFormField(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary, const FString& Name, const FString& Value)
	{
		AppendUtf8(DataToSend, FString::Printf(
			TEXT("\r\n------%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s"),
			*Boundary, *Name, *Value));
	}

	void AppendFileField(
		CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend,
		const FString& Boundary,
		const FString& Name,
		const FString& FileName,
		const TArray<uint8>& FileBytes,
		const FString& ContentType)
	{
		AppendUtf8(DataToSend, FString::Printf(
			TEXT("\r\n------%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: %s\r\n\r\n"),
			*Boundary, *Name, *FileName, *ContentType));
		DataToSend.Append(FileBytes.GetData(), FileBytes.Num());
	}
}


void UConvaiAPIBaseProxy::Activate()
{
    StartAPIRequest();
}

void UConvaiAPIBaseProxy::SetAPIRequestWorldContext(UObject* WorldContextObject)
{
    if (bAPIRequestContextCaptured) return;
    if (WorldContextObject && GEngine)
    {
        APIRequestWorld = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
    }
}

void UConvaiAPIBaseProxy::CaptureAPIRequestContext(UObject* WorldContextObject)
{
    if (bAPIRequestContextCaptured) return;
    SetAPIRequestWorldContext(WorldContextObject);
    bAPIRequestContextCaptured = true;
    // Track the experience/session token independently: a project API key can
    // remain installed while the session changes. Never log either credential.
    APIRequestSessionSnapshot = UConvaiUtils::GetAuthToken();
    APIRequestAuthSnapshot = UConvaiUtils::GetAuthHeaderAndKey();
    if (APIRequestWorld.IsValid())
    {
        APIWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::HandleAPIWorldCleanup);
    }
}

bool UConvaiAPIBaseProxy::HasAPIRequestContextChanged() const
{
    return bAPIRequestContextCaptured && (APIRequestSessionSnapshot != UConvaiUtils::GetAuthToken()
        || APIRequestAuthSnapshot != UConvaiUtils::GetAuthHeaderAndKey());
}

double UConvaiAPIBaseProxy::GetAPINowSeconds() const
{
#if WITH_DEV_AUTOMATION_TESTS
    if (TestNow) return TestNow();
#endif
    return FPlatformTime::Seconds();
}

void UConvaiAPIBaseProxy::BeginAPIRequestLifecycle(UObject* WorldContextObject)
{
    if (bAPIRequestStarted || bAPIRequestFinished || bAPIRequestCancelled) return;
    bAPIRequestStarted = true;
    AddToRoot(); // Before ProcessRequest, including synchronous/failed submission.
    CaptureAPIRequestContext(WorldContextObject);
    APIRetriesRemaining = FMath::Clamp(GetMaxAPIRetries(), 0, 10);
    const double DeadlineSeconds = GetAPIRequestTotalDeadlineSeconds();
    const UWorld* RequestWorld = APIRequestWorld.Get();
    const bool bWorldEnded = APIRequestWorld.IsStale()
        || (RequestWorld && (!RequestWorld->IsInitialized()
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
            || RequestWorld->IsBeingCleanedUp()
#endif
            || RequestWorld->bIsTearingDown));
    if (!FMath::IsFinite(DeadlineSeconds) || DeadlineSeconds <= 0.0 || HasAPIRequestContextChanged() || bWorldEnded)
    {
        CancelAPIRequest();
        return;
    }
    APIRequestDeadline = GetAPINowSeconds() + DeadlineSeconds;
    APIRequestTicker = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &ThisClass::TickAPIRequest), 0.05f);
}

void UConvaiAPIBaseProxy::StartAPIRequest(UObject* WorldContextObject)
{
    BeginAPIRequestLifecycle(WorldContextObject);
    if (bAPIFirstAttemptStarted || bAPIRequestFinished || bAPIRequestCancelled) return;
    bAPIFirstAttemptStarted = true;
    StartAPIRequestAttempt();
}

void UConvaiAPIBaseProxy::StartAPIRequestAttempt()
{
    if (bAPIRequestFinished || bAPIRequestCancelled || ActiveAPIRequest.IsValid()) return;
    if (HasAPIRequestContextChanged() || GetAPINowSeconds() >= APIRequestDeadline)
    {
        CancelAPIRequest();
        return;
    }
    bAPIRetryPending = false;
    ResponseCode = 0;
    FailureResponseBody.Empty();
    ResponseString.Empty();
    ResponseData.Reset();
    bAPIRequestConfigurationFailed = false;
    if (!PrepareAPIRequestAttempt())
    {
        CompleteAPIRequest(false);
        return;
    }
#if WITH_DEV_AUTOMATION_TESTS
    TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request = TestCreateRequest
        ? TestCreateRequest() : CONVAI_HTTP_MODULE::Get().CreateRequest();
    const bool bConfigured = TestConfigureRequest
        ? TestConfigureRequest(Request) : ConfigureRequest(Request, ConvaiHttpConstants::GET);
#else
    TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request = CONVAI_HTTP_MODULE::Get().CreateRequest();
    const bool bConfigured = ConfigureRequest(Request, ConvaiHttpConstants::GET);
#endif
    if (!bConfigured || bAPIRequestConfigurationFailed)
    {
        OnAPIRequestAttemptFinished();
        CompleteAPIRequest(false); // Invalid input/auth/local setup is not a network retry.
        return;
    }
    APIRequestVerb = Request->GetVerb();
    const double RemainingSeconds = APIRequestDeadline - GetAPINowSeconds();
    if (RemainingSeconds <= 0.0 || HasAPIRequestContextChanged())
    {
        OnAPIRequestAttemptFinished();
        CancelAPIRequest();
        return;
    }
    Request->SetTimeout(static_cast<float>(RemainingSeconds));
    ActiveAPIRequest = Request;
    Request->OnProcessRequestComplete().BindUObject(this, &ThisClass::OnHttpRequestComplete);
#if WITH_DEV_AUTOMATION_TESTS
    const bool bSubmitted = TestProcessRequest ? TestProcessRequest(Request) : Request->ProcessRequest();
#else
    const bool bSubmitted = Request->ProcessRequest();
#endif
    if (!bSubmitted && ActiveAPIRequest == Request)
    {
        // Some transports refuse submission without issuing a completion callback.
        // Treat the reported transport status once; a later callback is stale.
        OnHttpRequestComplete(Request, Request->GetResponse(), false);
    }
}

bool UConvaiAPIBaseProxy::IsAPIRequestRetrySafe(const FString& Verb) const
{
#if WITH_DEV_AUTOMATION_TESTS
    if (TestRetrySafety) return TestRetrySafety(Verb);
#endif
    return Verb.Equals(TEXT("GET"), ESearchCase::IgnoreCase)
        || Verb.Equals(TEXT("HEAD"), ESearchCase::IgnoreCase);
}

int32 UConvaiAPIBaseProxy::GetMaxAPIRetries() const
{
    return Convai::Get().GetConvaiSettings()->MaxAPIRetries;
}

bool UConvaiAPIBaseProxy::ShouldRetryAPIRequest(CONVAI_HTTP_REQUEST_PTR Request,
    CONVAI_HTTP_RESPONSE_PTR Response, bool bWasSuccessful) const
{
    if (bAPIRequestCancelled || bAPIRequestFinished || !Request.IsValid()
        || !IsAPIRequestRetrySafe(Request->GetVerb())) return false;
#if !USE_CONVAI_HTTP && (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
    if (!bWasSuccessful && Request->GetFailureReason() == EHttpFailureReason::Cancelled) return false;
#endif
    if (Response.IsValid())
    {
        const int32 Code = Response->GetResponseCode();
        // Ordinary auth/not-found/validation responses never enter this list.
        if (Code == 408 || Code == 429 || Code == 500 || Code == 502 || Code == 503 || Code == 504) return true;
        if (Code != 0 && (Code < 200 || Code >= 300 || bWasSuccessful)) return false;
    }
    if (bWasSuccessful) return false;
#if USE_CONVAI_HTTP || ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 4)
    const auto Status = Request->GetStatus();
#if USE_CONVAI_HTTP
    return Status == EConvaihttpRequestStatus::Failed_ConnectionError || Status == EConvaihttpRequestStatus::Failed;
#else
    return Status == EHttpRequestStatus::Failed_ConnectionError || Status == EHttpRequestStatus::Failed;
#endif
#else
    const EHttpFailureReason Reason = Request->GetFailureReason();
    return Reason == EHttpFailureReason::ConnectionError || Reason == EHttpFailureReason::TimedOut;
#endif
}

double UConvaiAPIBaseProxy::GetRetryAfterSeconds(const FString& Header, const FDateTime& Now)
{
    // Same delta-seconds / HTTP-date forms supported by ConvaiHTTP's retry helper.
    // Parse once here so both ordinary HTTP and ConvaiHTTP use the same proxy lifecycle.
    const FString Value = Header.TrimStartAndEnd();
    if (Value.IsEmpty()) return 0.0;
    double Seconds = 0.0;
    if (Value.IsNumeric() && LexTryParseString(Seconds, *Value) && FMath::IsFinite(Seconds)) return FMath::Max(0.0, Seconds);
    FDateTime RetryAt;
    return FDateTime::ParseHttpDate(Value, RetryAt) ? FMath::Max(0.0, (RetryAt - Now).GetTotalSeconds()) : 0.0;
}

bool UConvaiAPIBaseProxy::RequestAPIRequestRetry(double MinimumDelaySeconds)
{
    if (bAPIRequestFinished || bAPIRequestCancelled || ActiveAPIRequest.IsValid()
        || !IsAPIRequestRetrySafe(APIRequestVerb) || APIRetriesRemaining <= 0) return false;
    if (HasAPIRequestContextChanged())
    {
        CancelAPIRequest();
        return false;
    }
    const double Backoff = FMath::Min(8.0, 0.5 * FMath::Pow(2.0, static_cast<double>(APIRetriesUsed)));
    const double Delay = FMath::Max(Backoff, FMath::Max(0.0, MinimumDelaySeconds));
    const double Now = GetAPINowSeconds();
    if (!FMath::IsFinite(Delay) || Now + Delay >= APIRequestDeadline) return false;
    --APIRetriesRemaining;
    ++APIRetriesUsed;
    NextAPIAttemptAt = Now + Delay;
    bAPIRetryPending = true;
    // URL/query/auth and server bodies are intentionally not logged.
    CONVAI_LOG(ConvaiBaseHttpLogs, Log, TEXT("Retrying safe API read: additional attempt %d after %.2fs"), APIRetriesUsed, Delay);
    return true;
}

void UConvaiAPIBaseProxy::OnHttpRequestComplete(CONVAI_HTTP_REQUEST_PTR Request,
    CONVAI_HTTP_RESPONSE_PTR Response, bool bWasSuccessful)
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UConvaiAPIBaseProxy> WeakThis(this);
        AsyncTask(ENamedThreads::GameThread, [WeakThis, Request, Response, bWasSuccessful]()
        {
            if (WeakThis.IsValid()) WeakThis->OnHttpRequestComplete(Request, Response, bWasSuccessful);
        });
        return;
    }
    if (bAPIRequestFinished || !Request.IsValid() || Request != ActiveAPIRequest) return; // Duplicate/late completion.
    const bool bRetryable = ShouldRetryAPIRequest(Request, Response, bWasSuccessful);
    const double RetryAfter = Response.IsValid() ? GetRetryAfterSeconds(Response->GetHeader(TEXT("Retry-After")), FDateTime::UtcNow()) : 0.0;
    ActiveAPIRequest.Reset();
    Request->OnProcessRequestComplete().Unbind();
    OnAPIRequestAttemptFinished(); // No HTTP worker can write to that attempt's stream now.
    if (HasAPIRequestContextChanged() || GetAPINowSeconds() >= APIRequestDeadline) bAPIRequestCancelled = true;
    if (bAPIRequestCancelled)
    {
        OnAPIRequestCancellationReady();
        return;
    }
    if (bRetryable && RequestAPIRequestRetry(RetryAfter)) return;
    HandleAPIRequestResponse(Request, Response, bWasSuccessful);
}

bool UConvaiAPIBaseProxy::TickAPIRequest(float DeltaSeconds)
{
    if (bAPIRequestFinished || bAPIRequestCancelled) return false;
    if (GIsRequestingExit || HasAPIRequestContextChanged()
        || (APIRequestWorld.IsStale()) || GetAPINowSeconds() >= APIRequestDeadline)
    {
        CancelAPIRequest();
        return false;
    }
    if (bAPIRetryPending && GetAPINowSeconds() >= NextAPIAttemptAt) StartAPIRequestAttempt();
    return !bAPIRequestFinished && !bAPIRequestCancelled;
}

void UConvaiAPIBaseProxy::StopAPIRequestTicker()
{
    if (APIRequestTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(APIRequestTicker);
        APIRequestTicker.Reset();
    }
}

void UConvaiAPIBaseProxy::HandleAPIWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
    if (World == APIRequestWorld.Get()) CancelAPIRequest();
}

void UConvaiAPIBaseProxy::CancelAPIRequest()
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UConvaiAPIBaseProxy> WeakThis(this);
        AsyncTask(ENamedThreads::GameThread, [WeakThis]() { if (WeakThis.IsValid()) WeakThis->CancelAPIRequest(); });
        return;
    }
    if (bAPIRequestFinished || bAPIRequestCancelled) return;
    bAPIRequestCancelled = true;
    bAPIRetryPending = false;
    StopAPIRequestTicker();
    if (ActiveAPIRequest.IsValid())
    {
        // Do not close a response stream while an HTTP worker still owns it.
        ActiveAPIRequest->CancelRequest();
    }
    else
    {
        OnAPIRequestCancellationReady();
    }
}

void UConvaiAPIBaseProxy::OnAPIRequestCancellationReady()
{
    CompleteAPIRequest(false);
}

void UConvaiAPIBaseProxy::CompleteAPIRequest(bool bSuccess)
{
    if (bAPIRequestFinished) return;
    if (ActiveAPIRequest.IsValid())
    {
        // A child cannot finalize while its response writer is still owned by the worker.
        CancelAPIRequest();
        return;
    }
    if (bAPIRequestStarted && (HasAPIRequestContextChanged() || GetAPINowSeconds() >= APIRequestDeadline))
    {
        bAPIRequestCancelled = true;
    }
    bAPIRequestFinished = true;
    bAPIRetryPending = false;
    StopAPIRequestTicker();
    FWorldDelegates::OnWorldCleanup.Remove(APIWorldCleanupHandle);
    APIWorldCleanupHandle.Reset();
#if WITH_DEV_AUTOMATION_TESTS
    if (TestFinalResult) TestFinalResult(bSuccess && !bAPIRequestCancelled);
#endif
    if (bSuccess && !bAPIRequestCancelled) HandleSuccess();
    else HandleFailure();
    if (IsRooted()) RemoveFromRoot();
}

void UConvaiAPIBaseProxy::BeginDestroy()
{
    StopAPIRequestTicker();
    FWorldDelegates::OnWorldCleanup.Remove(APIWorldCleanupHandle);
    if (ActiveAPIRequest.IsValid())
    {
        ActiveAPIRequest->OnProcessRequestComplete().Unbind();
        ActiveAPIRequest->CancelRequest();
        ActiveAPIRequest.Reset();
    }
    Super::BeginDestroy();
}

bool UConvaiAPIBaseProxy::ConfigureRequest(TSharedRef<CONVAI_HTTP_REQUEST_INTERFACE> Request, const TCHAR* Verb)
{
    if (!UConvaiFormValidation::ValidateInputText(URL) || !UConvaiFormValidation::ValidateInputText(Verb))
    {
        MarkAPIRequestConfigurationFailure();
        return false;
    }

    TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
    FString AuthKey = AuthHeaderAndKey.Value;
    FString AuthHeader = AuthHeaderAndKey.Key;

    if (!UConvaiFormValidation::ValidateAuthKey(AuthKey))
    {
        MarkAPIRequestConfigurationFailure();
        return false;
    }

    Request->SetURL(URL);
    Request->SetVerb(Verb);
    Request->SetHeader(AuthHeader, AuthKey);

    // Child classes can add filed to this object
    TSharedPtr<FJsonObject> ObjectToSend = MakeShareable(new FJsonObject);
    if (AddContentToRequestAsString(ObjectToSend))
    {
        Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

        FString Content;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Content);
        FJsonSerializer::Serialize(ObjectToSend.ToSharedRef(), Writer);

        Request->SetContentAsString(Content);

        return true;
    }

    if (bAPIRequestConfigurationFailed) return false;

    CONVAI_HTTP_PAYLOAD_ARRAY_TYPE DataToSend;
    FString Boundary = TEXT("ConvaiPluginFormBoundary") + FString::FromInt(FDateTime::Now().GetTicks());
    if (AddContentToRequest(DataToSend, Boundary))
    {
        // MoveTemp hands the body over on engines with the rvalue SetContent (5.4-5.8 checked);
        // where it is missing, the call binds to the const& overload and copies as before.
        if (FString(Verb).Equals(ConvaiHttpConstants::PUT))
        {
            Request->SetHeader(TEXT("Content-Type"), TEXT("application/octet-stream"));
            Request->SetContent(MoveTemp(DataToSend));
            return true;
        }
        
        Request->SetHeader(TEXT("Content-Type"), FString::Printf(TEXT("multipart/form-data; boundary=----%s"), *Boundary));

        // Add closing boundary
        FString ClosingBoundary = FString::Printf(TEXT("\r\n------%s--\r\n"), *Boundary);
        DataToSend.Append((uint8*)TCHAR_TO_UTF8(*ClosingBoundary), ClosingBoundary.Len());

        // Set the request content and content length
        Request->SetHeader(TEXT("Content-Length"), FString::FromInt(DataToSend.Num()));
        Request->SetContent(MoveTemp(DataToSend));

        return true;
    }

    return !bAPIRequestConfigurationFailed;
}

void UConvaiAPIBaseProxy::HandleAPIRequestResponse(CONVAI_HTTP_REQUEST_PTR Request, CONVAI_HTTP_RESPONSE_PTR Response, bool bWasSuccessful)
{
    ResponseCode = Response.IsValid() ? Response->GetResponseCode() : 0;
    FailureResponseBody.Empty();
    if (!Response.IsValid() || !bWasSuccessful || ResponseCode < 200 || ResponseCode > 299)
    {
        if (Response.IsValid()) FailureResponseBody = Response->GetContentAsString();
        CONVAI_LOG(ConvaiBaseHttpLogs, Warning, TEXT("HTTP API request failed (code %d)"), ResponseCode);
        CompleteAPIRequest(false);
        return;
    }
    ResponseString = Response->GetContentAsString();
    ResponseData = Response->GetContent();
    CompleteAPIRequest(ValidateAPIResponse());
}

void UConvaiAPIBaseProxy::HandleSuccess()
{
    RemoveFromRoot();
}

void UConvaiAPIBaseProxy::HandleFailure()
{
    RemoveFromRoot();
}
// END Base api proxy






bool UConvaiAPITokenInBodyProxy::AddContentToRequest(CONVAI_HTTP_PAYLOAD_ARRAY_TYPE& DataToSend, const FString& Boundary)
{
    const TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
    const FString AuthKey = AuthHeaderAndKey.Value;
    const FString AuthHeader = AuthHeaderAndKey.Key;

    if (!UConvaiFormValidation::ValidateAuthKey(AuthKey))
    {
        MarkAPIRequestConfigurationFailure();
        return false;
    }

    if (AuthHeader == ConvaiConstants::Auth_Token_Header)
    {
        const FString ExpIdField = FString::Printf(TEXT("\r\n------%s\r\nContent-Disposition: form-data; name=\"experience_session_id\"\r\n\r\n%s"), *Boundary, *AuthKey);
        DataToSend.Append((uint8*)TCHAR_TO_UTF8(*ExpIdField), ExpIdField.Len());
    }

    return true;
}

bool UConvaiAPITokenInBodyProxy::AddContentToRequestAsString(TSharedPtr<FJsonObject>& ObjectToSend)
{
    const TPair<FString, FString> AuthHeaderAndKey = UConvaiUtils::GetAuthHeaderAndKey();
    const FString AuthKey = AuthHeaderAndKey.Value;
    const FString AuthHeader = AuthHeaderAndKey.Key;

    if (!UConvaiFormValidation::ValidateAuthKey(AuthKey))
    {
        MarkAPIRequestConfigurationFailure();
        return false;
    }

    if (AuthHeader == ConvaiConstants::Auth_Token_Header)
    {
        ObjectToSend->SetStringField(TEXT("experience_session_id"), AuthKey);
    }

    return true;
}
