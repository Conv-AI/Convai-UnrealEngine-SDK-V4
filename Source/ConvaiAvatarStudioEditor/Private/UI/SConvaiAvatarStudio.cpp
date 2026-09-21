// Copyright Convai Inc. All Rights Reserved.
#include "UI/SConvaiAvatarStudio.h"
#include "UI/ConvaiAvatarThumbnailCache.h"

#include "Algo/Compare.h"
#include "AssetRegistry/AssetData.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "DesktopPlatformModule.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "IDesktopPlatform.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PropertyCustomizationHelpers.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ConvaiStyle.h"
#include "Styling/ConvaiEditorVisualStyle.h"
#include "UI/Widgets/SConvaiAvatarPortrait.h"
#include "Widgets/Colors/SSimpleGradient.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STileView.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "ConvaiAvatarStudio"

namespace AvatarStudioVisual
{
const FLinearColor Background = ConvaiEditorVisual::Window();
const FLinearColor Panel = ConvaiEditorVisual::Panel();
const FLinearColor Muted = ConvaiEditorVisual::SecondaryText();
const FLinearColor Warning = ConvaiEditorVisual::FromHex(TEXT("E5B65A"));
const FLinearColor Error = ConvaiEditorVisual::FromHex(TEXT("E9ABAB"));
const FSlateRoundedBoxBrush Card(ConvaiEditorVisual::Row(), 5.f, ConvaiEditorVisual::Border(), 1.f);
const FSlateRoundedBoxBrush Surface(Panel, 6.f, ConvaiEditorVisual::Border(), 1.f);
const FSlateRoundedBoxBrush Notice(ConvaiEditorVisual::Row(), 5.f, ConvaiEditorVisual::Border(), 1.f);
const FSlateRoundedBoxBrush Separator(ConvaiEditorVisual::Border(), 0.f);

const FSplitterStyle& SplitterStyle()
{
    static const FSplitterStyle Style = FSplitterStyle()
        .SetHandleNormalBrush(FSlateRoundedBoxBrush(Background, 0.f))
        .SetHandleHighlightBrush(FSlateRoundedBoxBrush(ConvaiEditorVisual::Border(), 0.f));
    return Style;
}

const FSlateBrush* SelectedCardBrush()
{
    static const FSlateRoundedBoxBrush Brush(ConvaiEditorVisual::SelectedRow(), 5.f, ConvaiEditorVisual::Accent(), 1.f);
    return &Brush;
}

FSlateFontInfo Font(int32 Size, bool bMedium = false)
{
	FSlateFontInfo Result = FConvaiStyle::Get().GetFontStyle(bMedium ? TEXT("Convai.Font.accountLabel") : TEXT("Convai.Font.accountValue"));
	Result.Size = Size;
	return Result;
}

const FTextBlockStyle& ButtonTextStyle()
{
    static const FTextBlockStyle Style = FTextBlockStyle()
        .SetFont(Font(10, true)).SetColorAndOpacity(FSlateColor::UseForeground());
    return Style;
}

FText FormatBytes(uint64 Bytes)
{
	FNumberFormattingOptions Options;
	Options.MinimumFractionalDigits = 1;
	Options.MaximumFractionalDigits = 1;
	return FText::AsMemory(Bytes, &Options);
}

FText TransferBytes(const FConvaiAvatarStudioViewState& State)
{
	if (!State.JobBytesCompleted.IsSet() || State.JobBytesCompleted.GetValue() < 0) return FText::GetEmpty();
	const FText Completed = FormatBytes(static_cast<uint64>(State.JobBytesCompleted.GetValue()));
	if (State.JobBytesTotal.IsSet() && State.JobBytesTotal.GetValue() > 0)
		return FText::Format(LOCTEXT("TransferBytesKnown", "{0} of {1} transferred"), Completed, FormatBytes(static_cast<uint64>(State.JobBytesTotal.GetValue())));
	return FText::Format(LOCTEXT("TransferBytesUnknown", "{0} transferred"), Completed);
}

FText UploadedFiles(const FConvaiAvatarStudioCard& Avatar)
{
	TArray<FString> Sizes;
	const auto Add = [&Sizes](const FText& Name, const FString& Version, const TOptional<int64>& Bytes)
	{
		if (Bytes.IsSet() && Bytes.GetValue() >= 0)
		{
			FString Engine = Version;
			if (Engine.StartsWith(TEXT("ue-"))) { Engine.RightChopInline(3); int32 Suffix; if (Engine.FindChar(TEXT('-'), Suffix)) Engine.LeftInline(Suffix); }
			if (Engine == TEXT("raw")) Engine.Empty();
			const FText Label = Engine.IsEmpty() ? Name : FText::Format(LOCTEXT("UploadedFileVersion", "{0} (UE {1})"), Name, FText::FromString(Engine));
			Sizes.Add(FText::Format(LOCTEXT("UploadedFileSize", "{0}: {1}"), Label, FormatBytes(static_cast<uint64>(Bytes.GetValue()))).ToString());
		}
	};
	Add(LOCTEXT("WindowsSize", "Windows"), Avatar.WindowsVersion, Avatar.WindowsBytes);
	Add(LOCTEXT("SourceSize", "Editable source"), Avatar.SourceVersion, Avatar.SourceBytes);
	Add(LOCTEXT("LinuxSize", "Linux"), Avatar.LinuxVersion, Avatar.LinuxBytes);
	return FText::FromString(FString::Join(Sizes, TEXT(" | ")));
}

TSharedRef<SWidget> HealthIcon(const FConvaiAvatarStudioCard& Avatar)
{
	return SNew(SBox).WidthOverride(16).HeightOverride(16).HAlign(HAlign_Center).VAlign(VAlign_Center)
		[SNew(SImage).Image(FAppStyle::GetBrush(Avatar.bHealthUnknown ? TEXT("Icons.Info") : TEXT("Icons.Warning")))
			.ColorAndOpacity(Avatar.bHealthUnknown ? Muted : Warning)];
}

TSharedRef<STextBlock> Label(const FText& Text, int32 Size = 10, bool bMuted = false)
{
	return SNew(STextBlock).Text(Text).AutoWrapText(true)
		.Font(Font(Size))
		.ColorAndOpacity(bMuted ? Muted : ConvaiEditorVisual::PrimaryText());
}
}

void SConvaiAvatarStudio::Construct(const FArguments& InArgs)
{
	OnRefresh = InArgs._OnRefresh;
	OnSelect = InArgs._OnSelect;
	OnDownload = InArgs._OnDownload;
	OnUploadChanges = InArgs._OnUploadChanges;
	OnDelete = InArgs._OnDelete;
	OnResumeLocalDraft = InArgs._OnResumeLocalDraft;
	OnDiscardLocalDraft = InArgs._OnDiscardLocalDraft;
	OnCreate = InArgs._OnCreate;
	OnScanDiorama = InArgs._OnScanDiorama;
	OnCaptureCreate = InArgs._OnCaptureCreate;
	OnSaveMetadata = InArgs._OnSaveMetadata;
	OnCaptureMetadata = InArgs._OnCaptureMetadata;
	OnBrowseContent = InArgs._OnBrowseContent;
	OnOpenLog = InArgs._OnOpenLog;
	OnOpenDocumentation = InArgs._OnOpenDocumentation;
	OnCancelJob = InArgs._OnCancelJob;
	OnSignIn = InArgs._OnSignIn;
	OnApplyConfiguration = InArgs._OnApplyConfiguration;
	OnRestoreConfiguration = InArgs._OnRestoreConfiguration;
	OnContinueEngineMismatch = InArgs._OnContinueEngineMismatch;
	ThumbnailCache = MakeShared<FConvaiAvatarThumbnailCache>();

	ChildSlot
	[
		SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(AvatarStudioVisual::Background).Padding(20)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[AvatarStudioVisual::Label(LOCTEXT("Title", "Cloud Avatars"), 22)]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 0)
					[AvatarStudioVisual::Label(LOCTEXT("Subtitle", "Upload and manage custom avatars for Avatar Studio and ConvaiSim streaming."), 10, true)]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
				[SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("About", "About")).OnClicked(this, &SConvaiAvatarStudio::ShowAbout).Tag(TEXT("AvatarStudio.About"))]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0)
				[
					SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("Refresh", "Refresh"))
					.IsEnabled_Lambda([this] { return !State.bBusy && !State.bLoading; })
					.OnClicked_Lambda([this] { ThumbnailCache->RetryFailed(); OnRefresh.ExecuteIfBound(); return FReply::Handled(); })
					.Tag(TEXT("AvatarStudio.Refresh"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("CreateAvatar", "+ Create avatar"))
					.ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle())
					.IsEnabled_Lambda([this] { return State.bCanCreate && !State.bBusy; })
					.ToolTipText_Lambda([this] { return FText::FromString(State.CreateDisabledReason); })
					.OnClicked(this, &SConvaiAvatarStudio::BeginCreate)
					.Tag(TEXT("AvatarStudio.Create"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SVerticalBox).Visibility_Lambda([this] { return State.EngineSummary.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(State.EngineSummary); }).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted).Tag(TEXT("AvatarStudio.EngineSummary"))]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
				[
					SNew(SHorizontalBox).Visibility_Lambda([this] { return State.EngineWarning.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(State.EngineWarning); }).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted)]
					+ SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)
					[SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).ContentPadding(FMargin(10, 6))
						.Text(LOCTEXT("ContinueEngineMismatch", "Continue with this engine"))
						.Visibility_Lambda([this] { return State.bEngineMismatch && !State.bEngineMismatchAcknowledged ? EVisibility::Visible : EVisibility::Collapsed; })
						.IsEnabled_Lambda([this] { return !State.bBusy && !State.bRemoteConfigurationLoading; })
						.OnClicked_Lambda([this] { OnContinueEngineMismatch.ExecuteIfBound(); return FReply::Handled(); }).Tag(TEXT("AvatarStudio.ContinueEngineMismatch"))]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
			[
				BuildProjectSettings()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(SBorder).BorderImage(&AvatarStudioVisual::Notice).Padding(12)
				.Visibility_Lambda([this] { return (!State.Error.IsEmpty() || !State.Notice.IsEmpty() || State.bNeedsSignIn) ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
					[
						SNew(STextBlock).AutoWrapText(true)
						.Text_Lambda([this] { return State.bNeedsSignIn ? LOCTEXT("SignInHelp", "Sign in to Convai to view and upload your avatars.") : FText::FromString(!State.Error.IsEmpty() ? State.Error : State.Notice); })
						.ColorAndOpacity_Lambda([this] { return State.Error.IsEmpty() || State.bNeedsSignIn ? AvatarStudioVisual::Muted : AvatarStudioVisual::Error; })
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)
					[
						SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("SignIn", "Sign in"))
						.Visibility_Lambda([this] { return State.bNeedsSignIn ? EVisibility::Visible : EVisibility::Collapsed; })
						.IsEnabled_Lambda([this] { return !State.bBusy && OnSignIn.IsBound(); })
						.OnClicked_Lambda([this] { if (!State.bBusy && State.bNeedsSignIn) OnSignIn.ExecuteIfBound(); return FReply::Handled(); }).Tag(TEXT("AvatarStudio.SignIn"))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)[BuildLogActions()]
				]
			]
			+ SVerticalBox::Slot().FillHeight(1)
			[
				SNew(SSplitter).Style(&AvatarStudioVisual::SplitterStyle()).PhysicalSplitterHandleSize(12.f)
				+ SSplitter::Slot().Value(0.62f).MinSize(220)
				[SNew(SBorder).BorderImage(&AvatarStudioVisual::Surface).Padding(14)[BuildLibrary()]]
				+ SSplitter::Slot().Value(0.38f).MinSize(300)
				[
					SNew(SBorder).BorderImage(&AvatarStudioVisual::Surface).Padding(18)
					[SAssignNew(DetailsHost, SBox)]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 0)
			[
				SNew(SBorder).BorderImage(&AvatarStudioVisual::Notice).Padding(14)
				.Visibility_Lambda([this] { return State.JobTitle.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1)
						[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(State.JobTitle); }).AutoWrapText(true)]
						+ SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)[BuildLogActions()]
						+ SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)
						[
							SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("CancelJob", "Cancel"))
							.Visibility_Lambda([this] { return State.bBusy && State.bCanCancel ? EVisibility::Visible : EVisibility::Collapsed; })
							.OnClicked_Lambda([this] { OnCancelJob.ExecuteIfBound(); return FReply::Handled(); })
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 8)
					[
						SNew(SProgressBar).FillColorAndOpacity(ConvaiEditorVisual::Accent()).Percent_Lambda([this] { return State.JobProgress; })
						.Visibility_Lambda([this] { return State.bBusy ? EVisibility::Visible : EVisibility::Collapsed; })
					]
					+ SVerticalBox::Slot().AutoHeight()
					[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(State.JobDetail); }).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted)]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox).Padding(FMargin(0, 5, 0, 0))
						.Visibility_Lambda([this] { return AvatarStudioVisual::TransferBytes(State).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
						[SNew(STextBlock).Text_Lambda([this] { return AvatarStudioVisual::TransferBytes(State); })
							.AutoWrapText(true).Font(AvatarStudioVisual::Font(10, true)).Tag(TEXT("AvatarStudio.Transfer.Bytes"))]
					]
				]
			]
		]
	];
	RebuildDetails();
}

