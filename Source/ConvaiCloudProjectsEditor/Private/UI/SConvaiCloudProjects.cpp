// Copyright Convai Inc. All Rights Reserved.
#include "UI/SConvaiCloudProjects.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/ConvaiEditorVisualStyle.h"
#include "Styling/ConvaiStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "ConvaiCloudProjects"

namespace CloudProjectsVisual
{
// Cloud Avatars sets the house style: one typographic scale, generous spacing, and almost no
// borders. Sections are separated by space rather than nested boxes -- stacking bordered panels
// inside bordered panels is what makes a dense panel read as cluttered.
const FLinearColor Background = ConvaiEditorVisual::Window();
const FLinearColor Muted = ConvaiEditorVisual::SecondaryText();
const FLinearColor Accent = ConvaiEditorVisual::Accent();
const FLinearColor Warning = ConvaiEditorVisual::FromHex(TEXT("E5B65A"));
const FLinearColor Failure = ConvaiEditorVisual::FromHex(TEXT("E9ABAB"));
const FSlateRoundedBoxBrush Row(ConvaiEditorVisual::Row(), 5.f);
const FSlateRoundedBoxBrush SelectedRow(ConvaiEditorVisual::SelectedRow(), 5.f, ConvaiEditorVisual::Accent(), 1.f);
const FSlateRoundedBoxBrush Well(ConvaiEditorVisual::Row(), 5.f, ConvaiEditorVisual::Border(), 1.f);

FSlateFontInfo Font(int32 Size, bool bMedium = false)
{
	FSlateFontInfo Result = FConvaiStyle::Get().GetFontStyle(
		bMedium ? TEXT("Convai.Font.accountLabel") : TEXT("Convai.Font.accountValue"));
	Result.Size = Size;
	return Result;
}

const FTextBlockStyle& ButtonTextStyle()
{
	static const FTextBlockStyle Style = FTextBlockStyle()
		.SetFont(Font(10, true)).SetColorAndOpacity(FSlateColor::UseForeground());
	return Style;
}

TSharedRef<SWidget> Label(const FText& Text, int32 Size = 11, bool bMuted = false, bool bMedium = false)
{
	return SNew(STextBlock)
		.Text(Text)
		.Font(Font(Size, bMedium))
		.ColorAndOpacity(bMuted ? Muted : ConvaiEditorVisual::PrimaryText())
		.AutoWrapText(true);
}
}

namespace
{
/** Pipeline order. Used for "step N of 6" rather than a row of chips. */
const TArray<TPair<EConvaiProjectStage, FString>>& Stages()
{
	static const TArray<TPair<EConvaiProjectStage, FString>> Value = {
		{ EConvaiProjectStage::Prepare,  TEXT("Prepare") },
		{ EConvaiProjectStage::Package,  TEXT("Package") },
		{ EConvaiProjectStage::Compress, TEXT("Compress") },
		{ EConvaiProjectStage::Upload,   TEXT("Upload") },
		{ EConvaiProjectStage::Build,    TEXT("Cloud build") },
		{ EConvaiProjectStage::Activate, TEXT("Activate") },
	};
	return Value;
}

int32 StageIndex(EConvaiProjectStage Stage)
{
	return Stages().IndexOfByPredicate([Stage](const TPair<EConvaiProjectStage, FString>& Entry)
	{
		return Entry.Key == Stage;
	});
}

/** "4m 12s" / "1h 06m". Seconds are noise once a package has been running for an hour. */
FString Elapsed(double Since)
{
	if (Since <= 0.0) return FString();
	const int32 Total = FMath::Max(0, FMath::FloorToInt(FPlatformTime::Seconds() - Since));
	if (Total < 60) return FString::Printf(TEXT("%ds"), Total);
	if (Total < 3600) return FString::Printf(TEXT("%dm %02ds"), Total / 60, Total % 60);
	return FString::Printf(TEXT("%dh %02dm"), Total / 3600, (Total % 3600) / 60);
}
}

