#include "ExposureProbeDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailGroup.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "ExposureProbeActor.h"
#include "ExposureProbeSettings.h"
#include "ExposureProbeSubsystem.h"
#include "ExposureProbeText.h"
#include "Framework/Docking/TabManager.h"
#include "ObjectEditorUtils.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace ExposureProbeDetails
{
	static TSharedRef<SWidget> SmallButton(const FText& Label, const FText& Tip, TFunction<void()> OnClick)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(Tip)
			.ContentPadding(FMargin(6, 1))
			.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(Label).Font(IDetailLayoutBuilder::GetDetailFont())
			];
	}

	static const FExposureEdge* FindEdge(const FString& A, const FString& B)
	{
		const UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
		if (!S)
		{
			return nullptr;
		}
		return S->GetEdges().FindByPredicate([&](const FExposureEdge& E)
		{
			return (E.TProbe == A && E.CTProbe == B) || (E.TProbe == B && E.CTProbe == A);
		});
	}
}

TSharedRef<IDetailCustomization> FExposureProbeDetails::MakeInstance()
{
	return MakeShared<FExposureProbeDetails>();
}

FExposureProbeDetails::~FExposureProbeDetails()
{
	if (UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get())
	{
		S->OnReportUpdated.Remove(ReportHandle);
	}
}

void FExposureProbeDetails::CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& Builder)
{
	WeakBuilder = Builder;
	CustomizeDetails(*Builder);
}

TArray<FString> FExposureProbeDetails::CollectPartners() const
{
	TSet<FString> Out;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return TArray<FString>();
	}
	for (TActorIterator<AExposureProbeActor> It(World); It; ++It)
	{
		for (const TWeakObjectPtr<AExposureProbeActor>& P : Probes)
		{
			if (P.IsValid() && IsValid(*It) && It->Team != P->Team) { Out.Add(It->GetProbeName()); break; }
		}
	}
	TArray<FString> Sorted = Out.Array();
	Sorted.Sort();
	return Sorted;
}

AExposureProbeActor* FExposureProbeDetails::FindPartner(const FString& Partner) const
{
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		for (TActorIterator<AExposureProbeActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetProbeName() == Partner) { return *It; }
		}
	}
	return nullptr;
}

TArray<AExposureProbeActor*> FExposureProbeDetails::Owners(const FString& Partner) const
{
	TArray<AExposureProbeActor*> Out;
	const AExposureProbeActor* Other = FindPartner(Partner);
	for (const TWeakObjectPtr<AExposureProbeActor>& P : Probes)
	{
		if (P.IsValid() && (!Other || Other->Team != P->Team)) { Out.Add(P.Get()); }
	}
	return Out;
}

void FExposureProbeDetails::SetShown(const TArray<FString>& InPartners, bool bShown)
{
	FProperty* Prop = FindFProperty<FProperty>(AExposureProbeActor::StaticClass(), GET_MEMBER_NAME_CHECKED(AExposureProbeActor, HiddenLinks));
	const FScopedTransaction Transaction(bShown ? EPT("显示连线", "Show Links") : EPT("隐藏连线", "Hide Links"));
	TSet<AExposureProbeActor*> Touched;
	for (const FString& Partner : InPartners)
	{
		for (AExposureProbeActor* P : Owners(Partner))
		{
			if (!Touched.Contains(P))
			{
				P->Modify();
				Touched.Add(P);
			}
			if (bShown) { P->HiddenLinks.Remove(Partner); }
			else { P->HiddenLinks.AddUnique(Partner); }
		}
		// Showing a link also clears a hide set from the partner's side.
		if (AExposureProbeActor* Other = bShown ? FindPartner(Partner) : nullptr)
		{
			for (const AExposureProbeActor* P : Owners(Partner))
			{
				if (Other->HiddenLinks.Contains(P->GetProbeName()))
				{
					if (!Touched.Contains(Other)) { Other->Modify(); Touched.Add(Other); }
					Other->HiddenLinks.Remove(P->GetProbeName());
				}
			}
		}
	}
	for (AExposureProbeActor* P : Touched)
	{
		FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
		P->PostEditChangeProperty(Event);
	}
}

void FExposureProbeDetails::HandleReport()
{
	if (CollectPartners() == Partners || !GEditor)
	{
		return;
	}
	GEditor->GetTimerManager()->SetTimerForNextTick([Weak = WeakBuilder]()
	{
		if (TSharedPtr<IDetailLayoutBuilder> B = Weak.Pin()) { B->ForceRefreshDetails(); }
	});
}