SConvaiAvatarStudio::~SConvaiAvatarStudio()
{
	if (FSlateApplication::IsInitialized())
		if (const auto Window = PortraitWindow.Pin()) FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
}

void SConvaiAvatarStudio::Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);
	if (bCreating && DetailsHost)
	{
		const float DetailsWidth = DetailsHost->GetCachedGeometry().GetLocalSize().X;
		if (DetailsWidth > 0.f && bCreateNarrowLayout != (DetailsWidth < 340.f))
		{
			bCreateNarrowLayout = DetailsWidth < 340.f;
			RebuildDetails(); // Reflow only when crossing the breakpoint; the draft owns field values.
		}
	}
	if (!TileView) return;
	// STableViewBase reserves a 16px slot for its internal vertical scrollbar.
	// The library has its own allotted width, independent of its item dimensions.
	const float Available = TileView->GetCachedGeometry().GetLocalSize().X - 16.f;
	if (Available <= 0.f) return;
	const int32 Columns = FMath::Clamp(FMath::FloorToInt(Available / 180.f), 1, 3);
	const float Width = FMath::Max(1.f, FMath::FloorToFloat(Available / Columns));
	if (!FMath::IsNearlyEqual(Width, LibraryItemWidth))
	{
		LibraryItemWidth = Width;
		TileView->SetItemWidth(Width);
		TileView->SetItemHeight(Width);
		// Regenerate virtualized rows as well as arranging their new width.
		TileView->RequestListRefresh();
	}
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildProjectSettings()
{
	return SNew(SExpandableArea).InitiallyCollapsed(true).AreaTitle(LOCTEXT("ProjectSettings", "Project settings · Optional")).Tag(TEXT("AvatarStudio.ProjectSettings"))
		.Padding(FMargin(12, 8)).BorderImage(FAppStyle::GetBrush("NoBorder"))
		.BodyContent()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[AvatarStudioVisual::Label(LOCTEXT("ConfigurationHelp", "Review rendering and packaging changes before applying them. Your previous settings are backed up. Uploading does not require applying these settings to your open project."), 10, true)]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
			[
				SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("ApplyConfiguration", "Apply Avatar Studio configuration..."))
				.IsEnabled_Lambda([this] { return !State.bBusy && State.bCanApplyConfiguration; })
				.OnClicked_Lambda([this] { OnApplyConfiguration.ExecuteIfBound(); return FReply::Handled(); })
				.Tag(TEXT("AvatarStudio.ApplyConfiguration"))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 8)
			[
				SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("RestoreConfiguration", "Restore previous configuration..."))
				.IsEnabled_Lambda([this] { return !State.bBusy && State.bCanRestoreConfiguration; })
				.OnClicked_Lambda([this] { OnRestoreConfiguration.ExecuteIfBound(); return FReply::Handled(); })
				.Tag(TEXT("AvatarStudio.RestoreConfiguration"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(State.ConfigurationStatus.IsEmpty() ? TEXT("Save your work and restart Unreal after applying or restoring settings.") : State.ConfigurationStatus); }).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(State.RemoteConfigurationStatus); }).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted)]
		];
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildLogActions()
{
	return SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("OpenLog", "Open log"))
		.Visibility_Lambda([this] { return State.bCanOpenLog ? EVisibility::Visible : EVisibility::Collapsed; })
		.ToolTipText_Lambda([this] { return FText::FromString(State.DiagnosticReference.IsEmpty() ? TEXT("Open the log for this operation.") : State.DiagnosticReference); })
		.OnClicked_Lambda([this] { OnOpenLog.ExecuteIfBound(TEXT("latest")); return FReply::Handled(); })
		.Tag(TEXT("AvatarStudio.OpenLog"));
}

FReply SConvaiAvatarStudio::ShowAbout()
{
	const FText Dependencies = FText::FromString(State.DependencySummary); // The window may outlive this view.
	TSharedRef<SWindow> Window = SNew(SWindow).Tag(TEXT("AvatarStudio.About.Window")).Title(LOCTEXT("AboutTitle", "About Cloud Avatars"))
		.ClientSize(FVector2D(540, 500)).SupportsMaximize(false).SupportsMinimize(false);
	const TWeakPtr<SWindow> WeakWindow = Window;
	Window->SetContent(SNew(SBorder).Padding(24).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(AvatarStudioVisual::Panel)
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().FillHeight(1)
		[
			SNew(SScrollBox) + SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[AvatarStudioVisual::Label(LOCTEXT("AboutHeading", "Your avatars, ready to build on."), 22)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 24)[AvatarStudioVisual::Label(LOCTEXT("AboutPurpose", "Upload and manage custom avatars for Avatar Studio and ConvaiSim streaming."), 11, true)]
				+ SVerticalBox::Slot().AutoHeight()[AvatarStudioVisual::Label(LOCTEXT("AboutUploadChoicesTitle", "Choose what to upload"), 14)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 18)[AvatarStudioVisual::Label(LOCTEXT("AboutUploadChoices", "Windows package provides the avatar used for streaming. Include editable source is an optional checkbox on the create and update forms. Select both to stream your avatar and keep a copy you can download and edit later."), 11, true)]
				+ SVerticalBox::Slot().AutoHeight()[AvatarStudioVisual::Label(LOCTEXT("AboutSourceTitle", "Edit again or upgrade Unreal Engine"), 14)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 18)[AvatarStudioVisual::Label(LOCTEXT("AboutSource", "Download your uploaded source to edit or recover an avatar. It also gives you the files needed when upgrading avatars to newer Unreal Engine versions. An upgrade may need manual fixes."), 11, true)]
				+ SVerticalBox::Slot().AutoHeight()[AvatarStudioVisual::Label(LOCTEXT("AboutRecoverTitle", "Recover, repair, and upload again"), 14)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 18)[AvatarStudioVisual::Label(LOCTEXT("AboutRecover", "If an avatar needs fixes after an Unreal Engine upgrade, download its source, repair it in your project, and upload it again. You can recover the last uploaded source, including only the changes saved in that upload."), 11, true)]
				+ SVerticalBox::Slot().AutoHeight()[AvatarStudioVisual::Label(LOCTEXT("AboutRevisions", "Upload Windows and editable source together to keep them in sync. Uploading only one leaves the other uploaded file unchanged."), 10, true)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 18, 0, 0)
				[
					SNew(SExpandableArea).InitiallyCollapsed(true).AreaTitle(LOCTEXT("TechnicalDetails", "Technical details"))
					.Tag(TEXT("AvatarStudio.About.TechnicalDetails")).BorderImage(FAppStyle::GetBrush("NoBorder"))
					.BodyContent()[SNew(SBox).MaxDesiredHeight(160).Padding(FMargin(0, 8))
						[SNew(SScrollBox) + SScrollBox::Slot()
							[SNew(STextBlock).Text(Dependencies.IsEmpty() ? LOCTEXT("NoDependencies", "Dependency information is not available yet. Refresh the library and reopen About to check again.") : Dependencies)
								.AutoWrapText(true).Font(AvatarStudioVisual::Font(10)).ColorAndOpacity(AvatarStudioVisual::Muted).Tag(TEXT("AvatarStudio.About.Dependencies"))]]]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 20, 0, 0)
				[SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("OpenDocumentation", "Open uploader documentation")).IsEnabled(OnOpenDocumentation.IsBound()).OnClicked_Lambda([Callback = OnOpenDocumentation] { Callback.ExecuteIfBound(); return FReply::Handled(); }).Tag(TEXT("AvatarStudio.Documentation"))]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 20, 0, 0)
		[SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("CloseAbout", "Done")).Tag(TEXT("AvatarStudio.About.Close")).OnClicked_Lambda([WeakWindow] { if (auto Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow(); return FReply::Handled(); })]
	]);
	if (const auto Parent = FSlateApplication::Get().FindWidgetWindow(AsShared())) FSlateApplication::Get().AddWindowAsNativeChild(Window, Parent.ToSharedRef());
	else FSlateApplication::Get().AddWindow(Window);
	return FReply::Handled();
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildLibrary()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
		[
			SNew(SSearchBox).HintText(LOCTEXT("Search", "Search your avatars"))
			.OnTextChanged_Lambda([this](const FText& Text) { SearchText = Text.ToString(); FilterCards(); })
			.Tag(TEXT("AvatarStudio.Search"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
		[
			SNew(STextBlock).ColorAndOpacity(AvatarStudioVisual::Muted)
			.Text_Lambda([this] { return FText::Format(LOCTEXT("LibraryCount", "YOUR LIBRARY  /  {0}"), FText::AsNumber(State.Avatars.Num())); })
		]
		+ SVerticalBox::Slot().FillHeight(1).Padding(0)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(TileView, STileView<TSharedPtr<FConvaiAvatarStudioCard>>)
				.ListItemsSource(&FilteredCards).ItemWidth(LibraryItemWidth).ItemHeight(LibraryItemWidth)
				.SelectionMode(ESelectionMode::Single)
				.IsEnabled_Lambda([this] { return !State.bBusy; })
				.OnGenerateTile(this, &SConvaiAvatarStudio::GenerateCard)
				.OnSelectionChanged(this, &SConvaiAvatarStudio::SelectionChanged)
				.Tag(TEXT("AvatarStudio.Library"))
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(24)
			[
				SNew(STextBlock).Text(this, &SConvaiAvatarStudio::GetEmptyStateText)
				.Visibility(this, &SConvaiAvatarStudio::GetEmptyStateVisibility)
				.Justification(ETextJustify::Center).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted)
			]
		];
}