void SConvaiCloudProjects::Construct(const FArguments& InArgs)
{
	OnRefresh = InArgs._OnRefresh;
	OnSignIn = InArgs._OnSignIn;
	OnCancelJob = InArgs._OnCancelJob;
	OnSelect = InArgs._OnSelect;
	OnCreate = InArgs._OnCreate;
	OnUploadChanges = InArgs._OnUploadChanges;
	OnOpenInBrowser = InArgs._OnOpenInBrowser;
	OnActivateVersion = InArgs._OnActivateVersion;
	OnShowLogs = InArgs._OnShowLogs;
	OnPublish = InArgs._OnPublish;
	OnSaveSettings = InArgs._OnSaveSettings;

	// Header, spacing and button metrics are lifted from Cloud Avatars so the two tools are
	// indistinguishable in layout: 20px frame, 16px under the header, ContentPadding(10,6)
	// on every button, and the library actions living in the header rather than over the list.
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(CloudProjectsVisual::Background)
		.Padding(20.f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 16.f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[CloudProjectsVisual::Label(LOCTEXT("Title", "Cloud Projects"), 22)]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 5.f, 0.f, 0.f)
					[
						CloudProjectsVisual::Label(LOCTEXT("Subtitle",
							"Package this Unreal project and upload it to Convai for browser streaming."), 10, true)
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f)
				[
					SNew(SButton)
					.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
					.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
					.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
					.Text(LOCTEXT("Refresh", "Refresh"))
					.IsEnabled_Lambda([this]() { return !State.bBusy && !State.bLoading; })
					.OnClicked_Lambda([this]() { OnRefresh.ExecuteIfBound(); return FReply::Handled(); })
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
					.ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle())
					.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
					.Text(LOCTEXT("NewProject", "+ New project"))
					.ToolTipText(LOCTEXT("NewProjectTip",
						"Package this Unreal project and upload it to Convai as a new application."))
					.IsEnabled_Lambda([this]() { return CanMutate() && !bCreating; })
					.OnClicked(this, &SConvaiCloudProjects::BeginCreate)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				BuildMessages()
			]

			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SSplitter)
				+ SSplitter::Slot().Value(0.42f)
				[
					BuildLibrary()
				]
				+ SSplitter::Slot().Value(0.58f)
				[
					SAssignNew(DetailsHost, SBox)
					.Padding(FMargin(16.f, 0.f, 0.f, 0.f))
					[
						BuildDetails()
					]
				]
			]
		]
	];
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildLibrary()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
		[
			SNew(SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Search projects"))
			.OnTextChanged_Lambda([this](const FText& Text)
			{
				SearchText = Text.ToString();
				RebuildRows();
			})
		]

		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SBorder)
			.BorderImage(&CloudProjectsVisual::Well)
			.Padding(6.f)
			[
				SNew(SOverlay)

				+ SOverlay::Slot()
				[
					SAssignNew(ListView, SListView<TSharedPtr<FConvaiCloudProjectCard>>)
					.ListItemsSource(&Rows)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SConvaiCloudProjects::GenerateRow)
					.OnSelectionChanged(this, &SConvaiCloudProjects::SelectionChanged)
				]

				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SBox)
					.Padding(16.f)
					.Visibility_Lambda([this]()
					{
						return Rows.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
					})
					[
						SNew(STextBlock)
						.Font(CloudProjectsVisual::Font(10))
						.ColorAndOpacity(CloudProjectsVisual::Muted)
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
						.Text_Lambda([this]()
						{
							if (State.bNeedsSignIn) return LOCTEXT("EmptySignIn", "Sign in to Convai to see your uploaded projects.");
							if (State.bLoading) return LOCTEXT("EmptyLoading", "Loading your projects...");
							if (!SearchText.IsEmpty()) return LOCTEXT("EmptySearch", "No project matches that search.");
							return LOCTEXT("EmptyLibrary", "No projects uploaded yet.\nUse + New project to package and upload this one.");
						})
					]
				]
			]
		];
}

TSharedRef<ITableRow> SConvaiCloudProjects::GenerateRow(TSharedPtr<FConvaiCloudProjectCard> Item,
	const TSharedRef<STableViewBase>& Owner)
{
	const FConvaiCloudProjectCard Card = Item.IsValid() ? *Item : FConvaiCloudProjectCard();
	const FLinearColor StatusColor = Card.bFailed
		? CloudProjectsVisual::Failure
		: (Card.bLive ? CloudProjectsVisual::Accent
			: (Card.bWorking ? CloudProjectsVisual::Warning : CloudProjectsVisual::Muted));

	return SNew(STableRow<TSharedPtr<FConvaiCloudProjectCard>>, Owner)
		.Padding(FMargin(0.f, 3.f))
		[
			SNew(SBorder)
			.BorderImage(&CloudProjectsVisual::Row)
			.Padding(FMargin(10.f, 8.f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Card.Name))
						.ColorAndOpacity(ConvaiEditorVisual::PrimaryText())
						.Font(FAppStyle::Get().GetFontStyle(TEXT("NormalFontBold")))
					]

					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Card.Status))
						.ColorAndOpacity(StatusColor)
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(CloudProjectsVisual::Muted)
					.Text(FText::FromString(Card.Version.IsEmpty()
						? TEXT("No version uploaded")
						: FString::Printf(TEXT("Version %s"), *Card.Version)))
				]
			]
		];
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildDetails()
{
	if (bCreating) return BuildCreate();

	if (State.bNeedsSignIn)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
			[CloudProjectsVisual::Label(LOCTEXT("SignInTitle", "Sign in to Convai"), 18)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 16.f)
			[CloudProjectsVisual::Label(LOCTEXT("SignInBody", "Uploading a project needs your Convai account."), 11, true)]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle())
				.ContentPadding(FMargin(12.f, 9.f))
				.Text(LOCTEXT("SignIn", "Sign in"))
				.OnClicked_Lambda([this]() { OnSignIn.ExecuteIfBound(); return FReply::Handled(); })
			];
	}

	const FConvaiCloudProjectCard* Card = SelectedCard();
	if (!Card)
	{
		return SNew(SBox).VAlign(VAlign_Center).HAlign(HAlign_Center).Padding(24.f)
			[
				CloudProjectsVisual::Label(LOCTEXT("NoSelection",
					"Select a project to update it, or create a new one to upload this Unreal project for the first time."),
					11, true)
			];
	}

	// Eyebrow, title, one status line. Sections below are separated by space, not by nested
	// panels -- the same flat rhythm Cloud Avatars uses for its details column.
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
			[CloudProjectsVisual::Label(LOCTEXT("ProjectEyebrow", "CLOUD PROJECT"), 10, true, true)]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
			[CloudProjectsVisual::Label(FText::FromString(Card->Name), 20)]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 20.f)
			[
				SNew(STextBlock)
				.Font(CloudProjectsVisual::Font(10))
				.ColorAndOpacity(Card->bFailed ? CloudProjectsVisual::Failure
					: (Card->bLive ? CloudProjectsVisual::Accent : CloudProjectsVisual::Muted))
				.Text(FText::FromString(Card->Version.IsEmpty()
					? Card->Status
					: FString::Printf(TEXT("%s  -  version %s"), *Card->Status, *Card->Version)))
			]

			+ SVerticalBox::Slot().AutoHeight()[BuildActions()]
			// Progress sits directly under the actions that start it, so the eye does not
			// travel to the far corner of the window to find out what a click is doing.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 0.f)[BuildJobBanner()]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 24.f, 0.f, 0.f)[BuildVersions()]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 24.f, 0.f, 0.f)[BuildLogs()]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 24.f, 0.f, 0.f)[BuildSettings()]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 24.f, 0.f, 12.f)[BuildPublish()]
		];
}

