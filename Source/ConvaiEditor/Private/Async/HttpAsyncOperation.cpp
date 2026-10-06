/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * HttpAsyncOperation.cpp
 *
 * Implementation of HTTP async operations.
 */

#include "Async/HttpAsyncOperation.h"
#include "ConvaiEditor.h"
#include "HttpModule.h"

namespace ConvaiEditor
{
    TSharedPtr<FAsyncOperation<FHttpAsyncResponse>> FHttpAsyncOperation::Create(
        const FHttpAsyncRequest &Request,
        TSharedPtr<FCancellationToken> CancellationToken)
    {
        return MakeShared<FAsyncOperation<FHttpAsyncResponse>>(
            [Request](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<FHttpAsyncResponse>
            {
                return ExecuteHttpRequest(Request, Token, Progress, nullptr, nullptr);
            },
            CancellationToken);
    }

    TSharedPtr<FAsyncOperation<FHttpAsyncResponse>> FHttpAsyncOperation::CreateWithCircuitBreaker(
        const FHttpAsyncRequest &Request,
        TSharedPtr<FCircuitBreaker> CircuitBreaker,
        TSharedPtr<FCancellationToken> CancellationToken)
    {
        return MakeShared<FAsyncOperation<FHttpAsyncResponse>>(
            [Request, CircuitBreaker](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<FHttpAsyncResponse>
            {
                return ExecuteHttpRequest(Request, Token, Progress, CircuitBreaker, nullptr);
            },
            CancellationToken);
    }

    TSharedPtr<FAsyncOperation<FHttpAsyncResponse>> FHttpAsyncOperation::CreateWithRetry(
        const FHttpAsyncRequest &Request,
        TSharedPtr<FRetryPolicy> RetryPolicy,
        TSharedPtr<FCancellationToken> CancellationToken)
    {
        return MakeShared<FAsyncOperation<FHttpAsyncResponse>>(
            [Request, RetryPolicy](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<FHttpAsyncResponse>
            {
                return ExecuteHttpRequest(Request, Token, Progress, nullptr, RetryPolicy);
            },
            CancellationToken);
    }

    TSharedPtr<FAsyncOperation<FHttpAsyncResponse>> FHttpAsyncOperation::CreateWithProtection(
        const FHttpAsyncRequest &Request,
        TSharedPtr<FCircuitBreaker> CircuitBreaker,
        TSharedPtr<FRetryPolicy> RetryPolicy,
        TSharedPtr<FCancellationToken> CancellationToken)
    {
        return MakeShared<FAsyncOperation<FHttpAsyncResponse>>(
            [Request, CircuitBreaker, RetryPolicy](TSharedPtr<FCancellationToken> Token, TSharedPtr<IAsyncProgressReporter> Progress) -> TConvaiResult<FHttpAsyncResponse>
            {
                return ExecuteHttpRequest(Request, Token, Progress, CircuitBreaker, RetryPolicy);
            },
            CancellationToken);
    }

    TConvaiResult<FHttpAsyncResponse> FHttpAsyncOperation::ExecuteHttpRequest(
        const FHttpAsyncRequest &Request,
        TSharedPtr<FCancellationToken> Token,
        TSharedPtr<IAsyncProgressReporter> Progress,
        TSharedPtr<FCircuitBreaker> CircuitBreaker,
        TSharedPtr<FRetryPolicy> RetryPolicy)
    {
        Progress->ReportStage(TEXT("HttpRequest"));

        auto Operation = [&Request, &Token, &Progress]() -> TConvaiResult<FHttpAsyncResponse>
        {
            return PerformHttpRequest(Request, Token, Progress);
        };

        if (CircuitBreaker.IsValid())
        {
            if (RetryPolicy.IsValid())
            {
                return CircuitBreaker->Execute<FHttpAsyncResponse>(
                    [&RetryPolicy, &Operation]() -> TConvaiResult<FHttpAsyncResponse>
                    {
                        return RetryPolicy->Execute<FHttpAsyncResponse>(Operation);
                    });
            }
            else
            {
                return CircuitBreaker->Execute<FHttpAsyncResponse>(Operation);
            }
        }
        else if (RetryPolicy.IsValid())
        {
            return RetryPolicy->Execute<FHttpAsyncResponse>(Operation);
        }
        else
        {
            return Operation();
        }
    }

    TConvaiResult<FHttpAsyncResponse> FHttpAsyncOperation::PerformHttpRequest(
        const FHttpAsyncRequest &Request,
        TSharedPtr<FCancellationToken> Token,
        TSharedPtr<IAsyncProgressReporter> Progress)
    {
        if (Request.URL.IsEmpty())
        {
            return TConvaiResult<FHttpAsyncResponse>::Failure(TEXT("URL is empty"));
        }

        Progress->ReportProgress(0.0f, FString::Printf(TEXT("Connecting to %s..."), *Request.URL));

        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
        HttpRequest->SetURL(Request.URL);
        HttpRequest->SetVerb(Request.Verb);

        if (Request.TimeoutSeconds > 0.0f)
        {
            HttpRequest->SetTimeout(Request.TimeoutSeconds);
        }

        if (!Request.ContentType.IsEmpty())
        {
            HttpRequest->SetHeader(TEXT("Content-Type"), Request.ContentType);
        }

        for (const auto &Header : Request.Headers)
        {
            HttpRequest->SetHeader(Header.Key, Header.Value);
        }

        if (!Request.Body.IsEmpty())
        {
            HttpRequest->SetContentAsString(Request.Body);
        }

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 4)
        HttpRequest->OnRequestProgress().BindLambda(
            [Progress](FHttpRequestPtr Req, int32 BytesSent, int32 BytesReceived)
            {
                if (Progress.IsValid())
                {
                    int32 TotalBytes = Req->GetContentLength();
                    if (TotalBytes > 0)
                    {
                        Progress->ReportTransferProgress(BytesSent + BytesReceived, TotalBytes);
                    }
                }
            });
#else
        HttpRequest->OnRequestProgress64().BindLambda(
            [Progress](FHttpRequestPtr Req, uint64 BytesSent, uint64 BytesReceived)
            {
                if (Progress.IsValid())
                {
                    uint64 TotalBytes = Req->GetContentLength();
                    if (TotalBytes > 0)
                    {
                        Progress->ReportTransferProgress(static_cast<int32>(BytesSent + BytesReceived), static_cast<int32>(TotalBytes));
                    }
                }
            });
#endif

        struct FRequestState
        {
            bool bRequestCompleted = false;
            bool bRequestSuccess = false;
            FHttpResponsePtr HttpResponse = nullptr;
        };
        TSharedPtr<FRequestState> State = MakeShared<FRequestState>();

        HttpRequest->OnProcessRequestComplete().BindLambda(
            [State](FHttpRequestPtr Req, FHttpResponsePtr Response, bool bSuccess)
            {
                if (!Req.IsValid())
                {
                    State->bRequestCompleted = true;
                    State->bRequestSuccess = false;
                    return;
                }

                State->bRequestCompleted = true;
                State->bRequestSuccess = bSuccess;
                State->HttpResponse = Response;
            });

        if (!HttpRequest->ProcessRequest())
        {
            return TConvaiResult<FHttpAsyncResponse>::Failure(TEXT("Failed to send HTTP request"));
        }

        const double StartTime = FPlatformTime::Seconds();
        const int32 MaxIterations = Request.TimeoutSeconds > 0.0f ? static_cast<int32>(Request.TimeoutSeconds * 100) : 3000;
        int32 IterationCount = 0;

        while (!State->bRequestCompleted)
        {
            // Check for engine shutdown
            if (IsEngineExitRequested())
            {
                HttpRequest->CancelRequest();
                return TConvaiResult<FHttpAsyncResponse>::Failure(TEXT("HTTP request cancelled due to engine shutdown"));
            }

            if (Token.IsValid() && Token->IsCancellationRequested())
            {
                HttpRequest->CancelRequest();
                return TConvaiResult<FHttpAsyncResponse>::Failure(TEXT("HTTP request cancelled"));
            }

            if (Request.TimeoutSeconds > 0.0f)
            {
                const double ElapsedTime = FPlatformTime::Seconds() - StartTime;
                if (ElapsedTime >= Request.TimeoutSeconds)
                {
                    HttpRequest->CancelRequest();
                    return TConvaiResult<FHttpAsyncResponse>::Failure(FString::Printf(TEXT("HTTP request timed out after %.1f seconds"), Request.TimeoutSeconds));
                }
            }

            // Safety check: if we've iterated too many times, force exit
            if (++IterationCount >= MaxIterations)
            {
                HttpRequest->CancelRequest();
                return TConvaiResult<FHttpAsyncResponse>::Failure(TEXT("HTTP request exceeded maximum wait iterations"));
            }

            FPlatformProcess::Sleep(0.01f);
        }

        Progress->ReportProgress(1.0f, TEXT("Request completed"));

        if (!State->bRequestSuccess || !State->HttpResponse.IsValid())
        {
            return TConvaiResult<FHttpAsyncResponse>::Failure(TEXT("HTTP request failed - no response"));
        }

        FHttpAsyncResponse Response = ConvertHttpResponse(State->HttpResponse);

        if (!Response.IsSuccess())
        {
            return TConvaiResult<FHttpAsyncResponse>::Failure(FString::Printf(TEXT("HTTP error %d"), Response.ResponseCode))
                .WithCode(Response.ResponseCode);
        }

        return TConvaiResult<FHttpAsyncResponse>::Success(MoveTemp(Response));
    }

    FHttpAsyncResponse FHttpAsyncOperation::ConvertHttpResponse(FHttpResponsePtr HttpResponse)
    {
        FHttpAsyncResponse Response;

        if (HttpResponse.IsValid())
        {
            Response.ResponseCode = HttpResponse->GetResponseCode();
            Response.Body = HttpResponse->GetContentAsString();

            for (const FString &Header : HttpResponse->GetAllHeaders())
            {
                FString Key, Value;
                if (Header.Split(TEXT(": "), &Key, &Value))
                {
                    Response.Headers.Add(Key, Value);
                }
            }
        }

        return Response;
    }

} // namespace ConvaiEditor