TSharedRef<ITableRow> SConvaiAvatarStudio::GenerateCard(TSharedPtr<FConvaiAvatarStudioCard> Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<TSharedPtr<FConvaiAvatarStudioCard>>, Owner).Padding(5)
		.Tag(FName(*(TEXT("AvatarStudio.Card.") + Item->AssetId)))
		[
			SNew(SBorder).Padding(1).Clipping(EWidgetClipping::ClipToBounds)
			.BorderImage_Lambda([this, Item] { return State.SelectedAssetId == Item->AssetId ? AvatarStudioVisual::SelectedCardBrush() : &AvatarStudioVisual::Card; })
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SSimpleGradient).StartColor(FLinearColor(0.48f, 0.54f, 0.57f))
						.EndColor(FLinearColor(0.32f, 0.38f, 0.42f)).Orientation(Orient_Vertical)
				]
				+ SOverlay::Slot()
				[
					SNew(SConvaiAvatarPortrait).ImageVAlign(VAlign_Top).ImageStretch(EStretch::ScaleToFit)
						.Image_Lambda([this, Item] { return GetCardThumbnail(*Item); })
						.IsLoading_Lambda([this, Item] { return IsCardThumbnailLoading(*Item); })
						.FallbackText(FText::FromString(Item->Name.Left(1).ToUpper()))
						.FallbackFont(AvatarStudioVisual::Font(42, true)).FallbackColor(FLinearColor::White)
						.Tag(FName(*(TEXT("AvatarStudio.CardPortrait.") + Item->AssetId)))
				]
				+ SOverlay::Slot().VAlign(VAlign_Bottom)
				[
					SNew(SBox).HeightOverride(72)
					// Slate names the stop-line orientation: horizontal lines fade top to bottom.
					[SNew(SSimpleGradient).StartColor(FLinearColor(0, 0, 0, 0)).EndColor(FLinearColor(0, 0, 0, 0.82f)).Orientation(Orient_Horizontal)]
				]
				+ SOverlay::Slot().VAlign(VAlign_Bottom).Padding(12, 10)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[SNew(STextBlock).Text(FText::FromString(Item->Name)).ToolTipText(FText::FromString(Item->Name))
						.Font(AvatarStudioVisual::Font(11, true)).ColorAndOpacity(FLinearColor::White).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
					[SNew(STextBlock).Text(!Item->Status.IsEmpty() ? FText::FromString(Item->Status) : (Item->bIsLocalDraft ? LOCTEXT("LocalDraftCard", "Local draft · not uploaded") : (Item->bIsLocal ? LOCTEXT("LocalCard", "In this project") : (Item->bHasSource ? LOCTEXT("CloudCard", "Source available") : LOCTEXT("NoSourceCard", "Source not uploaded")))))
						.Font(AvatarStudioVisual::Font(9)).ColorAndOpacity(FLinearColor(0.85f, 0.9f, 0.92f)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
				]
				+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(8)
				[
					SNew(SBorder).BorderImage(&AvatarStudioVisual::Notice).Padding(5)
						.Visibility(Item->HealthWarning.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
						.ToolTipText(FText::FromString(Item->HealthWarning)).Tag(FName(*(TEXT("AvatarStudio.CardHealth.") + Item->AssetId)))
						[AvatarStudioVisual::HealthIcon(*Item)]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildDetails()
{
	const FConvaiAvatarStudioCard* Selected = SelectedCard();
	if (!Selected)
	{
		return SNew(SBox).VAlign(VAlign_Center).HAlign(HAlign_Center)
			[AvatarStudioVisual::Label(LOCTEXT("SelectAnAvatar", "Select an avatar to see its details, or create one from a Blueprint in your project."), 12, true)];
	}
	const FString Name = Selected->Name;
	if (Selected->bIsLocalDraft)
	{
		return SNew(SScrollBox) + SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[AvatarStudioVisual::Label(LOCTEXT("LocalDraftTitle", "LOCAL DRAFT"), 10, true)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[AvatarStudioVisual::Label(FText::FromString(Name), 22)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 18)
			[AvatarStudioVisual::Label(Selected->bIsIncompleteLocalDraft ? LOCTEXT("IncompleteDraftHelp", "Preparation was interrupted. Resume this draft to prepare and upload it.") : LOCTEXT("PreparedDraftHelp", "This avatar is prepared locally and has not been added to your cloud library. Resume to upload it."), 11, true)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("ResumeDraft", "Resume draft")).HAlign(HAlign_Center).ContentPadding(FMargin(12, 9))
				.IsEnabled_Lambda([this] { const auto* Card = SelectedCard(); return !State.bBusy && Card && Card->bIsLocalDraft && Card->bCanResumeDraft; })
				.OnClicked(this, &SConvaiAvatarStudio::ResumeSelectedDraft).Tag(TEXT("AvatarStudio.ResumeDraft"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("DiscardDraft", "Discard local draft...")).HAlign(HAlign_Center)
				.IsEnabled_Lambda([this] { const auto* Card = SelectedCard(); return !State.bBusy && Card && Card->bIsLocalDraft && Card->bCanDiscardDraft; })
				.OnClicked(this, &SConvaiAvatarStudio::DiscardSelectedDraft).Tag(TEXT("AvatarStudio.DiscardDraft"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[AvatarStudioVisual::Label(LOCTEXT("DiscardDraftHelp", "Discarding removes this draft’s prepared copy. Your original avatar stays in this project."), 10, true)]
		];
	}
	if (bEditingMetadata)
	{
		return SNew(SVerticalBox).Tag(TEXT("AvatarStudio.Metadata.Panel"))
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)[AvatarStudioVisual::Label(LOCTEXT("EditDetailsHeading", "Edit details"), 18)]
			+ SVerticalBox::Slot().FillHeight(1)
			[SNew(SScrollBox).Tag(TEXT("AvatarStudio.DetailsScroll")) + SScrollBox::Slot()[BuildMetadata()]]
			+ SVerticalBox::Slot().AutoHeight()[BuildMetadataFooter()];
	}
	const TSharedRef<SWidget> DownloadButton = SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle())
		.ButtonStyle(Selected->bIsLocal ? &ConvaiEditorVisual::SecondaryButtonStyle() : &ConvaiEditorVisual::PrimaryButtonStyle())
		.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(Selected->bIsLocal ? FMargin(8, 6) : FMargin(12, 9))
		.Text_Lambda([this] { const auto* C = SelectedCard(); return C && C->bIsLocal ? LOCTEXT("GetLatest", "Get latest") : LOCTEXT("Download", "Download to project"); })
		.IsEnabled_Lambda([this] { const auto* C = SelectedCard(); return !State.bBusy && !bEditingMetadata && C && C->bCanDownload; })
		.ToolTipText(this, &SConvaiAvatarStudio::GetActionReason, true)
		.OnClicked(this, &SConvaiAvatarStudio::DownloadSelected).Tag(TEXT("AvatarStudio.Download"));
	const TSharedRef<SWidget> BrowseButton = SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle())
		.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(8, 6))
		.Text(LOCTEXT("BrowseContent", "Browse to content"))
		.IsEnabled_Lambda([this] { const auto* C = SelectedCard(); return !State.bBusy && C && C->bCanBrowseContent; })
		.ToolTipText_Lambda([this] { const auto* C = SelectedCard(); return FText::FromString(C && C->bCanBrowseContent ? C->BlueprintPath : C ? C->BrowseDisabledReason : FString()); })
		.OnClicked(this, &SConvaiAvatarStudio::BrowseContent).Tag(TEXT("AvatarStudio.BrowseContent"));
	TSharedRef<SWidget> DownloadActions = SNullWidget::NullWidget;
	if (!Selected->bIsLocal) DownloadActions = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[DownloadButton]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[SNew(STextBlock).Text(this, &SConvaiAvatarStudio::GetActionReason, true).AutoWrapText(true).Font(AvatarStudioVisual::Font(9)).ColorAndOpacity(AvatarStudioVisual::Muted)];
	const TSharedRef<SWidget> UploadActions = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[BuildSourceChoice(false)]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(12, 9))
			.Text(LOCTEXT("UploadChanges", "Upload changes"))
			.IsEnabled_Lambda([this] { const auto* C = SelectedCard(); return !State.bBusy && !bEditingMetadata && C && C->bCanUpload && UploadValidation(false).IsEmpty(); })
			.ToolTipText(this, &SConvaiAvatarStudio::GetActionReason, false)
			.OnClicked(this, &SConvaiAvatarStudio::UploadSelected).Tag(TEXT("AvatarStudio.Upload"))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).Padding(FMargin(0, 6, 0, 0))
			.Visibility_Lambda([this] { return !State.bBusy && !GetActionReason(false).IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
			[SNew(STextBlock).Text(this, &SConvaiAvatarStudio::GetActionReason, false).AutoWrapText(true).Font(AvatarStudioVisual::Font(9)).ColorAndOpacity(AvatarStudioVisual::Muted)]
		];
	TSharedRef<SHorizontalBox> ContentActions = SNew(SHorizontalBox).Tag(TEXT("AvatarStudio.ContentActions"));
	ContentActions->AddSlot().FillWidth(1)[BrowseButton];
	if (Selected->bIsLocal) ContentActions->AddSlot().FillWidth(1).Padding(8, 0, 0, 0)[DownloadButton];
	return SNew(SScrollBox).Tag(TEXT("AvatarStudio.DetailsScroll"))
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 14, 0)
				[
					SNew(SBox).WidthOverride(112).HeightOverride(112).Clipping(EWidgetClipping::ClipToBounds).Tag(TEXT("AvatarStudio.SelectedPortrait"))
					[
						SNew(SConvaiAvatarPortrait).ImageVAlign(VAlign_Center)
						.Image_Lambda([this]() -> const FSlateBrush* { const auto* C = SelectedCard(); return C ? GetCardThumbnail(*C) : nullptr; })
						.IsLoading_Lambda([this] { const auto* C = SelectedCard(); return C && ThumbnailCache && (ThumbnailCache->IsLoading(C->SquareThumbnailUrl) || ThumbnailCache->IsLoading(C->ThumbnailUrl)); })
						.FallbackText(FText::FromString(Name.Left(1).ToUpper()))
						.FallbackFont(AvatarStudioVisual::Font(34, true)).FallbackColor(AvatarStudioVisual::Muted)
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[SNew(STextBlock).Text(FText::FromString(Name)).ToolTipText(FText::FromString(Name)).AutoWrapText(true)
						.WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping).Font(AvatarStudioVisual::Font(18, true))
						.ColorAndOpacity(ConvaiEditorVisual::PrimaryText()).Tag(TEXT("AvatarStudio.SelectedName"))]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 4)[AvatarStudioVisual::Label(FText::FromString(Selected->Status), 10, true)]
					+ SVerticalBox::Slot().AutoHeight()[AvatarStudioVisual::Label(FText::FromString(Selected->EngineVersion.IsEmpty() ? TEXT("Engine version not provided") : FString::Printf(TEXT("Unreal Engine %s"), *Selected->EngineVersion)), 9, true)]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).Padding(FMargin(0, 0, 0, 10)).Visibility(Selected->bDetailsLoading || !Selected->DetailsError.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed)
				[SNew(STextBlock).Text(Selected->bDetailsLoading ? LOCTEXT("CheckingAvatarDetails", "Checking uploaded files...") : FText::FromString(Selected->DetailsError))
					.AutoWrapText(true).Font(AvatarStudioVisual::Font(9)).ColorAndOpacity(AvatarStudioVisual::Muted).Tag(TEXT("AvatarStudio.DetailsStatus"))]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).Padding(FMargin(0, 0, 0, 12)).Visibility(Selected->HealthWarning.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				[SNew(SBorder).BorderImage(&AvatarStudioVisual::Notice).Padding(10).Tag(TEXT("AvatarStudio.Health.Warning"))
					[SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)[AvatarStudioVisual::HealthIcon(*Selected)]
						+ SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(FText::FromString(Selected->HealthWarning))
							.AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping).Font(AvatarStudioVisual::Font(10))
							.ColorAndOpacity(Selected->bHealthUnknown ? AvatarStudioVisual::Muted : AvatarStudioVisual::Warning).Tag(TEXT("AvatarStudio.Health.Text"))]]]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)[BuildMetadata()]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[SNew(STextBlock).Text(!Selected->SourceStatus.IsEmpty() ? FText::FromString(Selected->SourceStatus) :
				(Selected->bHasSource ? LOCTEXT("SourceAvailable", "Editable source available in cloud") : LOCTEXT("SourceUnavailable", "Editable source not uploaded")))
				.AutoWrapText(true).Font(AvatarStudioVisual::Font(10)).ColorAndOpacity(AvatarStudioVisual::Muted).Tag(TEXT("AvatarStudio.SourceStatus"))]
			// One primary action: upload local content, or download source to begin editing.
			+ SVerticalBox::Slot().AutoHeight()[Selected->bIsLocal ? UploadActions : DownloadActions]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SVerticalBox).Visibility(Selected->bIsLocal ? EVisibility::Visible : EVisibility::Collapsed)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0)[ContentActions]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
				[SNew(STextBlock).Text_Lambda([this] { const auto* C = SelectedCard(); return FText::FromString(C && !C->BlueprintPath.IsEmpty() ? TEXT("Blueprint: ") + FSoftObjectPath(C->BlueprintPath).GetAssetName() + TEXT("\nEdit content here, then upload changes.") : TEXT("Download source to edit this avatar’s Blueprint.")); })
					.AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping).Font(AvatarStudioVisual::Font(9)).ColorAndOpacity(AvatarStudioVisual::Muted)]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).Padding(FMargin(0, 6, 0, 0))
				.Visibility_Lambda([this] { const auto* C = SelectedCard(); return C && C->bIsLocal && !State.bBusy && !C->bHasSource ? EVisibility::Visible : EVisibility::Collapsed; })
				[SNew(STextBlock).Text(this, &SConvaiAvatarStudio::GetActionReason, true).Tag(TEXT("AvatarStudio.DownloadHelp"))
					.AutoWrapText(true).Font(AvatarStudioVisual::Font(9)).ColorAndOpacity(AvatarStudioVisual::Muted)]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).Padding(FMargin(0, 12, 0, 0)).Visibility(AvatarStudioVisual::UploadedFiles(*Selected).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				[SNew(SVerticalBox).Tag(TEXT("AvatarStudio.UploadedFiles"))
					+ SVerticalBox::Slot().AutoHeight()[AvatarStudioVisual::Label(LOCTEXT("UploadedFilesTitle", "Uploaded files"), 10, true)]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)[SNew(STextBlock).Text(AvatarStudioVisual::UploadedFiles(*Selected))
						.AutoWrapText(true).Font(AvatarStudioVisual::Font(10)).Tag(TEXT("AvatarStudio.UploadedFiles.Sizes"))]]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 8)
			[SNew(SBox).HeightOverride(1)[SNew(SBorder).BorderImage(&AvatarStudioVisual::Separator).Padding(0)]]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
			[
				SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::QuietDangerButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(8, 6))
				.Text(LOCTEXT("DeleteAvatar", "Delete from library..."))
				.IsEnabled_Lambda([this] { const auto* C = SelectedCard(); return !State.bBusy && C && C->bCanDelete; })
				.ToolTipText_Lambda([this] { const auto* C = SelectedCard(); return FText::FromString(C ? C->DeleteDisabledReason : FString()); })
				.OnClicked(this, &SConvaiAvatarStudio::DeleteSelected).Tag(TEXT("AvatarStudio.Delete"))
			]
		];
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildMetadata()
{
	const auto* Card = SelectedCard();
	if (!Card) return SNullWidget::NullWidget;
	if (!bEditingMetadata)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
			[AvatarStudioVisual::Label(FText::FromString(FString(TEXT("Gender: ")) + (Card->Gender == TEXT("female") ? TEXT("Female") : Card->Gender == TEXT("male") ? TEXT("Male") : TEXT("Not provided"))), 10, true)]
			+ SHorizontalBox::Slot().AutoWidth()
			[SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("EditDetails", "Edit details"))
				.IsEnabled_Lambda([this] { const auto* C = SelectedCard(); return !State.bBusy && C && C->bCanEditMetadata; })
				.ToolTipText_Lambda([this] { const auto* C = SelectedCard(); return FText::FromString(C ? C->MetadataDisabledReason : FString()); })
				.OnClicked(this, &SConvaiAvatarStudio::BeginMetadataEdit).Tag(TEXT("AvatarStudio.Metadata.Edit"))];
	}
	TSharedRef<SVerticalBox> Form = SNew(SVerticalBox);
	Form->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
	[SNew(STextBlock).Text(LOCTEXT("MetadataHelp", "Name, gender, and thumbnail changes save without uploading avatar files."))
		.AutoWrapText(true).Font(AvatarStudioVisual::Font(10)).ColorAndOpacity(AvatarStudioVisual::Muted).Tag(TEXT("AvatarStudio.Metadata.Help"))];
	Form->AddSlot().AutoHeight().Padding(0, 0, 0, 6)[AvatarStudioVisual::Label(LOCTEXT("NameLabel", "Avatar name"), 11)];
	Form->AddSlot().AutoHeight().Padding(0, 0, 0, 12)
	[SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(MetadataDraft.DisplayName); }).IsEnabled_Lambda([this] { return !State.bBusy; })
		.OnTextChanged_Lambda([this](const FText& Text) { MetadataDraft.DisplayName = Text.ToString(); }).Tag(TEXT("AvatarStudio.Metadata.Name"))];
	Form->AddSlot().AutoHeight().Padding(0, 0, 0, 6)[AvatarStudioVisual::Label(LOCTEXT("GenderLabel", "Gender"), 11)];
	TSharedRef<SHorizontalBox> Gender = SNew(SHorizontalBox);
	for (const FString Value : { FString(TEXT("female")), FString(TEXT("male")) })
	{
		Gender->AddSlot().AutoWidth().Padding(0, 0, 24, 0)
		[SNew(SCheckBox).Style(FAppStyle::Get(), "RadioButton").IsEnabled_Lambda([this] { return !State.bBusy; })
			.IsChecked_Lambda([this, Value] { return MetadataDraft.Gender == Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Value](ECheckBoxState Check) { if (!State.bBusy && Check == ECheckBoxState::Checked) MetadataDraft.Gender = Value; })
			.Tag(FName(*(TEXT("AvatarStudio.Metadata.Gender.") + Value)))
			[AvatarStudioVisual::Label(FText::FromString(Value == TEXT("female") ? TEXT("Female") : TEXT("Male")), 11)]];
	}
	Form->AddSlot().AutoHeight().Padding(0, 0, 0, 12)[Gender];
	Form->AddSlot().AutoHeight().Padding(0, 0, 0, 14)[BuildThumbnailEditor(false)];
	return Form;
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildMetadataFooter()
{
	return SNew(SVerticalBox).Tag(TEXT("AvatarStudio.Metadata.Footer"))
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 10)
		[SNew(SBox).HeightOverride(1)[SNew(SBorder).BorderImage(&AvatarStudioVisual::Separator).Padding(0)]]
		+ SVerticalBox::Slot().AutoHeight()
		[SNew(SBox).Padding(FMargin(0, 0, 0, 8)).Visibility_Lambda([this] { return MetadataValidation().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			[SNew(STextBlock).Text(this, &SConvaiAvatarStudio::MetadataValidation).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted)]]
		+ SVerticalBox::Slot().AutoHeight()
		[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1)
		[SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("SaveDetails", "Save details")).ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle()).HAlign(HAlign_Center)
			.IsEnabled_Lambda([this] { const auto* C = SelectedCard(); return !State.bBusy && C && C->bCanEditMetadata && MetadataValidation().IsEmpty() && (MetadataDraft.DisplayName.TrimStartAndEnd() != C->Name || MetadataDraft.Gender != C->Gender || !MetadataDraft.ThumbnailPath.IsEmpty()); })
			.OnClicked(this, &SConvaiAvatarStudio::SaveMetadata).Tag(TEXT("AvatarStudio.Metadata.Save"))]
		+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
		[SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("CancelMetadata", "Cancel")).IsEnabled_Lambda([this] { return !State.bBusy; })
			.OnClicked_Lambda([this] { bEditingMetadata = false; MetadataDraft = FConvaiAvatarStudioMetadataRequest(); RebuildDetails(); return FReply::Handled(); }).Tag(TEXT("AvatarStudio.Metadata.Cancel"))]
		];
}

