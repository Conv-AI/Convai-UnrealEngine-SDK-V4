#ifdef __APPLE__

// AVFoundation transitively includes CarbonCore/NumberFormatting.h, whose global
// `struct FVector` collides with Unreal's `FVector` type alias (pulled in via the
// module's shared PCH). Guard the system-header import exactly the way the engine
// does in Mac/MacSystemIncludes.h and AppleTestRunnerHelper.cpp: temporarily rename
// the Carbon symbol out of the way, then restore it.
#pragma push_macro("FVector")
#define FVector FVectorWorkaround
#import <AVFoundation/AVFoundation.h>
#undef FVector
#pragma pop_macro("FVector")

bool GetAppleMicPermission()
{
    __block bool permissionGranted = false;

    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
    {
        case AVAuthorizationStatusAuthorized:
            // Already authorized
            permissionGranted = true;
            break;
        case AVAuthorizationStatusNotDetermined:
        {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            // The user has not yet been asked to grant access to the audio capture device.
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted)
            {
                if (granted)
                {
                    // Permission granted
                    permissionGranted = true;
                }
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
            break;
        }
        case AVAuthorizationStatusRestricted:
        case AVAuthorizationStatusDenied:
            // The user has previously denied access or can't grant access.
            permissionGranted = false;
            break;
    }

    return permissionGranted;
}
#endif
