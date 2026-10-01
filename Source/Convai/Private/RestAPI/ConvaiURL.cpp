#include "RestAPI/ConvaiURL.h"
#include "Misc/CommandLine.h"
#include "../Convai.h"
#include "Utility/Log/ConvaiLogger.h"

// Define static members
const TCHAR UConvaiURL::BASE_URL_FORMAT[] = TEXT("https://{0}.convai.com/");

const TCHAR UConvaiURL::LTM_SUBDOMAIN[] = TEXT("user/speaker/");
const TCHAR UConvaiURL::USER_SUBDOMAIN[] = TEXT("user/");
const TCHAR UConvaiURL::CHARACTER_SUBDOMAIN[] = TEXT("character/");
const TCHAR UConvaiURL::NARRATIVE_DESIGN_SUBDOMAIN[] = TEXT("character/narrative/");
const TCHAR UConvaiURL::KNOWLEDGE_BANK_SUBDOMAIN[] = TEXT("character/knowledge-bank/");

TArray<EConvaiEndpoint> UConvaiURL::BetaEndpoints;

FString UConvaiURL::CustomBetaBaseURL = TEXT("");
FString UConvaiURL::CustomProdBaseURL = TEXT("");
bool UConvaiURL::bURLConfigInitialized = false;

void UConvaiURL::InitializeURLConfig()
{
#if UE_BUILD_SHIPPING
    if (bURLConfigInitialized)
    {
        return;
    }
#endif

    // First check settings. Assigned even when empty: outside Shipping this runs on
    // every lookup, so a conditional assignment would leave a cleared setting still
    // routing to the override it used to hold.
    FString SettingsBetaURL = Convai::Get().GetConvaiSettings()->CustomBetaURL;
    SettingsBetaURL.TrimEndInline();
    SettingsBetaURL.TrimStartInline();
    CustomBetaBaseURL = SettingsBetaURL;
    if (!CustomBetaBaseURL.IsEmpty())
    {
        CONVAI_LOG(LogTemp, Log, TEXT("Using beta URL from settings: %s"), *CustomBetaBaseURL);
    }

    FString SettingsProdURL = Convai::Get().GetConvaiSettings()->CustomProdURL;
    SettingsProdURL.TrimEndInline();
    SettingsProdURL.TrimStartInline();
    CustomProdBaseURL = SettingsProdURL;
    if (!CustomProdBaseURL.IsEmpty())
    {
        CONVAI_LOG(LogTemp, Log, TEXT("Using prod URL from settings: %s"), *CustomProdBaseURL);
    }

    // Then check command line parameters (these will override settings if present).
    // FParse::Value keeps whitespace inside a quoted value, so trim here as well or
    // it lands in the middle of the composed URL.
    FString BetaURL;
    if (FParse::Value(FCommandLine::Get(), TEXT("ConvaiBetaURL="), BetaURL))
    {
        BetaURL.TrimEndInline();
        BetaURL.TrimStartInline();
        CustomBetaBaseURL = BetaURL;
        CONVAI_LOG(LogTemp, Log, TEXT("Using custom beta URL from command line: %s"), *CustomBetaBaseURL);
    }

    FString ProdURL;
    if (FParse::Value(FCommandLine::Get(), TEXT("ConvaiProdURL="), ProdURL))
    {
        ProdURL.TrimEndInline();
        ProdURL.TrimStartInline();
        CustomProdBaseURL = ProdURL;
        CONVAI_LOG(LogTemp, Log, TEXT("Using custom prod URL from command line: %s"), *CustomProdBaseURL);
    }

    bURLConfigInitialized = true;
}

FString UConvaiURL::GetBaseURL(const bool bUseBeta)
{
    InitializeURLConfig();
    
    if (bUseBeta)
    {
        if (!CustomBetaBaseURL.IsEmpty())
        {
            return CustomBetaBaseURL;
        }
        return TEXT("https://beta.convai.com");
    }
    else
    {
        if (!CustomProdBaseURL.IsEmpty())
        {
            return CustomProdBaseURL;
        }
        return TEXT("https://api.convai.com");
    }
}