FReply SConvaiAvatarStudio::BeginMetadataEdit()
{
	const auto* C = SelectedCard();
	if (State.bBusy || !C || !C->bCanEditMetadata) return FReply::Handled();
	MetadataDraft = FConvaiAvatarStudioMetadataRequest(); MetadataDraft.AssetId = C->AssetId; MetadataDraft.DisplayName = C->Name; MetadataDraft.Gender = C->Gender;
	MetadataCaptureAssetId.Empty(); MetadataCaptureBlueprint.Empty();
	bEditingMetadata = true; RebuildDetails(); return FReply::Handled();
}

FReply SConvaiAvatarStudio::SaveMetadata()
{
	const auto* C = SelectedCard();
	if (State.bBusy || !bEditingMetadata || !C || !C->bCanEditMetadata || C->AssetId != MetadataDraft.AssetId || !MetadataValidation().IsEmpty()) return FReply::Handled();
	FConvaiAvatarStudioMetadataRequest Request = MetadataDraft; Request.DisplayName.TrimStartAndEndInline();
	OnSaveMetadata.ExecuteIfBound(Request); return FReply::Handled();
}

FText SConvaiAvatarStudio::MetadataValidation() const
{
	if (MetadataDraft.DisplayName.TrimStartAndEnd().IsEmpty()) return LOCTEXT("EnterName", "Enter a name for this avatar.");
	if (MetadataDraft.Gender != TEXT("female") && MetadataDraft.Gender != TEXT("male")) return LOCTEXT("ChooseGender", "Choose the avatar’s gender.");
	return ValidateThumbnail(MetadataDraft.ThumbnailPath, false);
}

void SConvaiAvatarStudio::FinishMetadataEdit(const FString& AssetId)
{
	if (bEditingMetadata && MetadataDraft.AssetId == AssetId && !MetadataDraft.ThumbnailPath.IsEmpty())
	{
		PendingThumbnailPreviews.Add(AssetId, MetadataDraft.ThumbnailPath);
		PendingThumbnailRefresh.Add(AssetId, false);
		if (const auto* C = State.Avatars.FindByPredicate([&AssetId](const auto& Item) { return Item.AssetId == AssetId; }))
		{
			ThumbnailCache->Invalidate(C->ThumbnailUrl);
			ThumbnailCache->Invalidate(C->SquareThumbnailUrl);
		}
	}
	if (bEditingMetadata && MetadataDraft.AssetId == AssetId) { bEditingMetadata = false; MetadataDraft = FConvaiAvatarStudioMetadataRequest(); RebuildDetails(); }
}

FReply SConvaiAvatarStudio::BrowseMetadataThumbnail()
{
	if (State.bBusy || !bEditingMetadata) return FReply::Handled();
	if (IDesktopPlatform* Desktop = FDesktopPlatformModule::Get())
	{
		TArray<FString> Files;
		if (Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()), TEXT("Choose avatar thumbnail"), FPaths::ProjectDir(), TEXT(""), TEXT("Images (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg"), EFileDialogFlags::None, Files) && !Files.IsEmpty()) MetadataThumbnailChosen(Files[0]);
	}
	return FReply::Handled();
}

void SConvaiAvatarStudio::MetadataThumbnailChosen(const FString& File) { MetadataDraft.ThumbnailPath = File; }

FReply SConvaiAvatarStudio::CaptureCreatePortrait()
{
	if (!bCreating || State.bBusy || !State.bCanCapturePortrait || Draft.DraftId.IsEmpty() || Draft.BlueprintPath.IsEmpty()) return FReply::Handled();
	const FConvaiAvatarStudioCreateRequest Request = Draft;
	OnCaptureCreate.ExecuteIfBound(Request);
	return FReply::Handled();
}

FReply SConvaiAvatarStudio::CaptureMetadataPortrait()
{
	const auto* C = SelectedCard();
	if (!bEditingMetadata || State.bBusy || !State.bCanCapturePortrait || !C || !C->bCanCapturePortrait || C->AssetId != MetadataDraft.AssetId) return FReply::Handled();
	const FConvaiAvatarStudioMetadataRequest Request = MetadataDraft;
	MetadataCaptureAssetId = C->AssetId; MetadataCaptureBlueprint = C->BlueprintPath;
	OnCaptureMetadata.ExecuteIfBound(Request);
	return FReply::Handled();
}

bool SConvaiAvatarStudio::SetCapturedCreateThumbnail(const FConvaiAvatarStudioCreateRequest& Requested, const FString& File)
{
	if (!bCreating || Requested.DraftId.IsEmpty() || Draft.DraftId != Requested.DraftId || Draft.BlueprintPath != Requested.BlueprintPath ||
		Draft.bIsMetaHuman != Requested.bIsMetaHuman || Draft.ThumbnailPath != Requested.ThumbnailPath || !ValidateThumbnail(File, true).IsEmpty()) return false;
	Draft.ThumbnailPath = File; CapturedDraftThumbnail = File;
	return true;
}

bool SConvaiAvatarStudio::SetCapturedMetadataThumbnail(const FConvaiAvatarStudioMetadataRequest& Requested, const FString& File)
{
	const auto* C = SelectedCard();
	if (!bEditingMetadata || Requested.AssetId.IsEmpty() || State.SelectedAssetId != Requested.AssetId || MetadataDraft.AssetId != Requested.AssetId ||
		MetadataCaptureAssetId != Requested.AssetId || !C || C->BlueprintPath != MetadataCaptureBlueprint ||
		MetadataDraft.ThumbnailPath != Requested.ThumbnailPath || !ValidateThumbnail(File, true).IsEmpty()) return false;
	MetadataDraft.ThumbnailPath = File;
	return true;
}