void FExposureProbeDetails::CustomizeDetails(IDetailLayoutBuilder& Builder)
{
	using namespace ExposureProbeDetails;
	Probes.Reset();
	for (const TWeakObjectPtr<UObject>& O : Builder.GetSelectedObjects())
	{
		if (AExposureProbeActor* P = Cast<AExposureProbeActor>(O.Get())) { Probes.Add(P); }
	}
	Partners = CollectPartners();

	UClass* Cls = AExposureProbeActor::StaticClass();
	Builder.HideProperty(GET_MEMBER_NAME_CHECKED(AExposureProbeActor, HiddenLinks), Cls);
	Builder.HideProperty(FName(TEXT("Body")), Cls);

	const FName ProbeCategory = FObjectEditorUtils::GetCategoryFName(FindFProperty<FProperty>(Cls, GET_MEMBER_NAME_CHECKED(AExposureProbeActor, ProbeName)));
	Builder.EditCategory(ProbeCategory, FText::GetEmpty(), ECategoryPriority::Important).SetSortOrder(0);

	// Links to the other side: A, both shares, and whether the link is drawn.
	IDetailCategoryBuilder& Links = Builder.EditCategory(TEXT("ExposureProbeLinks"), EPT("连线", "Links"), ECategoryPriority::Important);
	Links.HeaderContent(
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SmallButton(EPT("全部显示", "Show All"), EPT("显示与所有对方探针的连线", "Show links to every probe of the other side"), [this]() { SetShown(Partners, true); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SmallButton(EPT("全部隐藏", "Hide All"), EPT("隐藏与所有对方探针的连线；审计仍然覆盖它们", "Hide every link; the audit still covers them"), [this]() { SetShown(Partners, false); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(EPT("打开暴露探针面板", "Open the Exposure Probe panel"))
			.ContentPadding(FMargin(4, 1))
			.OnClicked_Lambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TEXT("ExposureProbe"))); return FReply::Handled(); })
			[
				SNew(SImage).Image(FAppStyle::GetBrush("LevelEditor.Tabs.Viewports")).ColorAndOpacity(FSlateColor::UseForeground())
			]
		]);

	const bool bSingle = Probes.Num() == 1;
	for (const FString& Partner : Partners)
	{
		const FText Label = FText::FromString(Partner);
		Links.AddCustomRow(Label)
		.NameContent()
		[
			SNew(SCheckBox)
			.ToolTipText(EPT("在视口中显示这条连线", "Draw this link in the viewport"))
			.IsChecked_Lambda([this, Partner]()
			{
				int32 Shown = 0, Total = 0;
				const AExposureProbeActor* Other = FindPartner(Partner);
				for (const AExposureProbeActor* P : Owners(Partner))
				{
					++Total;
					Shown += P->IsLinkShown(Partner) && (!Other || Other->IsLinkShown(P->GetProbeName())) ? 1 : 0;
				}
				return Shown == Total ? ECheckBoxState::Checked : Shown == 0 ? ECheckBoxState::Unchecked : ECheckBoxState::Undetermined;
			})
			.OnCheckStateChanged_Lambda([this, Partner](ECheckBoxState St) { SetShown({ Partner }, St == ECheckBoxState::Checked); })
			[
				SNew(STextBlock).Text(Label).Font(IDetailLayoutBuilder::GetDetailFontBold())
			]
		]
		.ValueContent()
		.MinDesiredWidth(220.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFontBold())
				.MinDesiredWidth(52.f)
				.ToolTipText(EPT("A = T 看见 CT 的身体比例 − CT 看见 T 的身体比例；正值 T 占优，负值 CT 占优",
					"A = share of CT's body visible to T minus share of T's body visible to CT; positive favours T"))
				.Text_Lambda([this, Partner, bSingle]()
				{
					const FExposureEdge* E = bSingle && Probes[0].IsValid() ? FindEdge(Probes[0]->GetProbeName(), Partner) : nullptr;
					return E ? FText::FromString(FString::Printf(TEXT("A %+.2f"), E->A)) : FText::FromString(TEXT("–"));
				})
				.ColorAndOpacity_Lambda([this, Partner, bSingle]()
				{
					const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
					const FExposureEdge* E = bSingle && Probes[0].IsValid() ? FindEdge(Probes[0]->GetProbeName(), Partner) : nullptr;
					if (!E || FMath::Abs(E->A) < 0.005f) { return FSlateColor::UseSubduedForeground(); }
					return FSlateColor(E->A > 0 ? S->TColor : S->CTColor);
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity_Lambda([this, Partner, bSingle]()
				{
					const FExposureEdge* E = bSingle && Probes[0].IsValid() ? FindEdge(Probes[0]->GetProbeName(), Partner) : nullptr;
					return E && E->bOneWay && !E->bDesignIntent ? FSlateColor(UExposureProbeSettings::AlertColor()) : FSlateColor::UseSubduedForeground();
				})
				.Text_Lambda([this, Partner, bSingle]()
				{
					const FExposureEdge* E = bSingle && Probes[0].IsValid() ? FindEdge(Probes[0]->GetProbeName(), Partner) : nullptr;
					if (!E) { return FText::GetEmpty(); }
					FString Str = FString::Printf(TEXT("T %d%%  ·  CT %d%%"), FMath::RoundToInt(E->ShareTtoCT * 100.f), FMath::RoundToInt(E->ShareCTtoT * 100.f));
					if (E->bOneWay) { Str += E->bDesignIntent ? EPS("  单向 · 保留", "  one-way · intent") : EPS("  单向", "  one-way"); }
					return FText::FromString(Str);
				})
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(EPT("在视口中只看这一对，并标出头、躯干、腿的可见比例", "Show only this pair, with the visible share of head, torso and legs"))
				.ContentPadding(FMargin(3, 1))
				.Visibility(bSingle ? EVisibility::Visible : EVisibility::Collapsed)
				.OnClicked_Lambda([this, Partner]()
				{
					UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get();
					const FExposureEdge* E = S && Probes.Num() == 1 && Probes[0].IsValid() ? FindEdge(Probes[0]->GetProbeName(), Partner) : nullptr;
					if (E)
					{
						S->SetFocusEdge(S->GetFocusEdge() == E->Edge ? FString() : E->Edge);
						if (S->GetDrawMode() == EExposureDrawMode::Off) { S->SetDrawMode(EExposureDrawMode::Changed); }
					}
					return FReply::Handled();
				})
				[
					SNew(SImage).Image(FAppStyle::GetBrush("Icons.Visible")).ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		];
	}

	// Body: this probe's choice first (shared body, a game's body, or custom), then the shared body itself, collapsed.
	const FName BodyCategory = FObjectEditorUtils::GetCategoryFName(FindFProperty<FProperty>(Cls, GET_MEMBER_NAME_CHECKED(AExposureProbeActor, BodyPreset)));
	IDetailCategoryBuilder& Body = Builder.EditCategory(BodyCategory, EPT("身体", "Body"), ECategoryPriority::Important);
	Body.SetSortOrder(1);
	Links.SetSortOrder(2);
	Body.AddProperty(Builder.GetProperty(GET_MEMBER_NAME_CHECKED(AExposureProbeActor, BodyPreset), Cls));
	// Dimensions only when the probe has its own body.
	TSharedRef<IPropertyHandle> Custom = Builder.GetProperty(GET_MEMBER_NAME_CHECKED(AExposureProbeActor, CustomBody), Cls);
	Builder.HideProperty(Custom);
	const TAttribute<EVisibility> OwnBody = TAttribute<EVisibility>::CreateLambda([this]()
	{
		for (const TWeakObjectPtr<AExposureProbeActor>& P : Probes)
		{
			if (P.IsValid() && P->BodyPreset != EExposureProbeBody::Global) { return EVisibility::Visible; }
		}
		return EVisibility::Collapsed;
	});
	uint32 NumChildren = 0;
	Custom->GetNumChildren(NumChildren);
	for (uint32 i = 0; i < NumChildren; ++i)
	{
		if (TSharedPtr<IPropertyHandle> Child = Custom->GetChildHandle(i))
		{
			Body.AddProperty(Child.ToSharedRef()).Visibility(OwnBody);
		}
	}

	IDetailGroup& Shared = Body.AddGroup(TEXT("SharedBody"), EPT("统一身体（全部探针）", "Shared Body (all probes)"), false, false);
	TArray<UObject*> Settings = { GetMutableDefault<UExposureProbeSettings>() };
	for (const FName Name : {
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, UnitPreset),
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, UnitToCm),
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, StandHeight),
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, HeadLength),
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, TorsoLength),
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, LegLength),
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, StandEye),
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, BodyWidth),
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, CrouchHeight),
		GET_MEMBER_NAME_CHECKED(UExposureProbeSettings, CrouchEye) })
	{
		Shared.AddExternalObjectProperty(Settings, Name, EPropertyLocation::Default, FAddPropertyParams());
	}

	if (UExposureProbeSubsystem* S = UExposureProbeSubsystem::Get())
	{
		S->OnReportUpdated.Remove(ReportHandle);
		ReportHandle = S->OnReportUpdated.AddRaw(this, &FExposureProbeDetails::HandleReport);
	}
}
