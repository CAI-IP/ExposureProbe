#include "ExposureProbeText.h"

#include "ExposureProbeActor.h"
#include "ExposureSmokeActor.h"
#include "ExposureProbeSettings.h"
#include "ExposureProbeTypes.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "UObject/UnrealType.h"

namespace ExposureProbeText
{
	bool IsChinese()
	{
		const UExposureProbeSettings* S = GetDefault<UExposureProbeSettings>();
		if (S && S->Language != EExposureLanguage::Auto)
		{
			return S->Language == EExposureLanguage::Chinese;
		}
		return FInternationalization::Get().GetCurrentLanguage()->GetTwoLetterISOLanguageName() == TEXT("zh");
	}

	FString Str(const TCHAR* Zh, const TCHAR* En)
	{
		return IsChinese() ? FString(Zh) : FString(En);
	}

	FText Get(const TCHAR* Zh, const TCHAR* En)
	{
		return FText::FromString(Str(Zh, En));
	}

	FText Format(const TCHAR* Zh, const TCHAR* En, const FFormatOrderedArguments& Args)
	{
		return FText::Format(FTextFormat::FromString(Str(Zh, En)), Args);
	}

#if WITH_EDITOR
	static void Prop(UStruct* Owner, const TCHAR* Name, const TCHAR* Display, const TCHAR* Category = nullptr, const TCHAR* Tip = nullptr)
	{
		if (FProperty* P = FindFProperty<FProperty>(Owner, Name))
		{
			P->SetMetaData(TEXT("DisplayName"), Display);
			if (Category) { P->SetMetaData(TEXT("Category"), Category); }
			P->SetMetaData(TEXT("ToolTip"), Tip ? Tip : Display);
		}
	}

	template <typename EnumType>
	static void Enum(EnumType Value, const TCHAR* Display)
	{
		UEnum* E = StaticEnum<EnumType>();
		const int32 Index = E->GetIndexByValue(int64(Value));
		E->SetMetaData(TEXT("DisplayName"), Display, Index);
		E->SetMetaData(TEXT("ToolTip"), Display, Index);
	}