FReply SConvaiAvatarStudio::BrowseContent()
{
	const auto* C = SelectedCard(); if (!State.bBusy && C && C->bCanBrowseContent) { const FString Id = C->AssetId; OnBrowseContent.ExecuteIfBound(Id); }
	return FReply::Handled();
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildSourceChoice(bool bForCreate)
{
	const FString Prefix = bForCreate ? TEXT("AvatarStudio.Create.") : TEXT("AvatarStudio.Update.");
	TSharedRef<SVerticalBox> Source = SNew(SVerticalBox);
	TSharedRef<SVerticalBox> Packages = SNew(SVerticalBox);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const auto Group = Index == 0 ? Source : Packages;
		const FText Label = Index == 0 ? LOCTEXT("SourceOption", "Include editable source") : Index == 1 ? LOCTEXT("WindowsOption", "Windows package") : LOCTEXT("LinuxOption", "Linux package");
		Group->AddSlot().AutoHeight().Padding(0, Index == 0 ? 0 : 8, 0, 3)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, bForCreate, Index] { const auto O = UploadOptions(bForCreate); return (Index == 0 ? O.bIncludeSource : Index == 1 ? O.bIncludeWindows : O.bIncludeLinux) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.IsEnabled_Lambda([this, Index] { return !State.bBusy && !bEditingMetadata && (Index == 0 ? State.bSourceAvailable : Index == 1 ? State.bWindowsAvailable : State.bLinuxAvailable); })
			.OnCheckStateChanged_Lambda([this, bForCreate, Index](ECheckBoxState Check)
			{
				if (State.bBusy || bEditingMetadata || !(Index == 0 ? State.bSourceAvailable : Index == 1 ? State.bWindowsAvailable : State.bLinuxAvailable)) return;
				if (!bForCreate) bUploadOptionsInitialized = true; // Keep explicit choices when published defaults arrive later.
				bool& Value = bForCreate ? (Index == 0 ? Draft.bIncludeSource : Index == 1 ? Draft.bIncludeWindows : Draft.bIncludeLinux) : (Index == 0 ? UpdateOptions.bIncludeSource : Index == 1 ? UpdateOptions.bIncludeWindows : UpdateOptions.bIncludeLinux);
				Value = Check == ECheckBoxState::Checked;
			})
			.Tag(FName(*(Prefix + (Index == 0 ? TEXT("Source") : Index == 1 ? TEXT("Windows") : TEXT("Linux")))))
			[AvatarStudioVisual::Label(Label, 11)]
		];
		Group->AddSlot().AutoHeight().Padding(24, 0, 0, Index == 0 ? 0 : 6)
		[
			SNew(STextBlock).AutoWrapText(true).Font(AvatarStudioVisual::Font(9)).ColorAndOpacity(AvatarStudioVisual::Muted)
			.Tag(FName(*(Prefix + (Index == 0 ? TEXT("SourceHelp") : Index == 1 ? TEXT("WindowsHelp") : TEXT("LinuxHelp")))))
			.Text_Lambda([this, Index]
			{
				const bool bAvailable = Index == 0 ? State.bSourceAvailable : Index == 1 ? State.bWindowsAvailable : State.bLinuxAvailable;
				const FString Reason = Index == 0 ? State.SourceDisabledReason : Index == 1 ? State.WindowsDisabledReason : State.LinuxDisabledReason;
				if (!bAvailable) return FText::FromString(Reason.IsEmpty() ? TEXT("Unavailable in this environment.") : Reason);
				return Index == 0 ? LOCTEXT("SourcePurpose", "Keep a copy to download, edit, or use when upgrading avatars to newer Unreal Engine versions.") : Index == 1 ? LOCTEXT("WindowsPurpose", "Package this avatar for Windows streaming.") : LOCTEXT("LinuxPurpose", "Optional package for Linux streaming.");
			})
		];
	}
	Packages->AddSlot().AutoHeight().Padding(0, 8, 0, 3)
		[
			SNew(SCheckBox).Tag(FName(*(Prefix + TEXT("ConvaiContent"))))
			.IsChecked_Lambda([this, bForCreate] { return UploadOptions(bForCreate).bIncludeConvaiContent ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.IsEnabled_Lambda([this] { return !State.bBusy && !bEditingMetadata; })
			.OnCheckStateChanged_Lambda([this, bForCreate](ECheckBoxState Check)
			{
				if (State.bBusy || bEditingMetadata) return;
				if (bForCreate) Draft.bIncludeConvaiContent = Check == ECheckBoxState::Checked;
				else { UpdateOptions.bIncludeConvaiContent = Check == ECheckBoxState::Checked; bUpdateContentChoiceEdited = true; }
			})
			.ToolTipText(LOCTEXT("ConvaiContentRetention", "Only referenced content is included. Turning this off keeps assets already copied into this avatar."))
			[AvatarStudioVisual::Label(LOCTEXT("IncludeConvaiContent", "Include Convai content"), 11)]
		];
	Packages->AddSlot().AutoHeight().Padding(24, 0, 0, 8)
		[AvatarStudioVisual::Label(LOCTEXT("IncludeConvaiContentHelp", "Keep customized Convai animations and other referenced assets with this avatar. Uploads may be larger."), 9, true)];
	Packages->AddSlot().AutoHeight().HAlign(HAlign_Left).Padding(0, 6)
	[SNew(SButton).Tag(FName(*(Prefix + TEXT("ResetUploadDefaults")))).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("ResetOptions", "Reset to defaults")).IsEnabled_Lambda([this] { return !State.bBusy && !bEditingMetadata && !State.bUploadDefaultsLoading; }).OnClicked_Lambda([this, bForCreate] { ResetUploadOptions(bForCreate); return FReply::Handled(); })];
	Packages->AddSlot().AutoHeight().Padding(0, 2, 0, 6)
	[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(State.UploadDefaultsStatus); }).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted)];
	Packages->AddSlot().AutoHeight().Padding(0, 4, 0, 8)
	[AvatarStudioVisual::Label(LOCTEXT("UnselectedArtifacts", "Unselected cloud files stay unchanged. Source-only uploads skip packaging. An older source copy may not match your latest streaming package."), 10, true)];
	return SNew(SVerticalBox).Tag(FName(*(Prefix + TEXT("UploadContents"))))
		+ SVerticalBox::Slot().AutoHeight()[Source]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(SExpandableArea).InitiallyCollapsed(true).Tag(FName(*(Prefix + TEXT("StreamingPackages"))))
			.HeaderContent()
			[SNew(STextBlock).AutoWrapText(true).Font(AvatarStudioVisual::Font(10))
			.Text_Lambda([this, bForCreate]
			{
				const auto O = UploadOptions(bForCreate);
				const FText Selected = O.bIncludeWindows && O.bIncludeLinux ? LOCTEXT("PackagesBoth", "Windows + Linux") : O.bIncludeWindows ? LOCTEXT("PackagesWindows", "Windows") : O.bIncludeLinux ? LOCTEXT("PackagesLinux", "Linux") : LOCTEXT("PackagesNone", "No packages");
				return FText::Format(O.bIncludeConvaiContent
					? LOCTEXT("AdvancedUploadsWithContent", "Advanced upload options ({0}; Convai content)")
					: LOCTEXT("AdvancedUploads", "Advanced upload options ({0})"), Selected);
			})]
			.BorderImage(FAppStyle::GetBrush("NoBorder")).Padding(FMargin(0, 6)).BodyContent()[Packages]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[SNew(STextBlock).Text_Lambda([this, bForCreate] { return UploadValidation(bForCreate); }).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted)];
}

FConvaiAvatarStudioUploadOptions SConvaiAvatarStudio::UploadOptions(bool bForCreate) const
{
	if (!bForCreate) return UpdateOptions;
	FConvaiAvatarStudioUploadOptions Result; Result.bIncludeSource = Draft.bIncludeSource; Result.bIncludeWindows = Draft.bIncludeWindows; Result.bIncludeLinux = Draft.bIncludeLinux; Result.bIncludeConvaiContent = Draft.bIncludeConvaiContent; return Result;
}

void SConvaiAvatarStudio::ResetUploadOptions(bool bForCreate)
{
	// Published defaults describe uploaded files, not this avatar's content-copy choice.
	const bool Source = State.bDefaultIncludeSource && State.bSourceAvailable, Windows = State.bDefaultIncludeWindows && State.bWindowsAvailable, Linux = State.bDefaultIncludeLinux && State.bLinuxAvailable;
	if (bForCreate) { Draft.bIncludeSource = Source; Draft.bIncludeWindows = Windows; Draft.bIncludeLinux = Linux; }
	else { UpdateOptions.bIncludeSource = Source; UpdateOptions.bIncludeWindows = Windows; UpdateOptions.bIncludeLinux = Linux; }
}

void SConvaiAvatarStudio::SyncConvaiContentChoice()
{
	if (UpdateContentChoiceAssetId != State.SelectedAssetId)
	{
		UpdateContentChoiceAssetId = State.SelectedAssetId;
		bUpdateContentChoiceEdited = false;
	}
	if (!bUpdateContentChoiceEdited)
	{
		const auto* Selected = SelectedCard();
		UpdateOptions.bIncludeConvaiContent = Selected && Selected->bIncludeConvaiContent;
	}
}

FText SConvaiAvatarStudio::UploadSummary(bool bForCreate) const
{
	const auto O = UploadOptions(bForCreate); TArray<FString> Names;
	if (O.bIncludeWindows) Names.Add(TEXT("Windows package"));
	if (O.bIncludeSource) Names.Add(TEXT("Editable source"));
	if (O.bIncludeLinux) Names.Add(TEXT("Linux package"));
	return FText::FromString(Names.IsEmpty() ? TEXT("Choose at least one item") : FString::Join(Names, TEXT(" + ")));
}

FText SConvaiAvatarStudio::UploadValidation(bool bForCreate) const
{
	const auto O = UploadOptions(bForCreate);
	if (!O.bIncludeSource && !O.bIncludeWindows && !O.bIncludeLinux) return LOCTEXT("SelectUploadItem", "Choose at least one item to upload.");
	if ((O.bIncludeSource && !State.bSourceAvailable) || (O.bIncludeWindows && !State.bWindowsAvailable) || (O.bIncludeLinux && !State.bLinuxAvailable)) return LOCTEXT("UnavailableSelection", "An upload option is unavailable. Review editable source and Advanced upload options to choose supported items.");
	return FText::GetEmpty();
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildThumbnailEditor(bool bForCreate)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()[BuildThumbnailPreview(bForCreate)]
		+ SHorizontalBox::Slot().FillWidth(1).Padding(12, 0, 0, 0).VAlign(VAlign_Center)[BuildThumbnailActions(bForCreate)];
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildThumbnailPreview(bool bForCreate)
{
	const FString Prefix = bForCreate ? TEXT("AvatarStudio.Create.") : TEXT("AvatarStudio.Metadata.");
	return SNew(SBox).WidthOverride(128).HeightOverride(256).Tag(FName(*(Prefix + TEXT("PortraitPreview"))))
	[
		SNew(SBorder).BorderImage(&AvatarStudioVisual::Notice).Padding(4)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SConvaiAvatarPortrait).ImageVAlign(VAlign_Center).ImageStretch(EStretch::ScaleToFit)
				.FallbackText(LOCTEXT("NoThumbnail", "No image")).FallbackFont(AvatarStudioVisual::Font(10)).FallbackColor(AvatarStudioVisual::Muted)
				.Image_Lambda([this, bForCreate]() -> const FSlateBrush*
				{
					const FString& Path = bForCreate ? Draft.ThumbnailPath : MetadataDraft.ThumbnailPath;
					if (!Path.IsEmpty()) return ThumbnailCache->GetLocal(Path);
					const auto* Card = bForCreate ? nullptr : SelectedCard();
					return Card ? GetThumbnail(*Card) : nullptr;
				})
			]
			+ SOverlay::Slot()
			[
				SNew(SButton).ButtonStyle(FAppStyle::Get(), "NoBorder").ContentPadding(0)
				.ToolTipText(LOCTEXT("ViewPortrait", "View portrait"))
				.Visibility_Lambda([this, bForCreate] { return GetPortraitFile(bForCreate).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.OnClicked_Lambda([this, bForCreate] { return OpenPortraitPreview(bForCreate); })
				.Tag(FName(*(Prefix + TEXT("ViewPortrait"))))
			]
		]
	];
}

FString SConvaiAvatarStudio::GetPortraitFile(bool bForCreate) const
{
	const FString& DraftPath = bForCreate ? Draft.ThumbnailPath : MetadataDraft.ThumbnailPath;
	if (!DraftPath.IsEmpty()) return DraftPath;
	const auto* Card = bForCreate ? nullptr : SelectedCard();
	return Card ? Card->ThumbnailLocalPath : FString();
}