TSharedRef<SWidget> SConvaiCloudProjects::SectionHeading(const FText& Title)
{
	return SNew(SBox).Padding(FMargin(0.f, 0.f, 0.f, 8.f))
		[CloudProjectsVisual::Label(Title, 10, true, true)];
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildActions()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
				.Text(LOCTEXT("UploadChanges", "Upload changes"))
				.ToolTipText(LOCTEXT("UploadChangesTip",
					"Packages this Unreal project, uploads it as a new version, and makes it active."))
				.IsEnabled_Lambda([this]() { return CanMutate(); })
				.OnClicked(this, &SConvaiCloudProjects::UploadSelected)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(10.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
				.Text(LOCTEXT("OpenInBrowser", "Open in browser"))
				.ToolTipText(LOCTEXT("OpenInBrowserTip", "Starts a stream session and opens the Convai player."))
				.IsEnabled_Lambda([this]()
				{
					const FConvaiCloudProjectCard* Selected = SelectedCard();
					return !State.bBusy && Selected && Selected->bLive;
				})
				.OnClicked(this, &SConvaiCloudProjects::OpenSelected)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SNew(SBox)
			.Visibility(State.NameMismatchWarning.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
			[
				SNew(STextBlock).Text(FText::FromString(State.NameMismatchWarning))
				.Font(CloudProjectsVisual::Font(9)).ColorAndOpacity(CloudProjectsVisual::Warning).AutoWrapText(true)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SNew(SBox)
			.Visibility(State.PackagingDisabledReason.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
			[
				SNew(STextBlock).Text(FText::FromString(State.PackagingDisabledReason))
				.Font(CloudProjectsVisual::Font(9)).ColorAndOpacity(CloudProjectsVisual::Failure).AutoWrapText(true)
			]
		];
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildVersions()
{
	TSharedRef<SVerticalBox> VersionRows = SNew(SVerticalBox);

	if (State.Versions.IsEmpty())
	{
		VersionRows->AddSlot().AutoHeight()
		[CloudProjectsVisual::Label(LOCTEXT("NoVersions", "No versions uploaded yet."), 10, true)];
	}

	for (const FConvaiProjectVersionRow& VersionRow : State.Versions)
	{
		const FString BuildId = VersionRow.BuildId;
		const FLinearColor StatusColor = VersionRow.bActive
			? CloudProjectsVisual::Accent
			: (VersionRow.Status == TEXT("Build failed") ? CloudProjectsVisual::Failure : CloudProjectsVisual::Muted);

		VersionRows->AddSlot().AutoHeight().Padding(0.f, 2.f)
		[
			SNew(SBorder)
			.BorderImage(VersionRow.bActive ? &CloudProjectsVisual::SelectedRow : &CloudProjectsVisual::Row)
			.Padding(FMargin(10.f, 7.f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[CloudProjectsVisual::Label(FText::FromString(VersionRow.Version), 12)]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString([&VersionRow]()
						{
							// Build state first, then the rollout state when it adds anything.
							FString Line = VersionRow.Status;
							if (!VersionRow.Size.IsEmpty()) Line += FString::Printf(TEXT("  -  %s"), *VersionRow.Size);
							if (!VersionRow.Availability.IsEmpty() && VersionRow.Availability != TEXT("Not active"))
								Line += FString::Printf(TEXT("  -  %s"), *VersionRow.Availability);
							return Line;
						}()))
						.Font(CloudProjectsVisual::Font(9)).ColorAndOpacity(StatusColor)
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
					.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
					.ContentPadding(FMargin(8.f, 5.f))
					.Text(FText::FromString(VersionRow.ActivateLabel))
					.ToolTipText(LOCTEXT("MakeActiveTip", "Serve this version to everyone opening the experience."))
					.Visibility(VersionRow.bCanActivate ? EVisibility::Visible : EVisibility::Collapsed)
					.IsEnabled_Lambda([this, bEnabled = VersionRow.bActivateEnabled]() { return !State.bBusy && bEnabled; })
					.OnClicked_Lambda([this, BuildId]()
					{
						OnActivateVersion.ExecuteIfBound(BuildId);
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
					.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
					.ContentPadding(FMargin(8.f, 5.f))
					.Text(LOCTEXT("ShowLogs", "Logs"))
					.ToolTipText(LOCTEXT("ShowLogsTip", "Show the cloud build log for this version."))
					.Visibility(VersionRow.bHasLogs ? EVisibility::Visible : EVisibility::Collapsed)
					.OnClicked_Lambda([this, BuildId]()
					{
						OnShowLogs.ExecuteIfBound(BuildId);
						return FReply::Handled();
					})
				]
			]
		];
	}

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[SectionHeading(LOCTEXT("VersionsTitle", "VERSIONS"))]
		+ SVerticalBox::Slot().AutoHeight()[VersionRows];
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildLogs()
{
	return SNew(SBox)
		.Visibility_Lambda([this]()
		{
			return State.LogVersion.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
		})
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(CloudProjectsVisual::Font(10, true))
					.ColorAndOpacity(CloudProjectsVisual::Muted)
					.Text_Lambda([this]()
					{
						return FText::FromString(FString::Printf(TEXT("BUILD LOG  -  %s"), *State.LogVersion));
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
					.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
					.ContentPadding(FMargin(8.f, 4.f))
					.Text(LOCTEXT("CloseLogs", "Close"))
					.OnClicked_Lambda([this]()
					{
						// Local dismissal only; the next Logs press refetches from the server.
						State.LogVersion.Reset();
						State.LogLines.Reset();
						if (DetailsHost.IsValid()) DetailsHost->SetContent(BuildDetails());
						return FReply::Handled();
					})
				]
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(&CloudProjectsVisual::Well)
				.Padding(8.f)
				[
					SNew(SBox).MaxDesiredHeight(200.f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(STextBlock)
							.ColorAndOpacity(CloudProjectsVisual::Muted)
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
							.Text_Lambda([this]()
							{
								if (State.bLogsLoading) return LOCTEXT("LogsLoading", "Loading the build log...");
								if (State.LogLines.IsEmpty())
									return LOCTEXT("LogsEmpty", "The cloud recorded no log lines for this build.");
								return FText::FromString(FString::Join(State.LogLines, TEXT("\n")));
							})
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildPublish()
{
	// Audience choices as the dashboard offers them. Draft is a state, never a choice.
	static const TArray<TPair<FString, FText>> Audiences = {
		{ TEXT("public"),   LOCTEXT("AudiencePublic", "Public") },
		{ TEXT("unlisted"), LOCTEXT("AudienceUnlisted", "Unlisted") },
		{ TEXT("private"),  LOCTEXT("AudiencePrivate", "Private") },
	};

	if (State.Publish.ExperienceId.IsEmpty())
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[SectionHeading(LOCTEXT("PublishTitle", "PUBLISH"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				CloudProjectsVisual::Label(LOCTEXT("PublishNoPage",
					"No experience page yet. Use Open in browser once a version is active; that creates one."), 10, true)
			];
	}

	TSharedRef<SHorizontalBox> Choices = SNew(SHorizontalBox);
	for (const TPair<FString, FText>& Audience : Audiences)
	{
		const FString Value = Audience.Key;
		Choices->AddSlot().AutoWidth().Padding(0.f, 0.f, 16.f, 0.f)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this, Value]()
			{
				return PublishDraft.Visibility == Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, Value](ECheckBoxState NewState)
			{
				if (NewState == ECheckBoxState::Checked) PublishDraft.Visibility = Value;
			})
			[
				SNew(SBox).Padding(FMargin(4.f, 0.f, 0.f, 0.f))
				[CloudProjectsVisual::Label(Audience.Value, 11)]
			]
		];
	}

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()[SectionHeading(LOCTEXT("PublishTitle", "PUBLISH"))]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SEditableTextBox)
			.Font(CloudProjectsVisual::Font(11))
			.Text_Lambda([this]() { return FText::FromString(PublishDraft.Name); })
			.HintText(LOCTEXT("PublishName", "Page name"))
			.OnTextChanged_Lambda([this](const FText& Text) { PublishDraft.Name = Text.ToString(); })
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SEditableTextBox)
			.Font(CloudProjectsVisual::Font(11))
			.Text_Lambda([this]() { return FText::FromString(PublishDraft.Description); })
			.HintText(LOCTEXT("PublishDescription", "Short description"))
			.OnTextChanged_Lambda([this](const FText& Text) { PublishDraft.Description = Text.ToString(); })
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)[Choices]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Font(CloudProjectsVisual::Font(9))
			.ColorAndOpacity(CloudProjectsVisual::Muted)
			.Text_Lambda([this]()
			{
				// What is live now, kept distinct from the radio selection, which is only a proposal.
				return FText::FromString(FString::Printf(TEXT("Live now: %s"), *State.Publish.Visibility));
			})
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
				.Text(LOCTEXT("PublishNow", "Publish"))
				.ToolTipText(LOCTEXT("PublishNowTip", "Make the page live at the selected audience."))
				.IsEnabled_Lambda([this]() { return !State.bBusy && !PublishDraft.Name.TrimStartAndEnd().IsEmpty(); })
				.OnClicked_Lambda([this]() { OnPublish.ExecuteIfBound(PublishDraft, true); return FReply::Handled(); })
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(10.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
				.Text(LOCTEXT("SaveDraft", "Save draft"))
				.ToolTipText(LOCTEXT("SaveDraftTip", "Save these details without making the page live."))
				.IsEnabled_Lambda([this]() { return !State.bBusy; })
				.OnClicked_Lambda([this]() { OnPublish.ExecuteIfBound(PublishDraft, false); return FReply::Handled(); })
			]
		];
}

TSharedRef<SWidget> SConvaiCloudProjects::SettingsField(const FText& Label, const FString& ErrorKey,
	TFunction<FString()> Get, TFunction<void(const FString&)> Set, const FText& Hint)
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[CloudProjectsVisual::Label(Label, 10, true)]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(SEditableTextBox)
			.Font(CloudProjectsVisual::Font(11))
			.HintText(Hint)
			.Text_Lambda([Get]() { return FText::FromString(Get()); })
			.OnTextChanged_Lambda([this, Set](const FText& Text)
			{
				Set(Text.ToString());
				// Validate as the user types so a bad field is marked before they press Save.
				SettingsErrors = SettingsDraft.Validate();
			})
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Font(CloudProjectsVisual::Font(9))
			.ColorAndOpacity(CloudProjectsVisual::Failure)
			.AutoWrapText(true)
			.Visibility_Lambda([this, ErrorKey]()
			{
				return SettingsErrors.Contains(ErrorKey) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.Text_Lambda([this, ErrorKey]()
			{
				const FString* Message = SettingsErrors.Find(ErrorKey);
				return Message ? FText::FromString(*Message) : FText::GetEmpty();
			})
		];
}

TSharedRef<SWidget> SConvaiCloudProjects::FrontendChoice(const FString& Key, const FText& Label)
{
	// Index 0 is "runtime default", which stores nothing. The dashboard's wording is reused
	// verbatim so the two tools describe the same three states identically.
	if (FrontendOptions.IsEmpty())
	{
		FrontendOptions.Add(MakeShared<FString>(TEXT("Use runtime default")));
		FrontendOptions.Add(MakeShared<FString>(TEXT("On")));
		FrontendOptions.Add(MakeShared<FString>(TEXT("Off")));
	}

	auto Current = [this, Key]() -> TSharedPtr<FString>
	{
		const TOptional<bool> Value = SettingsDraft.GetFrontendBool(Key);
		if (!Value.IsSet()) return FrontendOptions[0];
		return Value.GetValue() ? FrontendOptions[1] : FrontendOptions[2];
	};

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[CloudProjectsVisual::Label(Label, 10, true)]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&FrontendOptions)
			.InitiallySelectedItem(Current())
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Option)
			{
				return CloudProjectsVisual::Label(FText::FromString(Option.IsValid() ? *Option : FString()), 11);
			})
			.OnSelectionChanged_Lambda([this, Key](TSharedPtr<FString> Option, ESelectInfo::Type)
			{
				if (!Option.IsValid()) return;

				TOptional<bool> Value;
				if (*Option == TEXT("On")) Value = true;
				else if (*Option == TEXT("Off")) Value = false;
				// "Use runtime default" leaves Value unset, which removes the key.

				SettingsDraft.SetFrontendBool(Key, Value);
				SettingsErrors = SettingsDraft.Validate();
				// MatchViewportRes hides the resolution fields, so the form has to redraw.
				if (DetailsHost.IsValid()) DetailsHost->SetContent(BuildDetails());
			})
			[
				SNew(STextBlock)
				.Font(CloudProjectsVisual::Font(11))
				.ColorAndOpacity(ConvaiEditorVisual::PrimaryText())
				.Text_Lambda([this, Key]()
				{
					const TOptional<bool> Value = SettingsDraft.GetFrontendBool(Key);
					if (!Value.IsSet()) return LOCTEXT("FrontendDefault", "Use runtime default");
					return Value.GetValue() ? LOCTEXT("FrontendOn", "On") : LOCTEXT("FrontendOff", "Off");
				})
			]
		];
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildSettings()
{
	const bool bMatchViewport = SettingsDraft.IsFrontendTrue(TEXT("MatchViewportRes"));

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()[SectionHeading(LOCTEXT("SettingsTitle", "RUNTIME SETTINGS"))]

		+ SVerticalBox::Slot().AutoHeight()
		[
			CloudProjectsVisual::Label(LOCTEXT("SettingsBody",
				"Applied when the cloud launches this project. Leave a field empty to use its default."), 10, true)
		]

		// --- browser overrides (ps-frontend-config) -------------------------------------
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
		[CloudProjectsVisual::Label(LOCTEXT("SettingsBrowser", "Browser"), 11, false, true)]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[FrontendChoice(TEXT("UseMic"), LOCTEXT("UseMic", "UseMic"))]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(10.f, 0.f, 0.f, 0.f)
			[FrontendChoice(TEXT("HoveringMouse"), LOCTEXT("HoveringMouse", "HoveringMouse"))]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[FrontendChoice(TEXT("MatchViewportRes"), LOCTEXT("MatchViewportRes", "MatchViewportRes"))]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(10.f, 0.f, 0.f, 0.f)
			[FrontendChoice(TEXT("TimeoutIfIdle"), LOCTEXT("TimeoutIfIdle", "TimeoutIfIdle"))]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			CloudProjectsVisual::Label(LOCTEXT("FrontendDefaultHint",
				"Use runtime default omits the setting entirely, which is not the same as Off."), 9, true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SettingsField(LOCTEXT("AfkTimeout", "AFK timeout (seconds)"), TEXT("frontend_json"),
				[this]()
				{
					const int32 Value = SettingsDraft.GetAfkTimeout();
					return Value < 0 ? FString() : FString::FromInt(Value);
				},
				[this](const FString& Value) { SettingsDraft.SetAfkTimeout(Value); },
				LOCTEXT("AfkTimeoutHint", "Default"))
		]

		// --- Unreal launch settings (ps-ue-config) --------------------------------------
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 0.f)
		[CloudProjectsVisual::Label(LOCTEXT("SettingsUnreal", "Unreal"), 11, false, true)]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SBox)
			// An explicit resolution is meaningless while the stream matches the viewport.
			.Visibility(bMatchViewport ? EVisibility::Collapsed : EVisibility::Visible)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SettingsField(LOCTEXT("ResolutionWidth", "Width"), TEXT("resolutionWidth"),
						[this]() { return SettingsDraft.ResolutionWidth; },
						[this](const FString& Value) { SettingsDraft.ResolutionWidth = Value; },
						LOCTEXT("ResolutionWidthHint", "1920"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(10.f, 0.f, 0.f, 0.f)
				[
					SettingsField(LOCTEXT("ResolutionHeight", "Height"), TEXT("resolutionHeight"),
						[this]() { return SettingsDraft.ResolutionHeight; },
						[this](const FString& Value) { SettingsDraft.ResolutionHeight = Value; },
						LOCTEXT("ResolutionHeightHint", "1080"))
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SettingsField(LOCTEXT("ScreenPercentage", "Screen percentage"), TEXT("screenPercentage"),
				[this]() { return SettingsDraft.ScreenPercentage; },
				[this](const FString& Value) { SettingsDraft.ScreenPercentage = Value; },
				LOCTEXT("ScreenPercentageHint", "100"))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SettingsField(LOCTEXT("ExtraArgs", "Extra arguments"), TEXT("extraArgs"),
				[this]() { return SettingsDraft.ExtraArgs; },
				[this](const FString& Value) { SettingsDraft.ExtraArgs = Value; },
				LOCTEXT("ExtraArgsHint", "-nosound -ResX=1920"))
		]

		// --- advanced: the raw ps-frontend-config object --------------------------------
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
				.ContentPadding(FMargin(10.f, 6.f))
				.Text_Lambda([this]()
				{
					return bShowAdvancedJson ? LOCTEXT("HideAdvanced", "Hide advanced") : LOCTEXT("ShowAdvanced", "Advanced");
				})
				.OnClicked_Lambda([this]()
				{
					bShowAdvancedJson = !bShowAdvancedJson;
					if (DetailsHost.IsValid()) DetailsHost->SetContent(BuildDetails());
					return FReply::Handled();
				})
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SBox)
			.Visibility(bShowAdvancedJson ? EVisibility::Visible : EVisibility::Collapsed)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]()
					{
						return SettingsDraft.GetDisableVpxCompute() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
					{
						SettingsDraft.SetDisableVpxCompute(NewState == ECheckBoxState::Checked);
						SettingsErrors = SettingsDraft.Validate();
						// It writes into Extra arguments, so that field has to redraw with it.
						if (DetailsHost.IsValid()) DetailsHost->SetContent(BuildDetails());
					})
					[
						SNew(SBox).Padding(FMargin(4.f, 0.f, 0.f, 0.f))
						[CloudProjectsVisual::Label(LOCTEXT("DisableVpx", "Disable VPX compute shader"), 11)]
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(22.f, 2.f, 0.f, 0.f)
				[
					CloudProjectsVisual::Label(LOCTEXT("DisableVpxBody",
						"Encodes VP8 on the CPU instead of the GPU. Worth trying when the GPU is saturated "
						"and the CPU has headroom. Writes -PixelStreamingVPXUseCompute=false into Extra arguments."),
						9, true)
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
				[
					CloudProjectsVisual::Label(LOCTEXT("AdvancedBody",
						"Browser settings as a raw object. Any key is allowed; values must be true/false, "
						"a number, or text. The toggles above edit this same object."), 9, true)
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SBorder)
					.BorderImage(&CloudProjectsVisual::Well)
					.Padding(4.f)
					[
						SNew(SBox).MinDesiredHeight(90.f)
						[
							SNew(SMultiLineEditableTextBox)
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
							.Text_Lambda([this]() { return FText::FromString(SettingsDraft.FrontendJson); })
							.OnTextChanged_Lambda([this](const FText& Text)
							{
								SettingsDraft.SetFrontendJson(Text.ToString());
								SettingsErrors = SettingsDraft.Validate();
							})
						]
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Font(CloudProjectsVisual::Font(9))
					.ColorAndOpacity(CloudProjectsVisual::Failure)
					.AutoWrapText(true)
					.Visibility_Lambda([this]()
					{
						return SettingsErrors.Contains(TEXT("frontend_json")) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Text_Lambda([this]()
					{
						const FString* Message = SettingsErrors.Find(TEXT("frontend_json"));
						return Message ? FText::FromString(*Message) : FText::GetEmpty();
					})
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
				.Text(LOCTEXT("SaveAndRedeploy", "Save and redeploy"))
				.ToolTipText(LOCTEXT("SaveAndRedeployTip",
					"Saves these settings and restarts the running build with them. No repackage, no upload."))
				.IsEnabled_Lambda([this]()
				{
					return !State.bBusy && SettingsErrors.IsEmpty() && !ActiveBuildId().IsEmpty();
				})
				.OnClicked_Lambda([this]()
				{
					// Re-sending the build that is already active is what asks for a rollout: the
					// server compares the runtime_config hash and queues one when it differs.
					OnSaveSettings.ExecuteIfBound(SettingsDraft, ActiveBuildId());
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(10.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
				.Text(LOCTEXT("SaveSettings", "Save only"))
				.ToolTipText(LOCTEXT("SaveSettingsTip",
					"Stores these settings without restarting. They apply at the next deployment."))
				.IsEnabled_Lambda([this]() { return !State.bBusy && SettingsErrors.IsEmpty(); })
				.OnClicked_Lambda([this]()
				{
					OnSaveSettings.ExecuteIfBound(SettingsDraft, FString());
					return FReply::Handled();
				})
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Font(CloudProjectsVisual::Font(9))
			.ColorAndOpacity(CloudProjectsVisual::Muted)
			.AutoWrapText(true)
			.Visibility_Lambda([this]() { return ActiveBuildId().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
			.Text(LOCTEXT("NoActiveBuildForRedeploy",
				"Redeploy needs an active build. Make a version active first."))
		];
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildCreate()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()[CloudProjectsVisual::Label(LOCTEXT("CreateTitle", "Upload this project"), 18)]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 10.f)
		[
			CloudProjectsVisual::Label(LOCTEXT("CreateBody",
				"Convai enables Pixel Streaming, packages this project, uploads it, and builds it for streaming. "
				"The first package can take a while."))
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SectionHeading(LOCTEXT("CreateNameLabel", "PROJECT NAME"))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
		[
			SAssignNew(NameBox, SEditableTextBox)
			.Text(FText::FromString(DraftName))
			.HintText(LOCTEXT("CreateNameHint", "Name shown in your Convai dashboard"))
			.OnTextChanged_Lambda([this](const FText& Text) { DraftName = Text.ToString(); })
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(CloudProjectsVisual::Failure)
			.Text(this, &SConvaiCloudProjects::CreateValidation)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::PrimaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
				.Text(LOCTEXT("CreateAndUpload", "Create and upload"))
				.IsEnabled_Lambda([this]() { return CanMutate() && CreateValidation().IsEmpty(); })
				.OnClicked(this, &SConvaiCloudProjects::SubmitCreate)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
				.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
				.HAlign(HAlign_Center).VAlign(VAlign_Center).ContentPadding(FMargin(10.f, 6.f))
				.Text(LOCTEXT("CreateCancel", "Cancel"))
				.IsEnabled_Lambda([this]() { return !State.bBusy; })
				.OnClicked(this, &SConvaiCloudProjects::CancelCreate)
			]
		];
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildJobBanner()
{
	return SNew(SBox)
		.Visibility_Lambda([this]() { return State.JobTitle.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		[
			SNew(SBorder)
			.BorderImage(&CloudProjectsVisual::Well)
			.Padding(FMargin(12.f, 10.f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(CloudProjectsVisual::Font(12, true))
						.ColorAndOpacity(ConvaiEditorVisual::PrimaryText())
						.Text_Lambda([this]() { return FText::FromString(State.JobTitle); })
					]

					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SButton)
						.TextStyle(&CloudProjectsVisual::ButtonTextStyle())
						.ButtonStyle(&ConvaiEditorVisual::SecondaryButtonStyle())
						.ContentPadding(FMargin(8.f, 4.f))
						.Text(LOCTEXT("CancelJob", "Cancel"))
						.Visibility_Lambda([this]() { return State.bCanCancel ? EVisibility::Visible : EVisibility::Collapsed; })
						.OnClicked_Lambda([this]() { OnCancelJob.ExecuteIfBound(); return FReply::Handled(); })
					]
				]

				// One line instead of a row of chips: which step, out of how many, and both clocks.
				// The strip looked busier than the work it described.
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 3.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Font(CloudProjectsVisual::Font(9))
					.ColorAndOpacity(CloudProjectsVisual::Muted)
					.Text_Lambda([this]()
					{
						TArray<FString> Parts;

						const int32 Index = StageIndex(State.Stage);
						if (Index != INDEX_NONE)
						{
							Parts.Add(FString::Printf(TEXT("Step %d of %d"), Index + 1, Stages().Num()));
						}

						const FString Stage = Elapsed(State.StageStartedAt);
						if (!Stage.IsEmpty()) Parts.Add(Stage);

						const FString Total = Elapsed(State.JobStartedAt);
						if (!Total.IsEmpty() && Total != Stage) Parts.Add(Total + TEXT(" total"));

						return FText::FromString(FString::Join(Parts, TEXT("  -  ")));
					})
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
				[
					SNew(SBox).HeightOverride(4.f)
					[
						SNew(SProgressBar)
						.Percent_Lambda([this]() { return State.JobProgress; })
						.FillColorAndOpacity(CloudProjectsVisual::Accent)
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Font(CloudProjectsVisual::Font(9))
					.ColorAndOpacity(CloudProjectsVisual::Muted)
					.AutoWrapText(true)
					.Visibility_Lambda([this]() { return State.JobDetail.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.Text_Lambda([this]() { return FText::FromString(State.JobDetail); })
				]
			]
		];
}

void SConvaiCloudProjects::Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);

	// The elapsed clocks are computed in attribute lambdas, so the banner has to keep repainting
	// while a job runs. Nothing is invalidated once it finishes.
	if (State.JobStartedAt > 0.0) Invalidate(EInvalidateWidgetReason::Paint);
}

TSharedRef<SWidget> SConvaiCloudProjects::BuildMessages()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(CloudProjectsVisual::Failure)
			.Visibility_Lambda([this]() { return State.Error.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this]() { return FText::FromString(State.Error); })
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(CloudProjectsVisual::Accent)
			.Visibility_Lambda([this]() { return State.Notice.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this]() { return FText::FromString(State.Notice); })
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(CloudProjectsVisual::Warning)
			.Visibility_Lambda([this]() { return State.PrepareNotes.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this]() { return FText::FromString(FString::Join(State.PrepareNotes, TEXT("\n"))); })
		];
}

void SConvaiCloudProjects::SetState(const FConvaiCloudProjectsViewState& InState)
{
	check(IsInGameThread());
	const bool bSelectionChanged = State.SelectedId != InState.SelectedId;
	State = InState;

	// A create draft is the user's unsaved work; a background refresh must not discard it.
	if (bCreating && DraftName.IsEmpty()) DraftName = State.LocalProjectName;

	// Seed the publish form from the server once per project. Re-seeding on every refresh would
	// wipe whatever the user is typing while a build polls in the background.
	if (SettingsDraftFor != State.SelectedId)
	{
		SettingsDraft = State.Settings;
		SettingsErrors = SettingsDraft.Validate();
		SettingsDraftFor = State.SelectedId;
	}

	if (PublishDraftFor != State.SelectedId || (PublishDraft.ExperienceId != State.Publish.ExperienceId))
	{
		PublishDraft = State.Publish;
		if (PublishDraft.Visibility == TEXT("draft")) PublishDraft.Visibility = TEXT("unlisted");
		PublishDraftFor = State.SelectedId;
	}

	RebuildRows();

	if (ListView.IsValid())
	{
		TGuardValue<bool> Guard(bUpdatingSelection, true);
		const TSharedPtr<FConvaiCloudProjectCard>* Match = Rows.FindByPredicate(
			[this](const TSharedPtr<FConvaiCloudProjectCard>& Row) { return Row.IsValid() && Row->Id == State.SelectedId; });
		if (Match) ListView->SetSelection(*Match, ESelectInfo::Direct);
		else if (bSelectionChanged) ListView->ClearSelection();
	}

	if (DetailsHost.IsValid()) DetailsHost->SetContent(BuildDetails());
}

void SConvaiCloudProjects::RebuildRows()
{
	Rows.Reset();
	for (const FConvaiCloudProjectCard& Card : State.Projects)
	{
		if (!SearchText.IsEmpty() && !Card.Name.Contains(SearchText)) continue;
		Rows.Add(MakeShared<FConvaiCloudProjectCard>(Card));
	}
	if (ListView.IsValid()) ListView->RequestListRefresh();
}

void SConvaiCloudProjects::SelectionChanged(TSharedPtr<FConvaiCloudProjectCard> Item, ESelectInfo::Type SelectInfo)
{
	if (bUpdatingSelection || SelectInfo == ESelectInfo::Direct) return;
	if (bCreating) bCreating = false;
	State.SelectedId = Item.IsValid() ? Item->Id : FString();
	OnSelect.ExecuteIfBound(State.SelectedId);
	if (DetailsHost.IsValid()) DetailsHost->SetContent(BuildDetails());
}

const FConvaiCloudProjectCard* SConvaiCloudProjects::SelectedCard() const
{
	return State.Projects.FindByPredicate(
		[this](const FConvaiCloudProjectCard& Card) { return Card.Id == State.SelectedId; });
}

FString SConvaiCloudProjects::ActiveBuildId() const
{
	const FConvaiProjectVersionRow* Active = State.Versions.FindByPredicate(
		[](const FConvaiProjectVersionRow& Row) { return Row.bActive; });
	return Active ? Active->BuildId : FString();
}

bool SConvaiCloudProjects::CanMutate() const
{
	return !State.bBusy && !State.bNeedsSignIn && State.PackagingDisabledReason.IsEmpty();
}

FText SConvaiCloudProjects::CreateValidation() const
{
	const FString Trimmed = DraftName.TrimStartAndEnd();
	if (Trimmed.IsEmpty()) return LOCTEXT("NameRequired", "Enter a name for this project.");
	if (Trimmed.Len() > 100) return LOCTEXT("NameTooLong", "Use 100 characters or fewer.");
	return FText::GetEmpty();
}

FReply SConvaiCloudProjects::BeginCreate()
{
	bCreating = true;
	DraftName = State.LocalProjectName;
	if (DetailsHost.IsValid()) DetailsHost->SetContent(BuildDetails());
	return FReply::Handled();
}

FReply SConvaiCloudProjects::CancelCreate()
{
	bCreating = false;
	DraftName.Reset();
	if (DetailsHost.IsValid()) DetailsHost->SetContent(BuildDetails());
	return FReply::Handled();
}

FReply SConvaiCloudProjects::SubmitCreate()
{
	if (!CanMutate() || !CreateValidation().IsEmpty()) return FReply::Handled();
	bCreating = false;
	OnCreate.ExecuteIfBound(DraftName.TrimStartAndEnd());
	if (DetailsHost.IsValid()) DetailsHost->SetContent(BuildDetails());
	return FReply::Handled();
}

FReply SConvaiCloudProjects::UploadSelected()
{
	if (CanMutate() && !State.SelectedId.IsEmpty()) OnUploadChanges.ExecuteIfBound(State.SelectedId);
	return FReply::Handled();
}

FReply SConvaiCloudProjects::OpenSelected()
{
	if (!State.bBusy && !State.SelectedId.IsEmpty()) OnOpenInBrowser.ExecuteIfBound(State.SelectedId);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