	void ApplyEditorMetadata()
	{
		if (!IsChinese())
		{
			return;
		}
		UClass* S = UExposureProbeSettings::StaticClass();
		const TCHAR* Units = TEXT("单位");
		const TCHAR* Stand = TEXT("身体");
		const TCHAR* Crouch = TEXT("身体|蹲姿");
		const TCHAR* Sampling = TEXT("身体|采样");
		const TCHAR* Audit = TEXT("审计");
		const TCHAR* Tracing = TEXT("射线");
		const TCHAR* Display = TEXT("显示");
		Prop(S, TEXT("UnitPreset"), TEXT("对标游戏"), Units, TEXT("选择游戏后同时套用地图单位与玩家身体；手动修改任一尺寸后变为自定义"));
		Prop(S, TEXT("UnitToCm"), TEXT("1 单位 = 厘米"), Units);
		Prop(S, TEXT("StandHeight"), TEXT("身高"), Stand, TEXT("站姿头顶高度，即站立所需的头顶净空。修改身高时头、躯干、腿与眼位按比例缩放"));
		Prop(S, TEXT("HeadLength"), TEXT("头部"), Stand, TEXT("颈部到头顶。修改某一段时身高相应增减"));
		Prop(S, TEXT("TorsoLength"), TEXT("躯干"), Stand, TEXT("胯部到颈部。修改某一段时身高相应增减"));
		Prop(S, TEXT("LegLength"), TEXT("腿部"), Stand, TEXT("脚底到胯部。修改某一段时身高相应增减"));
		Prop(S, TEXT("StandEye"), TEXT("眼位"), Stand, TEXT("可见性射线的起点"));
		Prop(S, TEXT("BodyWidth"), TEXT("身宽"), Stand, TEXT("采样点分布在以身体轴线为中心、直径为身宽的圆上"));
		Prop(S, TEXT("CrouchHeight"), TEXT("头顶"), Crouch, TEXT("蹲姿头顶高度，即下蹲所需的头顶净空；头、躯干、腿按比例缩放"));
		Prop(S, TEXT("CrouchEye"), TEXT("眼位"), Crouch);
		Prop(S, TEXT("Sampling"), TEXT("采样"), Sampling, TEXT("16 点：4 层 × 4 方位（脚、膝、腰、头）；64 点：8 层 × 8 方位"));
		Prop(S, TEXT("StandSampleHeights"), TEXT("站姿采样高度"), Sampling);
		Prop(S, TEXT("CrouchSampleHeights"), TEXT("蹲姿采样高度"), Sampling);
		Prop(S, TEXT("SampleBearings"), TEXT("每层方位数"), Sampling);
		Prop(S, TEXT("OneWayThreshold"), TEXT("单向视线阈值"), Audit, TEXT("一方看见对方的比例不低于此值且对方完全看不见这一方时，记为单向视线"));
		Prop(S, TEXT("FarDistance"), TEXT("远处影响距离"), Audit, TEXT("受影响的探针对离改动超过此距离（单位）时标为远处影响"));
		Prop(S, TEXT("ChangeThreshold"), TEXT("明显变化阈值"), Audit, TEXT("回归报告中 |ΔA| 达到此值的探针对记为明显变化"));
		Prop(S, TEXT("bBaselineOnSave"), TEXT("保存关卡时存基线"), Audit, TEXT("每次保存关卡时自动保存「上次保存」基线"));
		Prop(S, TEXT("bAutoUpdate"), TEXT("改动后自动更新"), Audit);
		Prop(S, TEXT("DebounceSeconds"), TEXT("松手后延迟"), Audit);
		Prop(S, TEXT("TraceChannel"), TEXT("检测通道"), Tracing);
		Prop(S, TEXT("bTraceComplex"), TEXT("复杂碰撞"), Tracing);
		Prop(S, TEXT("HitToleranceCm"), TEXT("命中容差"), Tracing);
		Prop(S, TEXT("bSnapProbesToFloor"), TEXT("探针贴地"), Tracing);
		Prop(S, TEXT("bParallelTraces"), TEXT("并行检测"), Tracing);
		Prop(S, TEXT("RayThickness"), TEXT("线宽"), Display);
		Prop(S, TEXT("bShowBlockedRays"), TEXT("显示被挡射线"), Display);
		Prop(S, TEXT("bShowLabels"), TEXT("显示标签"), Display);
		Prop(S, TEXT("TColor"), TEXT("T 方颜色"), Display);
		Prop(S, TEXT("CTColor"), TEXT("CT 方颜色"), Display);
		Prop(S, TEXT("Language"), TEXT("界面语言"), Display, TEXT("重启编辑器后生效"));
		const TCHAR* HeatCat = TEXT("热力图");
		Prop(S, TEXT("HeatmapSpacing"), TEXT("采样间距"), HeatCat, TEXT("地面采样点间距（单位）；间距越小越精细，计算越久"));
		Prop(S, TEXT("HeatmapRadius"), TEXT("邻域半径"), HeatCat, TEXT("此距离内的其他位置计入 A（单位）"));
		Prop(S, TEXT("HeatmapScale"), TEXT("满色 |A|"), HeatCat, TEXT("|A| 达到此值时颜色最深；0 表示自动，取本图 |A| 的第 90 百分位"));
		Prop(S, TEXT("HeatmapBody"), TEXT("身体采样"), HeatCat, TEXT("16 点：4 层 × 4 方位，最快；与探针相同：使用探针的采样设置"));
		Prop(S, TEXT("bHeatmapOnNavMesh"), TEXT("只在导航网格上"), HeatCat, TEXT("关卡有导航网格时只在可走区域取样"));
		Prop(S, TEXT("bHeatmapReachableOnly"), TEXT("仅限可到达的地面"), HeatCat, TEXT("仅保留从探针或玩家出生点可步行或跳上到达的地面；导航网格覆盖到屋顶等无法到达之处时同样排除"));
		Prop(S, TEXT("HeatmapMaxClimb"), TEXT("最大攀爬高度"), HeatCat, TEXT("玩家可跳上的最大高度（单位），例如蹲跳；高于此值的台面视为无法到达"));
		Prop(S, TEXT("HeatSeesColor"), TEXT("看见更多的颜色"), HeatCat);
		Prop(S, TEXT("HeatSeenColor"), TEXT("被看见更多的颜色"), HeatCat);
		const TCHAR* SmokeCat = TEXT("烟雾");
		Prop(S, TEXT("SmokePreset"), TEXT("对标游戏的烟"), SmokeCat, TEXT("选择游戏后同时套用烟雾半径、球心高度与持续时间；手动修改任一项后变为自定义。尺寸设为「统一」的烟雾也使用此设置"));
		Prop(S, TEXT("SmokeRadius"), TEXT("烟雾半径"), SmokeCat, TEXT("烟按球体计算（单位）"));
		Prop(S, TEXT("SmokeLift"), TEXT("球心高度"), SmokeCat, TEXT("球心在落点上方的高度（单位）"));
		Prop(S, TEXT("SmokeCoverage"), TEXT("覆盖阈值"), SmokeCat, TEXT("烟雾需遮挡的可见视线比例；CC 为达到该比例所需的最少烟雾数"));
		Prop(S, TEXT("SmokeMaxCount"), TEXT("烟雾数上限"), SmokeCat, TEXT("达到上限仍未满足阈值时，CC 记为右删失"));
		Prop(S, TEXT("SmokeClearance"), TEXT("落点净空"), SmokeCat, TEXT("落点上方需要的空余高度（单位）"));
		Prop(S, TEXT("SmokeMinDistance"), TEXT("与观察者的最小距离"), SmokeCat, TEXT("距观察者近于此值的落点不予考虑（单位）"));
		Prop(S, TEXT("SmokeSearchRadius"), TEXT("落点搜索半径"), SmokeCat, TEXT("距观察者或目标在此距离内的地面均可作为落点（单位）"));
		Prop(S, TEXT("SmokeDuration"), TEXT("烟雾持续时间"), SmokeCat, TEXT("RT 不超过这个时间"));
		Prop(S, TEXT("SmokeRetreatRadius"), TEXT("绕行搜索半径"), SmokeCat, TEXT("在距观察者此距离内搜索绕行位置（单位）"));
		Prop(S, TEXT("SmokeRecoverShare"), TEXT("恢复视线比例"), SmokeCat, TEXT("绕行位置隔着烟雾仍需可见的原视线比例"));
		Prop(S, TEXT("SmokeRecoverDeltaA"), TEXT("允许的 A 下降"), SmokeCat, TEXT("绕行位置的 A 相对原位置的最大下降（需要地面分析）"));
		Prop(S, TEXT("SmokeMoveSpeed"), TEXT("移动速度"), SmokeCat, TEXT("计算绕行时间用的步行速度（单位/秒）"));
		Prop(S, TEXT("SmokeReacquireSeconds"), TEXT("重新瞄准时间"), SmokeCat, TEXT("到达绕行位置后重新瞄准所需的时间"));
		Prop(S, TEXT("SmokeStyle"), TEXT("显示样式"), SmokeCat, TEXT("体积烟：光线步进的体积，在墙面与地面处截断；烟团：半透明球体。计算始终按球体"));
		Prop(S, TEXT("SmokeDensity"), TEXT("体积烟浓度"), SmokeCat, TEXT("数值越大越不透明"));
		Prop(S, TEXT("SmokeColor"), TEXT("烟雾颜色"), SmokeCat);
		Prop(S, TEXT("bSmokeFog"), TEXT("局部雾体积"), SmokeCat, TEXT("在烟雾球体内另加局部雾体积；约 10 米外才显示"));
		Prop(S, TEXT("bSmokesBlockProbes"), TEXT("烟雾遮挡探针视线"), SmokeCat, TEXT("已放置的烟雾计入探针射线：连线、身体着色、A 与改动报告按烟雾遮挡后的结果显示；地面图层仍按静态几何计算"));
		Prop(S, TEXT("bSnapSmokesToCells"), TEXT("烟雾吸附格子"), SmokeCat, TEXT("放置或拖动后吸附到最近的地面分析格子（需先分析地面）"));

		UClass* A = AExposureProbeActor::StaticClass();
		const TCHAR* Probe = TEXT("暴露探针");
		Prop(A, TEXT("ProbeName"), TEXT("名称"), Probe, TEXT("为空时使用大纲视图中的名字"));
		Prop(A, TEXT("Team"), TEXT("阵营"), Probe);
		Prop(A, TEXT("Pose"), TEXT("姿态"), Probe);
		Prop(A, TEXT("DesignIntent"), TEXT("设计保留"), Probe, TEXT("与这些 CT 探针之间的视线关系属于设计保留"));
		Prop(A, TEXT("HiddenLinks"), TEXT("隐藏的连线"), Probe, TEXT("视口中不绘制与这些探针的连线；审计仍包括它们"));
		Prop(A, TEXT("BodyPreset"), TEXT("探针身体"), TEXT("身体"), TEXT("统一：使用全部探针共用的身体。选择游戏或自定义时只影响此探针"));
		Prop(A, TEXT("CustomBody"), TEXT("尺寸"), TEXT("身体"), TEXT("地图单位。修改身高时各段按比例缩放；修改某一段时身高相应增减"));
		UScriptStruct* B = FExposureBodyShape::StaticStruct();
		Prop(B, TEXT("StandHeight"), TEXT("身高"), nullptr, TEXT("修改身高时头、躯干、腿与眼位按比例缩放"));
		Prop(B, TEXT("HeadLength"), TEXT("头部"), nullptr, TEXT("颈部到头顶；修改此段时身高相应增减"));
		Prop(B, TEXT("TorsoLength"), TEXT("躯干"), nullptr, TEXT("胯部到颈部；修改此段时身高相应增减"));
		Prop(B, TEXT("LegLength"), TEXT("腿部"), nullptr, TEXT("脚底到胯部；修改此段时身高相应增减"));
		Prop(B, TEXT("StandEye"), TEXT("眼位"));
		Prop(B, TEXT("BodyWidth"), TEXT("身宽"));
		Prop(B, TEXT("CrouchHeight"), TEXT("蹲姿头顶"));
		Prop(B, TEXT("CrouchEye"), TEXT("蹲姿眼位"));
		UClass* SA = AExposureSmokeActor::StaticClass();
		Prop(SA, TEXT("bEnabled"), TEXT("参与计算"), TEXT("烟雾"), TEXT("关闭后不绘制，也不计入遮挡与评估"));
		Prop(SA, TEXT("Size"), TEXT("烟雾尺寸"), TEXT("烟雾"), TEXT("统一：使用插件设置中的烟雾尺寸。选择游戏或自定义时只影响此烟雾"));
		Prop(SA, TEXT("CustomSmoke"), TEXT("尺寸"), TEXT("烟雾"), TEXT("地图单位；修改任一项后变为自定义"));
		UScriptStruct* SS = FExposureSmokeShape::StaticStruct();
		Prop(SS, TEXT("Radius"), TEXT("半径"), nullptr, TEXT("烟按球体计算（单位）"));
		Prop(SS, TEXT("Lift"), TEXT("球心高度"), nullptr, TEXT("球心在落点上方的高度（单位）"));
		Prop(SS, TEXT("Duration"), TEXT("持续时间"), nullptr, TEXT("RT 不超过这个时间"));
		SA->SetMetaData(TEXT("DisplayName"), TEXT("烟雾"));
		UScriptStruct* M = FExposureDesignMark::StaticStruct();
		Prop(M, TEXT("CTProbe"), TEXT("CT 探针"));
		Prop(M, TEXT("Note"), TEXT("备注"));

		A->SetMetaData(TEXT("DisplayName"), TEXT("暴露探针"));
		AExposureProbeT::StaticClass()->SetMetaData(TEXT("DisplayName"), TEXT("T 探针"));
		AExposureProbeCT::StaticClass()->SetMetaData(TEXT("DisplayName"), TEXT("CT 探针"));

		Enum(EExposurePose::Auto, TEXT("自动"));
		Enum(EExposurePose::Stand, TEXT("站立"));
		Enum(EExposurePose::Crouch, TEXT("下蹲"));
		Enum(EExposureUnitPreset::Source, TEXT("CS2 / Source（2.54 cm）"));
		Enum(EExposureUnitPreset::CallOfDuty, TEXT("使命召唤（2.54 cm）"));
		Enum(EExposureUnitPreset::Valorant, TEXT("无畏契约（1 cm）"));
		Enum(EExposureUnitPreset::UnrealTemplate, TEXT("UE 第一人称模板（1 cm）"));
		Enum(EExposureUnitPreset::Unreal, TEXT("成人 180 cm（1 cm）"));
		Enum(EExposureUnitPreset::Custom, TEXT("自定义"));
		Enum(EExposureProbeBody::Global, TEXT("统一"));
		Enum(EExposureProbeBody::Source, TEXT("CS2 / Source"));
		Enum(EExposureProbeBody::CallOfDuty, TEXT("使命召唤"));
		Enum(EExposureProbeBody::Valorant, TEXT("无畏契约"));
		Enum(EExposureProbeBody::UnrealTemplate, TEXT("UE 第一人称模板"));
		Enum(EExposureProbeBody::Unreal, TEXT("成人 180 cm"));
		Enum(EExposureProbeBody::Custom, TEXT("自定义"));
		Enum(EExposureSampling::Fine, TEXT("64 点（8 × 8）"));
		Enum(EExposureSampling::Points16, TEXT("16 点（4 × 4）"));
		Enum(EExposureSampling::Custom, TEXT("自定义"));
		Enum(EExposureHeatBody::Points16, TEXT("16 点（4 × 4）"));
		Enum(EExposureHeatBody::Probe, TEXT("与探针相同"));
		Enum(EExposureSmokePreset::CS2, TEXT("CS2 烟雾弹"));
		Enum(EExposureSmokePreset::ValorantBrimstone, TEXT("无畏契约 Brimstone"));
		Enum(EExposureSmokePreset::ValorantOmen, TEXT("无畏契约 Omen"));
		Enum(EExposureSmokePreset::ValorantAstra, TEXT("无畏契约 Astra"));
		Enum(EExposureSmokePreset::Custom, TEXT("自定义"));
		Enum(EExposureSmokeSize::Shared, TEXT("统一"));
		Enum(EExposureSmokeSize::CS2, TEXT("CS2 烟雾弹"));
		Enum(EExposureSmokeSize::ValorantBrimstone, TEXT("无畏契约 Brimstone"));
		Enum(EExposureSmokeSize::ValorantOmen, TEXT("无畏契约 Omen"));
		Enum(EExposureSmokeSize::ValorantAstra, TEXT("无畏契约 Astra"));
		Enum(EExposureSmokeSize::Custom, TEXT("自定义"));
		Enum(EExposureSmokeStyle::Volume, TEXT("体积烟"));
		Enum(EExposureSmokeStyle::Puffs, TEXT("烟团"));
		Enum(EExposureLanguage::Auto, TEXT("跟随编辑器"));
		Enum(EExposureLanguage::Chinese, TEXT("中文"));
		Enum(EExposureLanguage::English, TEXT("English"));
	}
#endif
}