FReply SConvaiAvatarStudio::OpenPortraitPreview(bool bForCreate)
{
	const FString Path = GetPortraitFile(bForCreate);
	if (Path.IsEmpty()) return FReply::Handled();
	const auto PreviewCache = MakeShared<FConvaiAvatarThumbnailCache>(2048);
	const FSlateBrush* PreviewBrush = ValidateThumbnail(Path, true).IsEmpty() ? PreviewCache->GetLocal(Path) : nullptr;
	if (!PreviewBrush)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("PortraitPreviewUnreadable", "This image could not be opened. Choose a PNG or JPG image up to 4096 pixels on each side and 10 MiB."));
		return FReply::Handled();
	}
	if (const auto Previous = PortraitWindow.Pin()) FSlateApplication::Get().RequestDestroyWindow(Previous.ToSharedRef());
	const FSlateRect WorkArea = FSlateApplication::Get().GetPreferredWorkArea();
	const FVector2D Size(FMath::Min(560.f, (WorkArea.Right - WorkArea.Left) * 0.8f), FMath::Min(1060.f, (WorkArea.Bottom - WorkArea.Top) * 0.85f));
	const TSharedRef<SWindow> Window = SNew(SWindow).Title(LOCTEXT("PortraitWindow", "Avatar portrait"))
		.ClientSize(Size).SizingRule(ESizingRule::UserSized).SupportsMinimize(false).SupportsMaximize(true)
		.AdjustInitialSizeAndPositionForDPIScale(false) // Size is already bounded in desktop pixels.
		.AutoCenter(EAutoCenter::PreferredWorkArea).Tag(TEXT("AvatarStudio.Portrait.Window"))
		[
			SNew(SBorder).BorderImage(&AvatarStudioVisual::Surface).Padding(12)
			[
				SNew(SScaleBox).Stretch(EStretch::ScaleToFit).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					// Keep the decoded snapshot and its cache alive even if the local file changes or disappears.
					SNew(SImage).Image_Lambda([PreviewCache, PreviewBrush] { return PreviewBrush; }).Tag(TEXT("AvatarStudio.Portrait.Image"))
				]
			]
		];
	PortraitWindow = Window;
	FSlateApplication::Get().AddWindow(Window, true);
	return FReply::Handled();
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildThumbnailActions(bool bForCreate, bool bCompact)
{
	const FString Prefix = bForCreate ? TEXT("AvatarStudio.Create.") : TEXT("AvatarStudio.Metadata.");
	return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, bCompact ? 0 : 10)
			[SNew(SBox).Visibility(bCompact ? EVisibility::Collapsed : EVisibility::Visible)[AvatarStudioVisual::Label(LOCTEXT("ThumbnailLabel", "Thumbnail"), 11)]]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(8, bCompact ? 4 : 6))
				.Text_Lambda([this, bForCreate] { return bForCreate && Draft.ThumbnailPath.IsEmpty() ? LOCTEXT("Capture", "Capture") : LOCTEXT("Recapture", "Recapture"); })
				.IsEnabled_Lambda([this, bForCreate]
				{
					if (State.bBusy || !State.bCanCapturePortrait) return false;
					if (bForCreate) return !Draft.BlueprintPath.IsEmpty() && OnCaptureCreate.IsBound();
					const auto* C = SelectedCard(); return C && C->bCanCapturePortrait && OnCaptureMetadata.IsBound();
				})
				.ToolTipText_Lambda([this, bForCreate]
				{
					if (!State.bCanCapturePortrait) return FText::FromString(State.CaptureDisabledReason);
					if (bForCreate && Draft.BlueprintPath.IsEmpty()) return LOCTEXT("CaptureNeedsBlueprint", "Choose an avatar Blueprint first.");
					const auto* C = bForCreate ? nullptr : SelectedCard();
					if (C && !C->bCanCapturePortrait) return FText::FromString(C->CaptureDisabledReason);
					return LOCTEXT("CaptureHelp", "Capture a transparent avatar portrait. It stays in this draft until you save or upload.");
				})
				.OnClicked_Lambda([this, bForCreate] { return bForCreate ? CaptureCreatePortrait() : CaptureMetadataPortrait(); })
				.Tag(FName(*(Prefix + TEXT("CapturePortrait"))))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, bCompact ? 6 : 8, 0, 0)
			[
				SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(8, bCompact ? 4 : 6)).Text(LOCTEXT("ChooseThumbnail", "Choose image..."))
				.IsEnabled_Lambda([this] { return !State.bBusy; })
				.OnClicked_Lambda([this, bForCreate] { return bForCreate ? BrowseThumbnail() : BrowseMetadataThumbnail(); })
				.Tag(FName(*(Prefix + TEXT("Thumbnail"))))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, bCompact ? 6 : 10, 0, 0)
			[
				SNew(STextBlock).Font(AvatarStudioVisual::Font(bCompact ? 9 : 10)).AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
				.ColorAndOpacity(AvatarStudioVisual::Muted)
				.Text_Lambda([this, bForCreate, bCompact]
				{
					const FString& Path = bForCreate ? Draft.ThumbnailPath : MetadataDraft.ThumbnailPath;
					if (bCompact)
					{
						if (!Path.IsEmpty()) return LOCTEXT("SelectedThumbnailCompact", "Your selected image will be used.");
						if (State.bCanCapturePortrait) return LOCTEXT("AutoThumbnailCompact", "Captured on upload if left empty.");
						return LOCTEXT("ChooseThumbnailHelp", "Choose a PNG or JPG image, up to 10 MiB.");
					}
					if (!Path.IsEmpty()) return LOCTEXT("ThumbnailSelected", "PNG or JPG, up to 10 MiB. Your selected image will be used.");
					if (!bForCreate) return LOCTEXT("KeepThumbnail", "Your current thumbnail will be kept until you choose or capture another.");
					return State.bCanCapturePortrait ? LOCTEXT("CaptureOnUpload", "Leave this empty to capture a portrait when you upload.") : LOCTEXT("ChooseThumbnailHelp", "Choose a PNG or JPG image, up to 10 MiB.");
				})
				.ToolTipText_Lambda([this, bForCreate] { return FText::FromString(bForCreate ? Draft.ThumbnailPath : MetadataDraft.ThumbnailPath); })
			]
	;
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildCreateFields()
{
	TSharedRef<SVerticalBox> Fields = SNew(SVerticalBox);
	Fields->AddSlot().AutoHeight().Padding(0, 0, 0, 5)[AvatarStudioVisual::Label(LOCTEXT("NameLabel", "Avatar name"), 11)];
	Fields->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
	[
		SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(Draft.DisplayName); })
		.HintText(LOCTEXT("NameHint", "A name you’ll recognize in your library"))
		.ToolTipText_Lambda([this] { return FText::FromString(Draft.DisplayName); })
		.OnTextChanged_Lambda([this](const FText& Text) { Draft.DisplayName = Text.ToString(); })
		.Tag(TEXT("AvatarStudio.Create.Name"))
		.IsEnabled_Lambda([this] { return !State.bBusy; })
	];
	Fields->AddSlot().AutoHeight().Padding(0, 0, 0, 5)[AvatarStudioVisual::Label(LOCTEXT("GenderLabel", "Gender"), 11)];
	Fields->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SCheckBox).Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this] { return Draft.Gender == TEXT("female") ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState Check) { if (!State.bBusy && Check == ECheckBoxState::Checked) Draft.Gender = TEXT("female"); })
			.IsEnabled_Lambda([this] { return !State.bBusy; }).Tag(TEXT("AvatarStudio.Create.Gender.Female"))
			[AvatarStudioVisual::Label(LOCTEXT("FemaleVoice", "Female"), 11)]
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(18, 0, 0, 0)
		[
			SNew(SCheckBox).Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this] { return Draft.Gender == TEXT("male") ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState Check) { if (!State.bBusy && Check == ECheckBoxState::Checked) Draft.Gender = TEXT("male"); })
			.IsEnabled_Lambda([this] { return !State.bBusy; }).Tag(TEXT("AvatarStudio.Create.Gender.Male"))
			[AvatarStudioVisual::Label(LOCTEXT("MaleVoice", "Male"), 11)]
		]
	];
	Fields->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
	[SNew(SCheckBox).IsChecked_Lambda([this] { return Draft.bIsMetaHuman ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState Check)
		{
			const bool bMetaHuman = Check == ECheckBoxState::Checked;
			if (!State.bBusy && Draft.bIsMetaHuman != bMetaHuman)
			{
				Draft.bIsMetaHuman = bMetaHuman;
				if (!CapturedDraftThumbnail.IsEmpty() && Draft.ThumbnailPath == CapturedDraftThumbnail) Draft.ThumbnailPath.Empty();
				CapturedDraftThumbnail.Empty();
			}
		})
		.ToolTipText(LOCTEXT("MetaHumanChoiceHelp", "Sets up missing animations and replaces the standard MetaHuman face setup. Custom animations are kept."))
		.IsEnabled_Lambda([this] { return !State.bBusy; }).Tag(TEXT("AvatarStudio.Create.IsMetaHuman"))
		[SNew(STextBlock).Text(LOCTEXT("IsMetaHuman", "Is this a MetaHuman?"))]];
	Fields->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this] { return Draft.bIncludeDiorama ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState Check)
		{
			if (State.bBusy) return;
			Draft.bIncludeDiorama = Check == ECheckBoxState::Checked;
			RescanDiorama();
		})
		.ToolTipText(LOCTEXT("IncludeDioramaHelp", "Place your avatar in the level where it should stand. The surroundings upload; Avatar Studio spawns your avatar at that spot."))
		.IsEnabled_Lambda([this] { return !State.bBusy; }).Tag(TEXT("AvatarStudio.Create.IncludeDiorama"))
		[SNew(STextBlock).Text(LOCTEXT("IncludeDiorama", "Include environment"))]
	];
	const auto DioramaVisibility = [this] { return Draft.bIncludeDiorama ? EVisibility::Visible : EVisibility::Collapsed; };
	Fields->AddSlot().AutoHeight()
	[
		SNew(SVerticalBox).Visibility_Lambda(DioramaVisibility)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
		[AvatarStudioVisual::Label(LOCTEXT("DioramaLevelLabel", "Level"), 11)]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
		[
			SNew(SObjectPropertyEntryBox).AllowedClass(UWorld::StaticClass()).AllowClear(true)
			.ObjectPath_Lambda([this] { return Draft.LevelPath; })
			.OnObjectChanged(this, &SConvaiAvatarStudio::LevelChosen)
			.ToolTipText(LOCTEXT("DioramaLevelHelp", "The level your avatar is placed in. It uploads with the avatar."))
			.IsEnabled_Lambda([this] { return !State.bBusy; })
			.Visibility_Lambda(DioramaVisibility).Tag(TEXT("AvatarStudio.Create.Level"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
			[
				SNew(STextBlock).AutoWrapText(true).Font(AvatarStudioVisual::Font(10))
				.Text_Lambda([this] { return State.DioramaStatus.IsEmpty() ? LOCTEXT("DioramaLimitsNotRead", "Limits not read - Re-read policy") : FText::FromString(State.DioramaStatus); })
				.ColorAndOpacity_Lambda([this]
				{
					return State.DioramaErrorCount > 0 ? AvatarStudioVisual::Error : State.DioramaWarningCount > 0 ? AvatarStudioVisual::Warning
						: State.bDioramaLimitsRead ? ConvaiEditorVisual::Accent() : AvatarStudioVisual::Muted;
				})
				.Visibility_Lambda(DioramaVisibility).Tag(TEXT("AvatarStudio.Create.DioramaStatus"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
			[
				SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
				.ContentPadding(FMargin(8, 5)).Text(LOCTEXT("DioramaRescan", "Rescan"))
				.IsEnabled_Lambda([this] { return !State.bBusy; })
				.OnClicked_Lambda([this] { RescanDiorama(); return FReply::Handled(); })
				.Visibility_Lambda(DioramaVisibility).Tag(TEXT("AvatarStudio.Create.DioramaRescan"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).MaxDesiredHeight(180).Tag(TEXT("AvatarStudio.Create.DioramaIssuesViewport"))
			.Visibility_Lambda([this] { return State.DioramaIssues.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				SAssignNew(DioramaIssuesScroll, SScrollBox).Tag(TEXT("AvatarStudio.Create.DioramaIssuesScroll"))
				+ SScrollBox::Slot()[SAssignNew(DioramaIssuesBox, SVerticalBox).Tag(TEXT("AvatarStudio.Create.DioramaIssues"))]
			]
		]
	];
	RefreshDioramaIssues();
	return Fields;
}

TSharedRef<SWidget> SConvaiAvatarStudio::BuildCreate()
{
	TSharedRef<SVerticalBox> Form = SNew(SVerticalBox);
	Form->AddSlot().AutoHeight()[AvatarStudioVisual::Label(LOCTEXT("CreateTitle", "Create avatar"), 22)];
	Form->AddSlot().AutoHeight().Padding(0, 6, 0, 18)
		[AvatarStudioVisual::Label(LOCTEXT("CreateHelp", "Choose an avatar Blueprint from this project. We’ll prepare its dependencies for you."), 10, true)];
	Form->AddSlot().AutoHeight().Padding(0, 0, 0, 7)[AvatarStudioVisual::Label(LOCTEXT("BlueprintLabel", "Avatar Blueprint"), 11)];
	Form->AddSlot().AutoHeight().Padding(0, 0, 0, 18)
	[
		SNew(SObjectPropertyEntryBox).AllowedClass(UBlueprint::StaticClass()).AllowClear(true)
		.ObjectPath_Lambda([this] { return Draft.BlueprintPath; })
		.OnObjectChanged(this, &SConvaiAvatarStudio::BlueprintChosen)
		.IsEnabled_Lambda([this] { return !State.bBusy; })
	];
	if (bCreateNarrowLayout)
	{
		Form->AddSlot().AutoHeight()[BuildCreateFields()];
		Form->AddSlot().AutoHeight().Padding(0, 4, 0, 14)[BuildThumbnailEditor(true)];
	}
	else
	{
		Form->AddSlot().AutoHeight().Padding(0, 0, 0, 14)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)[BuildThumbnailPreview(true)]
			+ SHorizontalBox::Slot().FillWidth(1).Padding(12, 0, 0, 0)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[BuildCreateFields()]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)[BuildThumbnailActions(true, true)]
			]
		];
	}
	Form->AddSlot().AutoHeight().Padding(0, 0, 0, 20)[BuildSourceChoice(true)];
	TSharedRef<SVerticalBox> Footer = SNew(SVerticalBox);
	Footer->AddSlot().AutoHeight().Padding(0, 10, 0, 0)
		[SNew(STextBlock).Text_Lambda([this] { return UploadSummary(true); }).AutoWrapText(true).Font(AvatarStudioVisual::Font(10)).ColorAndOpacity(AvatarStudioVisual::Muted)];
	Footer->AddSlot().AutoHeight().Padding(0, 6, 0, 8)
		[SNew(STextBlock).Text(this, &SConvaiAvatarStudio::GetCreateValidation).AutoWrapText(true).ColorAndOpacity(AvatarStudioVisual::Muted)];
	Footer->AddSlot().AutoHeight()
	[
		SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(12, 9))
		.Text(LOCTEXT("CreateAndUpload", "Create and upload"))
		.IsEnabled(this, &SConvaiAvatarStudio::CanSubmitCreate)
		.OnClicked(this, &SConvaiAvatarStudio::SubmitCreate).Tag(TEXT("AvatarStudio.Create.Submit"))
	];
	Footer->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
	[
		SNew(SButton).TextStyle(&AvatarStudioVisual::ButtonTextStyle()).ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle()).HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10, 6)).Text(LOCTEXT("CancelCreate", "Back to library"))
		.IsEnabled_Lambda([this] { return !State.bBusy; })
		.OnClicked_Lambda([this] { if (!State.bBusy) ShowLibrary(false); return FReply::Handled(); }).Tag(TEXT("AvatarStudio.Create.Back"))
	];
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).Tag(TEXT("AvatarStudio.Create.Scroll")) + SScrollBox::Slot()[Form]]
		+ SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(1)[SNew(SBorder).BorderImage(&AvatarStudioVisual::Separator)]]
		+ SVerticalBox::Slot().AutoHeight()[Footer];
}

