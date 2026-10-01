// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "Interfaces/IHttpRequest.h"
#include "Misc/EngineVersionComparison.h"

namespace ConvaiEditorHttpCompat
{
inline bool SetReceiveStream(IHttpRequest& Request, TFunction<void(void*, int64&)> Receive)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    return Request.SetResponseBodyReceiveStreamDelegate(FHttpRequestStreamDelegate::CreateLambda(
        [Receive = MoveTemp(Receive)](void* Data, int64 Length)
        {
            const int64 Requested = Length;
            Receive(Data, Length);
            // Legacy HTTP aborts on false; V2 aborts when fewer bytes are consumed.
            return Length == Requested;
        }));
#else
    return Request.SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda(MoveTemp(Receive)));
#endif
}

inline void BindProgress(IHttpRequest& Request, TFunction<void(FHttpRequestPtr, uint64, uint64)> Progress)
{
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    Request.OnRequestProgress().BindLambda(
        [Progress = MoveTemp(Progress), LastSent = uint32(0), LastReceived = uint32(0),
         TotalSent = uint64(0), TotalReceived = uint64(0)](FHttpRequestPtr Active, int32 Sent, int32 Received) mutable
        {
            // Older HTTP counters wrap at 32 bits; keep large-upload progress monotonic.
            TotalSent += uint32(Sent) - LastSent;
            TotalReceived += uint32(Received) - LastReceived;
            LastSent = uint32(Sent); LastReceived = uint32(Received);
            Progress(Active, TotalSent, TotalReceived);
        });
#else
    Request.OnRequestProgress64().BindLambda(MoveTemp(Progress));
#endif
}

inline void UnbindProgress(IHttpRequest& Request)
{
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    Request.OnRequestProgress().Unbind();
#else
    Request.OnRequestProgress64().Unbind();
#endif
}
}