FString UConvaiURL::GetFullURL(const FString& ApiPath, const bool bUseBeta)
{
    FString BaseURL = GetBaseURL(bUseBeta);
    
    // Ensure the base URL ends with a slash and the API path doesn't start with one
    if (!BaseURL.EndsWith(TEXT("/")))
    {
        BaseURL += TEXT("/");
    }
    
    FString Path = ApiPath;
    if (Path.StartsWith(TEXT("/")))
    {
        Path = Path.RightChop(1);
    }
    
    return BaseURL + Path;
}

FString UConvaiURL::GetFormattedBaseURL(const FString& Subdomain)
{
    return FString::Format(BASE_URL_FORMAT, { Subdomain });
}

FString UConvaiURL::GetEndpoint(const EConvaiEndpoint Endpoint)
{
    FString Api;
    switch (Endpoint)
    {
    case EConvaiEndpoint::NewSpeaker:
        Api = FString(LTM_SUBDOMAIN) + TEXT("new");
        break;
    case EConvaiEndpoint::SpeakerIDList:
        Api = FString(LTM_SUBDOMAIN) + TEXT("list");
        break;
    case EConvaiEndpoint::DeleteSpeakerID:
        Api = FString(LTM_SUBDOMAIN) + TEXT("delete");
        break;
    case EConvaiEndpoint::ReferralSourceStatus:
        Api = FString(USER_SUBDOMAIN) + TEXT("referral-source-status");
        break;
    case EConvaiEndpoint::UpdateReferralSource:
        Api = FString(USER_SUBDOMAIN) + TEXT("update-source");
        break;
    case EConvaiEndpoint::UserAPIUsage:
        Api = FString(USER_SUBDOMAIN) + TEXT("user-api-usage");
        break;
    case EConvaiEndpoint::CharacterUpdate:
        Api = FString(CHARACTER_SUBDOMAIN) + TEXT("update");
        break;
    case EConvaiEndpoint::CharacterGet:
        Api = FString(CHARACTER_SUBDOMAIN) + TEXT("get");
        break;
    case EConvaiEndpoint::ListCharacterSections:
        Api = FString(NARRATIVE_DESIGN_SUBDOMAIN) + TEXT("list-sections");
        break;
    case EConvaiEndpoint::ListCharacterTriggers:
        Api = FString(NARRATIVE_DESIGN_SUBDOMAIN) + TEXT("list-triggers");
        break;
    case EConvaiEndpoint::KnowledgeBankUpload:
        Api = FString(KNOWLEDGE_BANK_SUBDOMAIN) + TEXT("upload");
        break;
    case EConvaiEndpoint::KnowledgeBankUpdate:
        Api = FString(KNOWLEDGE_BANK_SUBDOMAIN) + TEXT("update");
        break;
    case EConvaiEndpoint::KnowledgeBankList:
        Api = FString(KNOWLEDGE_BANK_SUBDOMAIN) + TEXT("list");
        break;
    case EConvaiEndpoint::KnowledgeBankDelete:
        Api = FString(KNOWLEDGE_BANK_SUBDOMAIN) + TEXT("delete");
        break;
    default:
        CONVAI_LOG(LogTemp, Warning, TEXT("Invalid endpoint!"));
        return FString();
    }

    const bool bOnProd = !BetaEndpoints.Contains(Endpoint);

    // Routed through GetFullURL rather than composing the host here: that is the
    // only path that consults CustomBetaURL / CustomProdURL and the -Convai*URL=
    // command line flags. Composing from BASE_URL_FORMAT sent every endpoint to
    // convai.com no matter how the user had configured the plugin.
    return GetFullURL(Api, !bOnProd);
}