void SConvaiAvatarStudio::SetState(const FConvaiAvatarStudioViewState& InState)
{
	check(IsInGameThread());
	const FString PreviousSelection = State.SelectedAssetId;
	const bool bDioramaIssuesChanged = !Algo::Compare(State.DioramaIssues, InState.DioramaIssues,
		[](const FConvaiAvatarDioramaIssue& A, const FConvaiAvatarDioramaIssue& B) { return A.Severity == B.Severity && A.Reason == B.Reason; });
	State = InState;
	if (bDioramaIssuesChanged) RefreshDioramaIssues();
	for (auto It = PendingThumbnailRefresh.CreateIterator(); It; ++It)
	{
		if (State.bLoading) { It.Value() = true; continue; }
		if (!It.Value() || !State.Error.IsEmpty()) continue;
		const auto* Refreshed = State.Avatars.FindByPredicate([&It](const auto& Item) { return Item.AssetId == It.Key(); });
		if (!Refreshed || Refreshed->ThumbnailUrl.IsEmpty()) continue;
		// The fresh record may reuse a signed URL for overwritten bytes. Revalidate it now,
		// not immediately after save while the view still contains the old record.
		ThumbnailCache->Invalidate(Refreshed->ThumbnailUrl);
		ThumbnailCache->Invalidate(Refreshed->SquareThumbnailUrl);
		PendingThumbnailPreviews.Remove(It.Key());
		It.RemoveCurrent();
	}
	if (!bUploadOptionsInitialized && !State.bLoading && !State.bNeedsSignIn && !State.bUploadDefaultsLoading) { ResetUploadOptions(false); bUploadOptionsInitialized = true; }
	if (State.SelectedAssetId.IsEmpty() && State.Avatars.ContainsByPredicate([&PreviousSelection](const auto& C) { return C.AssetId == PreviousSelection; }))
	{
		State.SelectedAssetId = PreviousSelection;
	}
	SyncConvaiContentChoice();
	if (bEditingMetadata && State.SelectedAssetId != MetadataDraft.AssetId) { bEditingMetadata = false; MetadataDraft = FConvaiAvatarStudioMetadataRequest(); }
	FilterCards();
	if (!bCreating && !bEditingMetadata) { RebuildDetails(); }
}

void SConvaiAvatarStudio::FilterCards()
{
	FilteredCards.Reset();
	for (const FConvaiAvatarStudioCard& Card : State.Avatars)
	{
		if (SearchText.IsEmpty() || Card.Name.Contains(SearchText) || Card.AssetId.Contains(SearchText))
		{
			FilteredCards.Add(MakeShared<FConvaiAvatarStudioCard>(Card));
		}
	}
	if (TileView)
	{
		TGuardValue<bool> Guard(bUpdatingSelection, true);
		TileView->RequestListRefresh();
		TileView->ClearSelection();
		for (const auto& Card : FilteredCards)
		{
			if (Card->AssetId == State.SelectedAssetId) { TileView->SetSelection(Card); break; }
		}
	}
}

void SConvaiAvatarStudio::SetSelectedAvatar(const FString& AssetId)
{
	State.SelectedAssetId = AssetId;
	SyncConvaiContentChoice();
	FilterCards();
	if (!bCreating) { RebuildDetails(); }
}

void SConvaiAvatarStudio::SelectionChanged(TSharedPtr<FConvaiAvatarStudioCard> Item, ESelectInfo::Type)
{
	if (bUpdatingSelection || !Item.IsValid() || State.bBusy) { return; }
	State.SelectedAssetId = Item->AssetId;
	SyncConvaiContentChoice();
	bCreating = false;
	bEditingMetadata = false;
	MetadataDraft = FConvaiAvatarStudioMetadataRequest();
	ResetUploadOptions(false);
	RebuildDetails();
	const FString SelectedId = State.SelectedAssetId;
	OnSelect.ExecuteIfBound(SelectedId);
}

const FConvaiAvatarStudioCard* SConvaiAvatarStudio::SelectedCard() const
{
	return State.Avatars.FindByPredicate([this](const FConvaiAvatarStudioCard& Card) { return Card.AssetId == State.SelectedAssetId; });
}

void SConvaiAvatarStudio::RebuildDetails()
{
	if (DetailsHost) { DetailsHost->SetContent(bCreating ? BuildCreate() : BuildDetails()); }
}

void SConvaiAvatarStudio::ShowLibrary(bool bDiscardDraft)
{
	bCreating = false;
	bEditingMetadata = false;
	if (bDiscardDraft) { Draft = FConvaiAvatarStudioCreateRequest(); CapturedDraftThumbnail.Empty(); }
	RebuildDetails();
}

void SConvaiAvatarStudio::ShowDraft(const FConvaiAvatarStudioCreateRequest& Request)
{
	if (State.bBusy) return;
	Draft = Request;
	CapturedDraftThumbnail.Empty();
	if (Draft.DraftId.IsEmpty()) Draft.DraftId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	bCreating = true;
	RebuildDetails();
	RescanDiorama();
}

FReply SConvaiAvatarStudio::BeginCreate()
{
	if (!State.bBusy && State.bCanCreate)
	{
		if (Draft.DraftId.IsEmpty()) { Draft.DraftId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens); ResetUploadOptions(true); }
		bCreating = true;
		bEditingMetadata = false;
		RebuildDetails();
		RescanDiorama();
	}
	return FReply::Handled();
}

FReply SConvaiAvatarStudio::SubmitCreate()
{
	if (!CanSubmitCreate()) { return FReply::Handled(); }
	Draft.DisplayName.TrimStartAndEndInline();
	const FConvaiAvatarStudioCreateRequest Request = Draft;
	OnCreate.ExecuteIfBound(Request);
	return FReply::Handled();
}

bool SConvaiAvatarStudio::CanSubmitCreate() const
{
	return bCreating && !State.bBusy && !State.bNeedsSignIn && State.bCanCreate && !Draft.DraftId.IsEmpty() && !Draft.BlueprintPath.IsEmpty() && !Draft.DisplayName.TrimStartAndEnd().IsEmpty()
		&& (Draft.Gender == TEXT("male") || Draft.Gender == TEXT("female")) && GetThumbnailValidation().IsEmpty() && UploadValidation(true).IsEmpty()
		&& (!Draft.bIncludeDiorama || (!Draft.LevelPath.IsEmpty() && State.bDioramaLimitsRead && State.DioramaErrorCount == 0));
}

FText SConvaiAvatarStudio::GetCreateValidation() const
{
	if (!State.bCanCreate) { return FText::FromString(State.CreateDisabledReason); }
	if (Draft.BlueprintPath.IsEmpty()) { return LOCTEXT("PickBlueprint", "Choose the Blueprint that represents your avatar."); }
	if (Draft.DisplayName.TrimStartAndEnd().IsEmpty()) { return LOCTEXT("EnterName", "Enter a name for this avatar."); }
	if (Draft.Gender != TEXT("male") && Draft.Gender != TEXT("female")) { return LOCTEXT("ChooseGender", "Choose the avatar’s gender."); }
	if (Draft.bIncludeDiorama)
	{
		if (Draft.LevelPath.IsEmpty()) return LOCTEXT("ChooseDioramaLevel", "Choose the level your avatar is placed in");
		if (!State.bDioramaLimitsRead || State.DioramaErrorCount > 0)
			return State.DioramaStatus.IsEmpty() ? LOCTEXT("DioramaLimitsNotRead", "Limits not read - Re-read policy") : FText::FromString(State.DioramaStatus);
	}
	const FText ThumbnailError = GetThumbnailValidation();
	return !ThumbnailError.IsEmpty() ? ThumbnailError : UploadValidation(true);
}

FText SConvaiAvatarStudio::GetThumbnailValidation() const
{
	return ValidateThumbnail(Draft.ThumbnailPath, !State.bCanCapturePortrait);
}

FText SConvaiAvatarStudio::ValidateThumbnail(const FString& Path, bool bRequired) const
{
	if (Path.IsEmpty()) return bRequired ? LOCTEXT("ChooseRequiredThumbnail", "Choose a thumbnail image for this avatar.") : FText::GetEmpty();
	const FString Extension = FPaths::GetExtension(Path).ToLower();
	if (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg"))
		return LOCTEXT("ThumbnailType", "Choose a PNG or JPG image.");
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size <= 0) return LOCTEXT("ThumbnailMissing", "The thumbnail is missing or empty. Choose the image again.");
	if (Size > 10 * 1024 * 1024) return LOCTEXT("ThumbnailTooLarge", "Choose a thumbnail no larger than 10 MiB.");
	const FDateTime Modified = IFileManager::Get().GetTimeStamp(*Path);
	if (ValidatedThumbnailPath == Path && ValidatedThumbnailSize == Size && ValidatedThumbnailTime == Modified) return ThumbnailValidation;
	ValidatedThumbnailPath = Path; ValidatedThumbnailSize = Size; ValidatedThumbnailTime = Modified;
	ThumbnailValidation = LOCTEXT("ThumbnailUnreadable", "This thumbnail could not be read as a PNG or JPG image. Choose another image.");
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() != Size) return ThumbnailValidation;
	IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const EImageFormat Expected = Extension == TEXT("png") ? EImageFormat::PNG : EImageFormat::JPEG;
	if (Images.DetectImageFormat(Bytes.GetData(), Bytes.Num()) != Expected) return ThumbnailValidation;
	const TSharedPtr<IImageWrapper> Wrapper = Images.CreateImageWrapper(Expected);
	if (Wrapper && Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) && Wrapper->GetWidth() > 0 && Wrapper->GetHeight() > 0)
		ThumbnailValidation = Wrapper->GetWidth() > 4096 || Wrapper->GetHeight() > 4096
			? LOCTEXT("ThumbnailDimensions", "Choose a thumbnail no larger than 4096 pixels on either side.") : FText::GetEmpty();
	return ThumbnailValidation;
}

FReply SConvaiAvatarStudio::BrowseThumbnail()
{
	if (IDesktopPlatform* Desktop = FDesktopPlatformModule::Get())
	{
		TArray<FString> Files;
		const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
		if (Desktop->OpenFileDialog(Parent, TEXT("Choose avatar thumbnail"), FPaths::ProjectDir(), TEXT(""), TEXT("Images (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg"), EFileDialogFlags::None, Files) && !Files.IsEmpty())
		{
			ThumbnailChosen(Files[0]);
		}
	}
	return FReply::Handled();
}

void SConvaiAvatarStudio::BlueprintChosen(const FAssetData& Asset)
{
	if (State.bBusy) return;
	const FString Path = Asset.IsValid() ? Asset.GetObjectPathString() : FString();
	const bool bChanged = Draft.BlueprintPath != Path;
	if (bChanged)
	{
		Draft.DraftId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
		if (!CapturedDraftThumbnail.IsEmpty() && Draft.ThumbnailPath == CapturedDraftThumbnail) Draft.ThumbnailPath.Empty();
		CapturedDraftThumbnail.Empty();
	}
	Draft.BlueprintPath = Path;
	if (Draft.DisplayName.IsEmpty() && Asset.IsValid())
	{
		Draft.DisplayName = Asset.AssetName.ToString();
		Draft.DisplayName.RemoveFromStart(TEXT("BP_"));
	}
	if (bChanged) RescanDiorama();
}

void SConvaiAvatarStudio::LevelChosen(const FAssetData& Asset)
{
	if (State.bBusy) return;
	Draft.LevelPath = Asset.IsValid() ? Asset.GetObjectPathString() : FString();
	RescanDiorama();
}

void SConvaiAvatarStudio::RescanDiorama()
{
	if (!bCreating || State.bBusy) return;
	State.DioramaStatus.Empty(); State.DioramaErrorCount = 0; State.DioramaWarningCount = 0;
	State.DioramaIssues.Reset(); State.bDioramaLimitsRead = false;
	RefreshDioramaIssues();
	OnScanDiorama.ExecuteIfBound(Draft);
}

void SConvaiAvatarStudio::RefreshDioramaIssues()
{
	if (!DioramaIssuesBox) return;
	DioramaIssuesBox->ClearChildren();
	if (DioramaIssuesScroll) DioramaIssuesScroll->ScrollToStart();
	for (const FConvaiAvatarDioramaIssue& Issue : State.DioramaIssues)
	{
		const FLinearColor Color = Issue.Severity == EConvaiAvatarDioramaSeverity::Error ? AvatarStudioVisual::Error
			: Issue.Severity == EConvaiAvatarDioramaSeverity::Warning ? AvatarStudioVisual::Warning : AvatarStudioVisual::Muted;
		DioramaIssuesBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock).Text(FText::FromString(Issue.Reason)).AutoWrapText(true)
			.Font(AvatarStudioVisual::Font(10)).ColorAndOpacity(Color)
		];
	}
}
void SConvaiAvatarStudio::ThumbnailChosen(const FString& File) { Draft.ThumbnailPath = File; CapturedDraftThumbnail.Empty(); }
#if WITH_DEV_AUTOMATION_TESTS
bool SConvaiAvatarStudio::AutomationChooseBlueprint(const FAssetData& Asset)
{
	const bool bOptedIn = FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestCloudJourney")) || FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioUIQA"));
	if (!bOptedIn || !bCreating || State.bBusy || !Asset.IsValid()) return false;
	BlueprintChosen(Asset); return Draft.BlueprintPath == Asset.GetObjectPathString();
}
bool SConvaiAvatarStudio::AutomationChooseLevel(const FAssetData& Asset)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioUIQA")) || !bCreating || State.bBusy) return false;
	LevelChosen(Asset); return Draft.LevelPath == (Asset.IsValid() ? Asset.GetObjectPathString() : FString());
}
bool SConvaiAvatarStudio::AutomationChooseThumbnail(const FString& File)
{
	const bool bOptedIn = FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestCloudJourney")) || FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioUIQA"));
	if (!bOptedIn || !bCreating || State.bBusy) return false;
	ThumbnailChosen(File); return GetThumbnailValidation().IsEmpty();
}
bool SConvaiAvatarStudio::AutomationChooseMetadataThumbnail(const FString& File)
{
	const bool bOptedIn = FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestCloudJourney")) || FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestCloudMetadata")) || FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioUIQA"));
	if (!bOptedIn || !bEditingMetadata || State.bBusy) return false;
	MetadataThumbnailChosen(File); return ValidateThumbnail(MetadataDraft.ThumbnailPath, false).IsEmpty();
}
bool SConvaiAvatarStudio::AutomationSelectAvatar(const FString& AssetId)
{
	const bool bOptedIn = FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestCloudJourney")) || FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestCloudMetadata")) || FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestCloudFreshDownload")) || FParse::Param(FCommandLine::Get(), TEXT("AvatarStudioTestDeleteTutorial"));
	if (!bOptedIn || State.bBusy || !TileView) return false;
	const FString Id = AssetId;
	for (const auto& Card : FilteredCards)
	{
		if (Card->AssetId != Id) continue;
		const auto Selected = Card;
		TileView->ClearSelection();
		TileView->SetSelection(Selected, ESelectInfo::OnMouseClick); // Real SelectionChanged synchronizes the controller.
		return State.SelectedAssetId == Id;
	}
	return false;
}
bool SConvaiAvatarStudio::AutomationHasRemoteThumbnail(const FString& AssetId) const
{
	const auto* Card = State.Avatars.FindByPredicate([&AssetId](const auto& Item) { return Item.AssetId == AssetId; });
	return Card && ThumbnailCache && !PendingThumbnailRefresh.Contains(AssetId) && ThumbnailCache->HasRemoteImage(Card->ThumbnailUrl);
}
bool SConvaiAvatarStudio::AutomationRemoteThumbnailMatches(const FString& AssetId, const FString& ExpectedHash) const
{
	const auto* Card = State.Avatars.FindByPredicate([&AssetId](const auto& Item) { return Item.AssetId == AssetId; });
	return Card && ThumbnailCache && !PendingThumbnailRefresh.Contains(AssetId) && ThumbnailCache->RemoteImageMatches(Card->ThumbnailUrl, ExpectedHash);
}
#endif

FReply SConvaiAvatarStudio::DeleteSelected()
{
	const FConvaiAvatarStudioCard* Card = SelectedCard();
	if (State.bBusy || !Card || !Card->bCanDelete) { return FReply::Handled(); }
	const FString Id = Card->AssetId;
	const FString Name = Card->Name;
	const FText Prompt = FText::Format(LOCTEXT("ConfirmDelete", "Delete ‘{0}’ from your avatar library?\n\nThis removes its uploaded files. Local project files will stay in place."), FText::FromString(Name));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Prompt, LOCTEXT("DeleteTitle", "Delete avatar")) == EAppReturnType::Yes)
	{
		// Modal dialogs pump Slate, so permissions may have changed while the prompt was open.
		const auto* Current = State.Avatars.FindByPredicate([&Id](const FConvaiAvatarStudioCard& Candidate) { return Candidate.AssetId == Id; });
		if (!State.bBusy && Current && Current->bCanDelete) { OnDelete.ExecuteIfBound(Id); }
	}
	return FReply::Handled();
}

FReply SConvaiAvatarStudio::UploadSelected()
{
	const auto* Card = SelectedCard();
	if (!State.bBusy && Card && Card->bCanUpload)
	{
		const FString Id = Card->AssetId;
		const FConvaiAvatarStudioUploadOptions Options = UploadOptions(false);
		if (!bEditingMetadata && UploadValidation(false).IsEmpty()) OnUploadChanges.ExecuteIfBound(Id, Options);
	}
	return FReply::Handled();
}

FReply SConvaiAvatarStudio::DownloadSelected()
{
	const auto* Card = SelectedCard();
	if (!State.bBusy && !bEditingMetadata && Card && Card->bCanDownload)
	{
		const FString Id = Card->AssetId;
		OnDownload.ExecuteIfBound(Id);
	}
	return FReply::Handled();
}

FReply SConvaiAvatarStudio::ResumeSelectedDraft()
{
	const auto* Card = SelectedCard();
	if (!State.bBusy && Card && Card->bIsLocalDraft && Card->bCanResumeDraft)
	{
		const FString Id = Card->AssetId;
		OnResumeLocalDraft.ExecuteIfBound(Id);
	}
	return FReply::Handled();
}

FReply SConvaiAvatarStudio::DiscardSelectedDraft()
{
	const auto* Card = SelectedCard();
	if (State.bBusy || !Card || !Card->bIsLocalDraft || !Card->bCanDiscardDraft) return FReply::Handled();
	const FString Id = Card->AssetId;
	const FString Name = Card->Name;
	const FText Prompt = FText::Format(LOCTEXT("ConfirmDiscardDraft", "Discard local draft ‘{0}’?\n\nThis removes the draft’s prepared copy. Your original avatar stays in this project."), FText::FromString(Name));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Prompt, LOCTEXT("DiscardDraftTitle", "Discard local draft")) == EAppReturnType::Yes)
	{
		const auto* Current = State.Avatars.FindByPredicate([&Id](const FConvaiAvatarStudioCard& Candidate) { return Candidate.AssetId == Id; });
		if (!State.bBusy && Current && Current->bIsLocalDraft && Current->bCanDiscardDraft) OnDiscardLocalDraft.ExecuteIfBound(Id);
	}
	return FReply::Handled();
}

FText SConvaiAvatarStudio::GetActionReason(bool bDownload) const
{
	const auto* Card = SelectedCard();
	if (!Card) { return FText::GetEmpty(); }
	if (bEditingMetadata) return LOCTEXT("FinishMetadataFirst", "Save or cancel detail edits before changing avatar content.");
	if (State.bBusy) return LOCTEXT("WaitForAvatarOperation", "Wait for the current operation to finish.");
	if (bDownload)
	{
		if (!Card->bSourceStatusKnown) { return LOCTEXT("UncheckedSourceHelp", "Checks the uploaded source before downloading it to this project."); }
		if (!Card->bHasSource) { return LOCTEXT("NoEditableSourceHelp", "The owner must upload editable source before this avatar can be added to a project."); }
		if (!Card->bCanDownload) { return FText::FromString(Card->DownloadDisabledReason); }
		return Card->bIsLocal ? LOCTEXT("GetLatestHelp", "Checks the uploaded source before replacing the local copy.") : LOCTEXT("DownloadHelp", "Adds this avatar’s editable source to your project.");
	}
	return FText::FromString(Card->UploadDisabledReason);
}

FText SConvaiAvatarStudio::GetEmptyStateText() const
{
	if (State.bLoading) { return LOCTEXT("Loading", "Loading your avatar library..."); }
	if (State.bNeedsSignIn) { return LOCTEXT("NeedsSignIn", "Sign in to Convai to see your avatar library."); }
	if (!State.Error.IsEmpty()) { return LOCTEXT("LoadError", "Your library could not be loaded. Resolve the message above, then refresh."); }
	if (!SearchText.IsEmpty()) { return LOCTEXT("NoMatches", "No avatars match your search."); }
	return LOCTEXT("EmptyLibrary", "Your avatar library starts here.\nCreate an avatar from a Blueprint in this project.");
}

EVisibility SConvaiAvatarStudio::GetEmptyStateVisibility() const
{
	return FilteredCards.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
}

const FSlateBrush* SConvaiAvatarStudio::GetThumbnail(const FConvaiAvatarStudioCard& Card) const
{
	if (const FString* Preview = PendingThumbnailPreviews.Find(Card.AssetId)) return ThumbnailCache->GetLocal(*Preview);
	if (Card.ThumbnailBrush.IsValid()) return Card.ThumbnailBrush.Get();
	if (const FSlateBrush* Remote = ThumbnailCache->GetOrRequest(Card.ThumbnailUrl)) return Remote;
	return ThumbnailCache->GetLocal(Card.ThumbnailLocalPath);
}

const FSlateBrush* SConvaiAvatarStudio::GetCardThumbnail(const FConvaiAvatarStudioCard& Avatar) const
{
	if (const FString* Preview = PendingThumbnailPreviews.Find(Avatar.AssetId)) return ThumbnailCache->GetLocal(*Preview, true);
	if (Avatar.ThumbnailBrush.IsValid()) return Avatar.ThumbnailBrush.Get();
	// Use only an explicitly supplied square URL. The Assets API may omit it;
	// never infer object-storage locations from the original thumbnail URL.
	if (!Avatar.SquareThumbnailUrl.IsEmpty())
	{
		if (const FSlateBrush* Square = ThumbnailCache->GetOrRequest(Avatar.SquareThumbnailUrl, true)) return Square;
		if (ThumbnailCache->IsLoading(Avatar.SquareThumbnailUrl)) return ThumbnailCache->GetLocal(Avatar.ThumbnailLocalPath, true);
	}
	if (const FSlateBrush* Original = ThumbnailCache->GetOrRequest(Avatar.ThumbnailUrl, true)) return Original;
	return ThumbnailCache->GetLocal(Avatar.ThumbnailLocalPath, true);
}

bool SConvaiAvatarStudio::IsCardThumbnailLoading(const FConvaiAvatarStudioCard& Avatar) const
{
	return ThumbnailCache && (ThumbnailCache->IsLoading(Avatar.SquareThumbnailUrl) || ThumbnailCache->IsLoading(Avatar.ThumbnailUrl));
}

#undef LOCTEXT_NAMESPACE
