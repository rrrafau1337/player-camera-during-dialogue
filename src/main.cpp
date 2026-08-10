#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#ifdef _MSC_VER
#pragma comment(lib, "Gdi32.lib")
#endif

#ifdef ERROR
#undef ERROR
#endif

#include "REL/THook.h"
#include "RE/B/BGSKeyword.h"
#include "RE/I/INISettingCollection.h"
#include "RE/N/NiAVObject.h"
#include "RE/N/NiNode.h"
#include "RE/P/PlayerCamera.h"
#include "RE/S/Setting.h"
#include "RE/T/TESObjectREFR.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	constexpr char kDialogueMenuName[] = "DialogueMenu";
	constexpr char kSpaceshipHudMenuName[] = "SpaceshipHudMenu";
	constexpr char kIniPath[] =
		".\\Data\\SFSE\\Plugins\\PointCameraAtPlayer.ini";

	enum class SwitchMode : std::uint32_t
	{
		kManual = 0,
		kAutomatic = 1
	};

	enum class ViewReason : std::uint32_t
	{
		kManual,
		kSpeakerStarted,
		kSpeakerStopped
	};

	// Current Starfield 1.16.244.0 actor-level facial-archetype
	// wrapper used by the registered ChangeAnimFaceArchetype native.
	constexpr std::uintptr_t kChangeAnimFaceArchetypeRva = 0x18E0220;

	// Facial archetype keywords exposed by Actor.ChangeAnimFaceArchetype.
	// Photo Mode has its own morph/preset presentation path, but these are
	// the stable gameplay expression archetypes we can safely dispatch from
	// an SFSE plugin without driving the PhotoModeMenu itself. Keep only
	// gameplay-facing archetypes; character-generation actions are not
	// expressions and caused confusing no-ops in older builds.
	constexpr const char* kExpressionNames[] = {
		"Neutral",
		"Afraid",
		"Amused",
		"Angry",
		"Apologetic",
		"Awed",
		"Concentrating",
		"Concerned",
		"Confident",
		"Confused",
		"Defiant",
		"Depressed",
		"Disbelief",
		"Disgust",
		"Dying",
		"EyesClosed",
		"Flirting",
		"Friendly",
		"Grateful",
		"Happy",
		"Impressed",
		"InPain",
		"Irritated",
		"Nervous",
		"Pleading",
		"Puzzled",
		"Question",
		"Rage",
		"Relieved",
		"Sarcastic",
		"Serious",
		"Shocked",
		"SinisterSmile",
		"Smirk",
		"Smug",
		"Somber",
		"Stern",
		"Surprised",
		"Suspicious",
		"Thinking",
		"Tired",
		"VeryAngry",
		"Worried"
	};

	constexpr std::size_t kExpressionCount =
		sizeof(kExpressionNames) /
		sizeof(kExpressionNames[0]);

	// Starfield 1.16.244.0 only.
	constexpr std::ptrdiff_t kRequestedTargetHandle = 0x2F4;
	constexpr std::ptrdiff_t kCurrentTargetHandle = 0x2F8;

	// DialogueCameraState::Update calls this helper early in the frame.
	// The helper resolves the scene speaker and writes +0x2F8.
	constexpr std::uintptr_t kResolveDialogueTargetCallsiteRva = 0xF9C20D;

	// Internal DialogueCameraState reset/rebuild function.
	constexpr std::uintptr_t kRebuildDialogueCameraRva = 0xF9E5A0;

	// Exact modern equivalent of the function-entry hook used by the
	// original 2023 mod:
	//
	// Old Starfield 1.7.36:
	//   controller update RVA 0x1D72B04
	//   active-dialogue loop call RVA 0x286A8DA
	//
	// Starfield 1.16.244.0:
	//   controller update RVA 0xF98080
	//   active-dialogue loop call RVA 0x1E8C0FF
	//
	// At this callsite RCX is the controller object and EDX is the active
	// dialogue target handle read from DialogueCameraState +0x2F8.
	constexpr std::uintptr_t kControllerUpdateCallsiteRva = 0x1E8C0FF;

	constexpr std::ptrdiff_t kControllerTargetHandle = 0x60;
	constexpr std::ptrdiff_t kControllerState = 0x9C;

	// DialogueCameraState vtable and Update slot established by the
	// earlier vtable probe.
	constexpr std::uintptr_t kDialogueStateVtableRva = 0x4C27500;
	constexpr std::size_t kDialogueUpdateVtableIndex = 12;

	// NiAVObject virtual slot used by Starfield 1.7.36's native dialogue
	// presentation path to toggle the target's app-culling state.
	// Slot 61 is still present in the current NiAVObject vtable.
	constexpr std::size_t kSetAppCulledVtableIndex = 61;
	constexpr std::uint64_t kAppCulledBit = 0x1;

	using RebuildDialogueCamera_t = void (*)(void*, bool);
	using SetAppCulled_t = void (*)(RE::NiAVObject*, bool);
	using ChangeAnimFaceArchetype_t =
		void (*)(RE::Actor*, RE::BGSKeyword*);

	// Forward declarations used by the staged body-preparation and camera hooks.
	[[nodiscard]] RebuildDialogueCamera_t GetRebuildFunction();
	void ResetSpeakerHeadAnchorLock();

	const SFSE::TaskInterface* g_taskInterface = nullptr;

	SwitchMode g_switchMode = SwitchMode::kAutomatic;
	int g_manualSwitchKey = 'V';
	int g_expressionCycleKey = 'X';
	int g_cinematicLightingToggleKey = 'L';
	std::uint32_t g_autoPlayerDelayMs = 250;
	std::uint32_t g_autoNpcDelayMs = 60;
	std::uint32_t g_autoStartupGraceMs = 350;
	// Explicit V camera changes remain authoritative briefly even in automatic
	// mode. Automatic switching resumes after this bounded grace period.
	constexpr std::uint32_t kManualCameraOverrideGraceMs = 1000;
	bool g_obstructionHandling = true;
	std::uint32_t g_obstructionRadiusPercent = 70;
	bool g_fixFirstPersonBody = true;
	std::uint32_t g_firstPersonBodyPrepDelayMs = 250;
	std::uint32_t g_dialogueOpenCameraDelayMs = 350;
	std::uint32_t g_firstPersonRestoreDelayMs = 120;
	bool g_fixPlayerHeadVisibility = true;
	bool g_usePlayerHeadFraming = true;
	std::uint32_t g_playerFramingHeightPercent = 55;
	bool g_stablePlayerVisibility = true;
	bool g_expressionNotifications = true;
	std::uint32_t g_expressionNotificationDurationMs = 1400;
	std::uint32_t g_expressionApplyDelayMs = 120;
	std::uint32_t g_exitVisibilityCleanupMs = 1500;
	std::uint32_t g_exitVisibilitySettleDelayMs = 150;
	std::uint32_t g_obstructionCheckIntervalMs = 100;
	std::uint32_t g_headVisibilityCheckIntervalMs = 250;
	bool g_lockPlayerCamera = false;
	bool g_funMode = false;
	bool g_cinematicPlayerLighting = true;
	// Force a known-good body state the instant dialogue opens: sheathe the
	// weapon and drop to third person, so the engine builds a fully posed
	// external skeleton before any player shot. Sidesteps the half-built
	// first-person-body cases entirely.
	bool g_forceSheatheOnDialogue = true;
	bool g_forceThirdPersonOnDialogue = true;
	// Keep an established player shot from being overwritten by Starfield's
	// recurring dialogue-target resolver. This is bounded by DialogueMenu state
	// so native teardown/input cleanup always resumes when dialogue closes.
	bool g_suppressCameraRecentering = true;
	std::size_t g_defaultExpressionIndex = kExpressionCount;

	std::atomic_flag g_tickLock = ATOMIC_FLAG_INIT;
	std::atomic_bool g_actionQueued = false;
	std::atomic_bool g_playerViewRequested = false;
	std::atomic_uint32_t g_earlyOverrideCount = 0;
	std::atomic_uintptr_t g_capturedController = 0;
	std::atomic_uint32_t g_capturedControllerTarget = 0;
	std::atomic_uint32_t g_controllerHookLogCount = 0;
	std::atomic_uint32_t g_visibilityHookLogCount = 0;
	std::atomic_bool g_visibilityCallFailed = false;
	std::atomic_bool g_expressionActionQueued = false;
	std::atomic_bool g_expressionOverrideActive = false;
	std::atomic_uintptr_t g_activeExpressionKeyword = 0;
	std::atomic_uintptr_t g_culledSpeakerRoot = 0;
	std::atomic_bool g_playerCameraLocked = false;
	// Set once the current dialogue composition has completed its intentional
	// rebuild. For NPC shots, the framing helper temporarily presents a fixed speaker
	// head transform to the native camera routines while leaving the visible actor
	// and all dialogue/input bookkeeping untouched.
	std::atomic_bool g_playerShotEstablished = false;
	std::atomic_uint32_t g_stabilizedSpeakerHeadUpdateCount = 0;
	RE::NiAVObject* g_lockedSpeakerHeadRoot = nullptr;
	RE::NiAVObject* g_lockedSpeakerHeadObject = nullptr;
	RE::NiTransform g_lockedSpeakerHeadWorld{};
	RE::NiTransform g_lockedSpeakerHeadPreviousWorld{};
	RE::NiPoint3 g_lockedSpeakerHeadBoundCenter{};
	float g_lockedSpeakerHeadBoundRadius = 0.0F;
	bool g_speakerHeadAnchorLocked = false;
	std::atomic_uint32_t g_suppressedTargetResolveCount = 0;
	std::atomic_uint32_t g_headVisibilityLogCount = 0;
	std::atomic_uint32_t g_headFramingLogCount = 0;
	std::atomic_flag g_playerFramingOverrideLock = ATOMIC_FLAG_INIT;
	std::atomic_bool g_dialogueActiveForHooks = false;
	std::atomic_bool g_shipDialogueBypass = false;
	ULONGLONG g_lastSpaceshipHudSeenAt = 0;
	constexpr std::uint32_t kRecentSpaceshipHudWindowMs = 2000;
	std::atomic_uintptr_t g_lastDialogueSpeakerReference = 0;
	std::atomic_uintptr_t g_lastDialogueSpeakerRoot = 0;
	std::atomic_uint64_t g_exitVisibilityCleanupUntil = 0;
	std::atomic_uint64_t g_exitVisibilityLastLateUpdateAt = 0;
	std::atomic_uint64_t g_exitVisibilityStartedAt = 0;
	// A DialogueCameraState teardown update can run after Tick has repaired
	// visibility but before the first gameplay frame is rendered. Repair once
	// more at the end of that native update so the outgoing speaker/player do
	// not vanish for the single transition frame.
	std::atomic_bool g_exitPreRenderVisibilityRepaired = false;
	// Always keep repairing NPC visibility for at least this long after dialogue
	// close, so a late teardown root swap is still caught.
	constexpr std::uint64_t kExitVisibilityMinRepairMs = 600;

	std::atomic_uint64_t g_nextObstructionCheckAt = 0;
	std::atomic_uint64_t g_nextHeadVisibilityCheckAt = 0;
	std::atomic_uintptr_t g_cachedPlayerHeadRoot = 0;
	std::atomic_uintptr_t g_cachedPlayerHeadObject = 0;
	std::atomic_int g_cachedPlayerHeadNameIndex = -2;

	constexpr UINT kExpressionNotificationMessage = WM_APP + 0x451;
	constexpr UINT_PTR kExpressionNotificationTimerId = 0x451;
	constexpr wchar_t kExpressionNotificationWindowClass[] =
		L"PointCameraAtPlayerExpressionNotification";
	std::atomic_uintptr_t g_expressionNotificationWindow = 0;
	std::atomic_bool g_expressionNotificationThreadStarted = false;
	HFONT g_expressionNotificationFont = nullptr;
	HBRUSH g_expressionNotificationBrush = nullptr;
	std::wstring g_expressionNotificationText;

	bool g_dialogueOpen = false;
	bool g_vWasDown = false;
	bool g_expressionWasDown = false;
	bool g_cinematicLightingToggleWasDown = false;
	std::size_t g_expressionIndex = 0;
	std::size_t g_lastFunExpressionIndex = kExpressionCount;
	std::uint32_t g_funRandomState = 0;
	bool g_gameplayViewKnown = false;
	bool g_lastGameplayFirstPerson = false;
	bool g_restoreFirstPersonAfterDialogue = false;
	bool g_forcedThirdPersonForDialogue = false;
	bool g_firstPersonRestorePending = false;
	ULONGLONG g_firstPersonRestoreAt = 0;
	ULONGLONG g_gameplayCameraRestoreDeadlineAt = 0;
	bool g_bodyPreparationPending = false;
	ULONGLONG g_bodyPreparationResumeAt = 0;
	ULONGLONG g_bodyPreparationDeadlineAt = 0;
	ViewReason g_deferredPlayerViewReason = ViewReason::kManual;

	// What the staged third-person body preparation should do once the body is
	// posed and DialogueCameraState is restored. Normally it queues the player
	// shot; at dialogue open we instead want to fall back to the engine's
	// natural NPC-facing dialogue view so the conversation *starts* face-to-face
	// even though we briefly forced third person to build the body.
	enum class BodyPrepTerminal : std::uint32_t
	{
		kShowPlayer,
		kShowNpc
	};
	BodyPrepTerminal g_bodyPrepTerminal = BodyPrepTerminal::kShowPlayer;

	// The external skeleton exists as scene-graph nodes several frames before
	// the animation graph has actually posed it. Showing the player shot during
	// that window produces the twisted/backwards body. Instead of trusting a
	// fixed millisecond delay (which loses the race at high frame rates), sample
	// the head-bone world position across consecutive third-person frames and
	// only proceed once it has stopped moving, i.e. the pose has settled.
	RE::NiPoint3 g_bodyPrepLastHeadPoint{};
	bool g_bodyPrepHasHeadSample = false;
	std::uint32_t g_bodyPrepStableFrames = 0;
	constexpr std::uint32_t kBodyPrepRequiredStableFrames = 3;
	constexpr float kBodyPrepStableEpsilonSq = 0.0025F;

	bool g_autoCandidateInitialized = false;
	bool g_autoCandidatePlayer = false;
	bool g_lastSpeakerTalking = false;
	ULONGLONG g_autoCandidateSince = 0;
	ULONGLONG g_autoIgnoreUntil = 0;

	std::atomic_bool g_pendingPlayerShotExpression = false;
	std::atomic_uint64_t g_pendingPlayerShotExpressionAt = 0;
	std::atomic_bool g_automaticExpressionTaskQueued = false;

	std::atomic_bool g_dialogueLightingWarningLogged = false;

	struct PerfCounter
	{
		std::atomic_uint64_t calls{ 0 };
		std::atomic_uint64_t totalUs{ 0 };
		std::atomic_uint64_t maxUs{ 0 };
	};

	PerfCounter g_perfTick;
	PerfCounter g_perfDialogueUpdateHook;
	PerfCounter g_perfControllerHook;
	PerfCounter g_perfResolverHook;
	PerfCounter g_perfSetRootAppCulled;
	PerfCounter g_perfObstructionUpdate;
	PerfCounter g_perfForceDialogueActorsVisible;
	PerfCounter g_perfHeadVisibility;
	PerfCounter g_perfBodyPreparation;
	PerfCounter g_perfAutomaticCamera;

	std::atomic_uint64_t g_perfVisibilityNoOp{ 0 };
	std::atomic_uint64_t g_perfVisibilityNativeCalls{ 0 };
	std::atomic_uint64_t g_perfVisibilityShowCalls{ 0 };
	std::atomic_uint64_t g_perfVisibilityCullCalls{ 0 };
	std::atomic_uint64_t g_perfPlayerVisibilityNativeCalls{ 0 };
	std::atomic_uint64_t g_perfSpeakerVisibilityNativeCalls{ 0 };
	std::atomic_uint64_t g_perfHeadVisibilityNativeCalls{ 0 };

	std::atomic_uint64_t g_perfObstructionChecks{ 0 };
	std::atomic_uint64_t g_perfObstructionHits{ 0 };
	std::atomic_uint64_t g_perfObstructionSpeakerCull{ 0 };
	std::atomic_uint64_t g_perfObstructionSpeakerRestore{ 0 };

	std::atomic_uint64_t g_perfResolverSuppressed{ 0 };
	std::atomic_uint64_t g_perfControllerPlayerOverrides{ 0 };

	std::atomic_uintptr_t g_primaryDialogueController{ 0 };
	std::array<std::atomic_uintptr_t, 16> g_seenDialogueControllers{};
	std::atomic_uint64_t g_seenDialogueControllerCount{ 0 };
	std::atomic_uint64_t g_duplicateControllerUpdatesSuppressed{ 0 };
	std::atomic_uint64_t g_perfPrevDuplicateControllerUpdatesSuppressed{ 0 };

	std::atomic_uint64_t g_perfCameraRebuildCalls{ 0 };
	std::atomic_uint64_t g_perfCameraRebuildTotalUs{ 0 };
	std::atomic_uint64_t g_perfCameraRebuildMaxUs{ 0 };
	std::atomic_uint64_t g_perfViewTransitions{ 0 };

	std::atomic_uint64_t g_perfForceThirdPersonCalls{ 0 };
	std::atomic_uint64_t g_perfSetDialogueCameraStateCalls{ 0 };
	std::atomic_uint64_t g_perfExitCleanupUpdates{ 0 };
	std::atomic_uint64_t g_perfExitPreRenderRepairs{ 0 };

	std::atomic_uint64_t g_perfPlayerRootChanges{ 0 };
	std::atomic_uint64_t g_perfSpeakerRootChanges{ 0 };
	std::atomic_uintptr_t g_perfLastPlayerRoot{ 0 };
	std::atomic_uintptr_t g_perfLastSpeakerRoot{ 0 };

	std::atomic_uint64_t g_perfDialogueIndex{ 0 };
	std::atomic_uint64_t g_perfLastSnapshotAt{ 0 };
	std::atomic_uint64_t g_perfPrevTickCalls{ 0 };
	std::atomic_uint64_t g_perfPrevDialogueHookCalls{ 0 };
	std::atomic_uint64_t g_perfPrevControllerHookCalls{ 0 };
	std::atomic_uint64_t g_perfPrevResolverHookCalls{ 0 };
	std::atomic_uint64_t g_perfPrevVisibilityNativeCalls{ 0 };
	std::atomic_uint64_t g_perfPrevObstructionChecks{ 0 };

	std::atomic_uint64_t g_viewTransitionRetryAfter{ 0 };
	std::atomic_bool g_viewTransitionRetryWarningArmed{ false };

	std::atomic_uint64_t g_perfViewTransitionRequests{ 0 };
	std::atomic_uint64_t g_perfViewTransitionCoalesced{ 0 };
	std::atomic_uint64_t g_perfViewTransitionBackoffSkipped{ 0 };
	std::atomic_uint64_t g_perfViewTransitionNoStateAborts{ 0 };

	std::atomic_uint64_t g_perfPrevViewTransitionRequests{ 0 };
	std::atomic_uint64_t g_perfPrevViewTransitionCoalesced{ 0 };
	std::atomic_uint64_t g_perfPrevViewTransitionBackoffSkipped{ 0 };
	std::atomic_uint64_t g_perfPrevViewTransitionNoStateAborts{ 0 };

	constexpr std::uint32_t kViewTransitionRetryBackoffMs = 50;

	[[nodiscard]] const char* DescribeViewReason(
		const ViewReason reason)
	{
		switch (reason) {
		case ViewReason::kManual:
			return "manual";
		case ViewReason::kSpeakerStarted:
			return "speaker-started";
		case ViewReason::kSpeakerStopped:
			return "speaker-stopped";
		default:
			return "unknown";
		}
	}

	[[nodiscard]] bool IsAsciiWhitespace(const char value)
	{
		return value == ' ' ||
			value == '\t' ||
			value == '\r' ||
			value == '\n';
	}

	[[nodiscard]] char ToLowerAscii(const char value)
	{
		return value >= 'A' && value <= 'Z' ?
			static_cast<char>(value + ('a' - 'A')) :
			value;
	}

	[[nodiscard]] bool EqualsIgnoreCaseAscii(
		const std::string_view left,
		const std::string_view right)
	{
		if (left.size() != right.size()) {
			return false;
		}

		for (std::size_t index = 0; index < left.size(); ++index) {
			if (ToLowerAscii(left[index]) !=
				ToLowerAscii(right[index])) {
				return false;
			}
		}

		return true;
	}

	[[nodiscard]] std::string_view TrimAscii(
		std::string_view value)
	{
		while (!value.empty() && IsAsciiWhitespace(value.front())) {
			value.remove_prefix(1);
		}

		while (!value.empty() && IsAsciiWhitespace(value.back())) {
			value.remove_suffix(1);
		}

		return value;
	}

	void LoadDefaultExpressionSetting()
	{
		char rawValue[128]{};
		const auto length = GetPrivateProfileStringA(
			"Settings",
			"sDefaultExpression",
			"",
			rawValue,
			static_cast<DWORD>(sizeof(rawValue)),
			kIniPath);

		std::string_view requested{ rawValue, length };
		requested = TrimAscii(requested);

		constexpr std::string_view notificationPrefix{ "Expression:" };
		if (requested.size() >= notificationPrefix.size() &&
			EqualsIgnoreCaseAscii(
				requested.substr(0, notificationPrefix.size()),
				notificationPrefix)) {
			requested = TrimAscii(
				requested.substr(notificationPrefix.size()));
		}

		constexpr std::string_view editorIdPrefix{ "AnimFaceArchetype" };
		if (requested.size() >= editorIdPrefix.size() &&
			EqualsIgnoreCaseAscii(
				requested.substr(0, editorIdPrefix.size()),
				editorIdPrefix)) {
			requested = TrimAscii(
				requested.substr(editorIdPrefix.size()));
		}

		g_defaultExpressionIndex = kExpressionCount;

		if (requested.empty() ||
			EqualsIgnoreCaseAscii(requested, "None") ||
			EqualsIgnoreCaseAscii(requested, "Off") ||
			EqualsIgnoreCaseAscii(requested, "Disabled")) {
			return;
		}

		for (std::size_t index = 0; index < kExpressionCount; ++index) {
			if (EqualsIgnoreCaseAscii(requested, kExpressionNames[index])) {
				g_defaultExpressionIndex = index;
				return;
			}
		}

		REX::WARN(
			"Unknown sDefaultExpression='{}'. Use the text shown after "
			"Expression: in the in-game notification; default disabled.",
			std::string(requested));
	}

	void LoadSettings()
	{
		const int rawMode =
			GetPrivateProfileIntA(
				"Settings",
				"iSwitchMode",
				static_cast<int>(SwitchMode::kAutomatic),
				kIniPath);

		g_switchMode =
			rawMode == static_cast<int>(SwitchMode::kManual) ?
				SwitchMode::kManual :
				SwitchMode::kAutomatic;

		g_manualSwitchKey =
			GetPrivateProfileIntA(
				"Settings",
				"iManualSwitchKey",
				'V',
				kIniPath);

		g_expressionCycleKey =
			GetPrivateProfileIntA(
				"Settings",
				"iExpressionCycleKey",
				'X',
				kIniPath);

		g_cinematicLightingToggleKey =
			GetPrivateProfileIntA(
				"Settings",
				"iCinematicLightingToggleKey",
				'L',
				kIniPath);

		g_autoPlayerDelayMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iAutoPlayerDelayMs",
					250,
					kIniPath));

		g_autoNpcDelayMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iAutoNpcDelayMs",
					60,
					kIniPath));

		g_autoStartupGraceMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iAutoStartupGraceMs",
					350,
					kIniPath));


		g_obstructionHandling =
			GetPrivateProfileIntA(
				"Settings",
				"iObstructionHandling",
				1,
				kIniPath) != 0;

		g_obstructionRadiusPercent =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iObstructionRadiusPercent",
					70,
					kIniPath));

		g_fixFirstPersonBody =
			GetPrivateProfileIntA(
				"Settings",
				"iFixFirstPersonBody",
				1,
				kIniPath) != 0;

		g_firstPersonBodyPrepDelayMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iFirstPersonBodyPrepDelayMs",
					250,
					kIniPath));

		g_dialogueOpenCameraDelayMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iDialogueOpenCameraDelayMs",
					350,
					kIniPath));

		g_firstPersonRestoreDelayMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iFirstPersonRestoreDelayMs",
					120,
					kIniPath));

		g_fixPlayerHeadVisibility =
			GetPrivateProfileIntA(
				"Settings",
				"iFixPlayerHeadVisibility",
				1,
				kIniPath) != 0;

		g_usePlayerHeadFraming =
			GetPrivateProfileIntA(
				"Settings",
				"iUsePlayerHeadFraming",
				1,
				kIniPath) != 0;

		g_playerFramingHeightPercent =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iPlayerFramingHeightPercent",
					55,
					kIniPath));

		g_stablePlayerVisibility =
			GetPrivateProfileIntA(
				"Settings",
				"iStablePlayerVisibility",
				1,
				kIniPath) != 0;

		g_expressionNotifications =
			GetPrivateProfileIntA(
				"Settings",
				"iExpressionNotifications",
				1,
				kIniPath) != 0;

		g_expressionNotificationDurationMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iExpressionNotificationDurationMs",
					1400,
					kIniPath));

		g_exitVisibilityCleanupMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iExitVisibilityCleanupMs",
					1500,
					kIniPath));

		const auto legacyExitCleanupInterval =
			GetPrivateProfileIntA(
				"Settings",
				"iExitVisibilityCleanupIntervalMs",
				150,
				kIniPath);

		g_exitVisibilitySettleDelayMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iExitVisibilitySettleDelayMs",
					legacyExitCleanupInterval,
					kIniPath));

		g_obstructionCheckIntervalMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iObstructionCheckIntervalMs",
					100,
					kIniPath));

		g_headVisibilityCheckIntervalMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iHeadVisibilityCheckIntervalMs",
					250,
					kIniPath));

		g_expressionApplyDelayMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iExpressionApplyDelayMs",
					120,
					kIniPath));

		const bool legacyLockRequested =
			GetPrivateProfileIntA(
				"Settings",
				"iLockPlayerCamera",
				0,
				kIniPath) != 0;

		// The legacy hard lock skipped Starfield's dialogue-target resolver.
		// That resolver also performs dialogue/input bookkeeping, so bypassing it
		// can leave the player unable to move after dialogue. Retain the INI key
		// for backwards compatibility, but deliberately ignore it.
		g_lockPlayerCamera = false;
		if (legacyLockRequested) {
			REX::WARN(
				"iLockPlayerCamera is deprecated and ignored because the hard "
				"resolver lock can soft-lock player movement; use "
				"iSuppressCameraRecentering=1 instead");
		}

		g_forceSheatheOnDialogue =
			GetPrivateProfileIntA(
				"Settings",
				"iForceSheatheOnDialogue",
				1,
				kIniPath) != 0;

		g_forceThirdPersonOnDialogue =
			GetPrivateProfileIntA(
				"Settings",
				"iForceThirdPersonOnDialogue",
				1,
				kIniPath) != 0;

		g_suppressCameraRecentering =
			GetPrivateProfileIntA(
				"Settings",
				"iSuppressCameraRecentering",
				1,
				kIniPath) != 0;

		g_funMode =
			GetPrivateProfileIntA(
				"Settings",
				"iFunMode",
				0,
				kIniPath) != 0;

		g_cinematicPlayerLighting =
			GetPrivateProfileIntA(
				"Settings",
				"iCinematicPlayerLighting",
				1,
				kIniPath) != 0;

		LoadDefaultExpressionSetting();

		if (g_autoPlayerDelayMs > 5000) {
			g_autoPlayerDelayMs = 5000;
		}

		if (g_autoNpcDelayMs > 5000) {
			g_autoNpcDelayMs = 5000;
		}

		if (g_autoStartupGraceMs > 5000) {
			g_autoStartupGraceMs = 5000;
		}

		g_obstructionRadiusPercent =
			std::clamp(g_obstructionRadiusPercent, 20u, 150u);
		g_firstPersonBodyPrepDelayMs =
			std::clamp(g_firstPersonBodyPrepDelayMs, 32u, 1000u);
		g_dialogueOpenCameraDelayMs =
			std::clamp(g_dialogueOpenCameraDelayMs, 100u, 1500u);
		g_firstPersonRestoreDelayMs =
			std::min(g_firstPersonRestoreDelayMs, 2000u);
		g_expressionNotificationDurationMs =
			std::clamp(g_expressionNotificationDurationMs, 250u, 5000u);
		g_exitVisibilityCleanupMs =
			std::clamp(g_exitVisibilityCleanupMs, 250u, 5000u);
		g_exitVisibilitySettleDelayMs =
			std::clamp(g_exitVisibilitySettleDelayMs, 50u, 1000u);
		g_obstructionCheckIntervalMs =
			std::clamp(g_obstructionCheckIntervalMs, 33u, 500u);
		g_headVisibilityCheckIntervalMs =
			std::clamp(g_headVisibilityCheckIntervalMs, 50u, 1000u);
		g_expressionApplyDelayMs =
			std::clamp(g_expressionApplyDelayMs, 32u, 1000u);
		g_playerFramingHeightPercent =
			std::clamp(g_playerFramingHeightPercent, 20u, 100u);

		REX::INFO(
			"Settings: mode={}, manualKey={}, expressionKey={}, lightingToggleKey={}, "
			"playerDelay={}ms, npcDelay={}ms, startupGrace={}ms, "
			"obstruction={}, obstructionRadius={}%, firstPersonFix={}, "
			"bodyPrepDelay={}ms, dialogueOpenDelay={}ms, "
			"firstPersonRestoreDelay={}ms, "
			"headVisibilityFix={}, headFraming={}, framingHeight={}%, "
			"stableVisibility={}, expressionNotifications={}, "
			"notificationDuration={}ms, exitCleanup={}ms settle={}ms, "
			"obstructionCheck={}ms, headCheck={}ms, lockPlayerCamera={}, "
			"forceSheathe={}, forceThirdPerson={}, suppressRecentering={}, "
			"funMode={}, cinematicPlayerLighting={}, defaultExpression={}",
			g_switchMode == SwitchMode::kAutomatic ?
				"automatic" :
				"manual",
			g_manualSwitchKey,
			g_expressionCycleKey,
			g_cinematicLightingToggleKey,
			g_autoPlayerDelayMs,
			g_autoNpcDelayMs,
			g_autoStartupGraceMs,
			g_obstructionHandling ? "on" : "off",
			g_obstructionRadiusPercent,
			g_fixFirstPersonBody ? "on" : "off",
			g_firstPersonBodyPrepDelayMs,
			g_dialogueOpenCameraDelayMs,
			g_firstPersonRestoreDelayMs,
			g_fixPlayerHeadVisibility ? "on" : "off",
			g_usePlayerHeadFraming ? "on" : "off",
			g_playerFramingHeightPercent,
			g_stablePlayerVisibility ? "on" : "off",
			g_expressionNotifications ? "on" : "off",
			g_expressionNotificationDurationMs,
			g_exitVisibilityCleanupMs,
			g_exitVisibilitySettleDelayMs,
			g_obstructionCheckIntervalMs,
			g_headVisibilityCheckIntervalMs,
			g_lockPlayerCamera ? "on" : "off",
			g_forceSheatheOnDialogue ? "on" : "off",
			g_forceThirdPersonOnDialogue ? "on" : "off",
			g_suppressCameraRecentering ? "on" : "off",
			g_funMode ? "on" : "off",
			g_cinematicPlayerLighting ? "on" : "off",
			g_defaultExpressionIndex < kExpressionCount ?
				kExpressionNames[g_defaultExpressionIndex] :
				"disabled");

	}




	template <class T>
	[[nodiscard]] bool WriteBooleanSetting(
		T* setting,
		const bool value)
	{
		if (!setting) {
			return false;
		}

		if constexpr (requires(T& candidate, bool newValue) {
			candidate.SetBool(newValue);
		}) {
			setting->SetBool(value);
			return true;
		} else if constexpr (requires(T& candidate, bool newValue) {
			candidate.SetBoolean(newValue);
		}) {
			setting->SetBoolean(value);
			return true;
		} else if constexpr (requires(T& candidate, bool newValue) {
			candidate.SetValue(newValue);
		}) {
			setting->SetValue(value);
			return true;
		} else if constexpr (requires(T& candidate, bool newValue) {
			candidate.Set(newValue);
		}) {
			setting->Set(value);
			return true;
		} else if constexpr (requires(T& candidate, bool newValue) {
			candidate.data.b = newValue;
		}) {
			setting->data.b = value;
			return true;
		}

		return false;
	}

	void SetCinematicPlayerLighting(const bool enabled)
	{
		auto* collection = RE::INISettingCollection::GetSingleton();
		if (!collection) {
			if (!g_dialogueLightingWarningLogged.exchange(
					true,
					std::memory_order_acq_rel)) {
				REX::WARN(
					"Cinematic lighting unavailable: "
					"INISettingCollection singleton is null");
			}
			return;
		}

		auto* enableDialogueLight =
			collection->GetSetting("bEnableDialogueLight:Dialogue");
		auto* enableLightingRigSwitch =
			collection->GetSetting("bEnableLightingRigSwitch:Dialogue");
		auto* allowDialogueLightMovement =
			collection->GetSetting("bAllowDialogueLightMovement:Dialogue");
		auto* removeLightWhenNoMatch =
			collection->GetSetting("bDialogueLightRemoveNoMatch:Dialogue");

		if (!enableDialogueLight ||
			!enableLightingRigSwitch ||
			!allowDialogueLightMovement ||
			!removeLightWhenNoMatch) {
			if (!g_dialogueLightingWarningLogged.exchange(
					true,
					std::memory_order_acq_rel)) {
				REX::WARN(
					"Cinematic lighting unavailable: one or more "
					"native Dialogue settings could not be resolved");
			}
			return;
		}

		const bool lightWritten =
			WriteBooleanSetting(enableDialogueLight, enabled);
		const bool rigSwitchWritten =
			WriteBooleanSetting(enableLightingRigSwitch, enabled);
		const bool movementWritten =
			WriteBooleanSetting(allowDialogueLightMovement, enabled);
		// Camera rebuilds can switch dialogue lighting rigs. Leaving stale
		// non-matching dialogue lights alive allows illumination to accumulate
		// across repeated player/NPC camera transitions. Always let Starfield
		// remove a dialogue light once its rig no longer matches.
		const bool noMatchWritten =
			WriteBooleanSetting(removeLightWhenNoMatch, true);

		if (!lightWritten ||
			!rigSwitchWritten ||
			!movementWritten ||
			!noMatchWritten) {
			if (!g_dialogueLightingWarningLogged.exchange(
					true,
					std::memory_order_acq_rel)) {
				REX::WARN(
					"Cinematic lighting could not write one or more "
					"native Dialogue settings");
			}
			return;
		}

		REX::INFO(
			"Cinematic dialogue lighting {}; stale-light cleanup=on",
			enabled ? "enabled" : "disabled");
	}

	[[nodiscard]] RE::TESObjectREFR* GetSpeakerReference()
	{
		auto* topicManager =
			RE::MenuTopicManager::GetSingleton();

		if (!topicManager) {
			return nullptr;
		}

		auto speaker = topicManager->speaker.get();
		return speaker ? speaker.get() : nullptr;
	}

	[[nodiscard]] bool IsSpeakerTalking(
		bool& validSpeaker)
	{
		auto* speaker = GetSpeakerReference();

		validSpeaker = speaker != nullptr;
		return speaker ? speaker->IsTalking() : false;
	}

	void ResetAutomaticState()
	{
		g_autoCandidateInitialized = false;
		g_autoCandidatePlayer = false;
		g_lastSpeakerTalking = false;
		g_autoCandidateSince = 0;
		g_autoIgnoreUntil = 0;
	}

	class TickGuard
	{
	public:
		TickGuard() :
			locked(!g_tickLock.test_and_set(std::memory_order_acquire))
		{}

		~TickGuard()
		{
			if (locked) {
				g_tickLock.clear(std::memory_order_release);
			}
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return locked;
		}

	private:
		bool locked;
	};

	[[nodiscard]] void* GetActiveDialogueState()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return nullptr;
		}

		const auto dialogueIndex =
			static_cast<std::size_t>(RE::CameraState::kDialogue);

		void* const dialogueState =
			camera->cameraStates[dialogueIndex];

		if (!dialogueState ||
			camera->currentState != dialogueState) {
			return nullptr;
		}

		return dialogueState;
	}

	// Live check of the DialogueMenu open state. Queued tasks (view toggles,
	// body-prep terminals) must use this rather than the g_dialogueOpen flag,
	// which is only updated once per tick and can be stale relative to a task
	// that was queued just before the player hit Tab/Escape. Rebuilding the
	// dialogue camera after the menu has closed is what soft-locks the player.
	[[nodiscard]] bool IsDialogueMenuOpen()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		static const RE::BSFixedString dialogueMenu{ kDialogueMenuName };
		return ui->IsMenuOpen(dialogueMenu);
	}


	[[nodiscard]] bool IsSpaceshipHudOpen()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}

		static const RE::BSFixedString spaceshipHudMenu{
			kSpaceshipHudMenuName
		};
		return ui->IsMenuOpen(spaceshipHudMenu);
	}

	void UpdateSpaceshipPilotingHistory()
	{
		if (IsSpaceshipHudOpen()) {
			g_lastSpaceshipHudSeenAt = GetTickCount64();
		}
	}

	[[nodiscard]] bool WasRecentlyPilotingShip()
	{
		if (IsSpaceshipHudOpen()) {
			return true;
		}

		if (g_lastSpaceshipHudSeenAt == 0) {
			return false;
		}

		const auto now = GetTickCount64();
		return now >= g_lastSpaceshipHudSeenAt &&
			now - g_lastSpaceshipHudSeenAt <=
				kRecentSpaceshipHudWindowMs;
	}

	[[nodiscard]] std::uint32_t ReadHandle(
		void* dialogueState,
		const std::ptrdiff_t offset)
	{
		const auto* field =
			reinterpret_cast<const std::uint32_t*>(
				static_cast<std::byte*>(dialogueState) + offset);

		return std::atomic_ref<const std::uint32_t>(*field).load(
			std::memory_order_acquire);
	}

	void WriteHandle(
		void* dialogueState,
		const std::ptrdiff_t offset,
		const std::uint32_t value)
	{
		auto* field =
			reinterpret_cast<std::uint32_t*>(
				static_cast<std::byte*>(dialogueState) + offset);

		std::atomic_ref<std::uint32_t>(*field).store(
			value,
			std::memory_order_release);
	}

	[[nodiscard]] std::uint32_t GetSpeakerHandle()
	{
		auto* topicManager = RE::MenuTopicManager::GetSingleton();
		return topicManager ?
			topicManager->speaker.get_handle() :
			0;
	}

	[[nodiscard]] std::uint32_t GetPlayerHandle()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		return player ? player->nativeHandle : 0;
	}

	// Version-tolerant weapon sheathe. CommonLibSF branches differ in what they
	// expose on Actor, so probe for a usable method at compile time and no-op if
	// none is present (rather than hard-coding an RVA that could crash on a
	// mismatched build). Returns true if a sheathe call was actually issued.
	template <class T>
	[[nodiscard]] bool SheatheActorWeaponImpl(T* actor)
	{
		if (!actor) {
			return false;
		}

		if constexpr (requires(T& a) { a.DrawWeaponMagicHands(false); }) {
			actor->DrawWeaponMagicHands(false);
			return true;
		} else if constexpr (requires(T& a) { a.DrawWeapon(false); }) {
			actor->DrawWeapon(false);
			return true;
		} else if constexpr (requires(T& a) { a.SheatheWeapon(); }) {
			actor->SheatheWeapon();
			return true;
		}

		return false;
	}

	// True only when we can positively confirm a weapon is drawn. Currently we
	// always issue the harmless sheathe rather than gating on this, but the
	// probe is kept for callers that want to avoid a redundant call.
	// (Intentionally omitted to keep the build warning-clean; re-add with a
	// [[maybe_unused]] template if a gated path is needed.)

	bool SheathePlayerWeapon(const char* phase)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}

		auto* actor = static_cast<RE::Actor*>(player);
		const bool issued = SheatheActorWeaponImpl(actor);

		if (issued) {
			REX::INFO("Weapon sheathe issued: {}", phase);
		} else {
			REX::WARN(
				"Weapon sheathe unavailable in this CommonLibSF branch: {}",
				phase);
		}

		return issued;
	}


	[[nodiscard]] RE::NiAVObject* GetReferenceRoot3D(
		RE::TESObjectREFR* reference)
	{
		if (!reference) {
			return nullptr;
		}

		auto loadedData = reference->loadedData.LockRead();
		auto* data = *loadedData;

		return data ? data->data3D.get() : nullptr;
	}

	[[nodiscard]] RE::NiAVObject* GetPlayerRoot3D()
	{
		return GetReferenceRoot3D(
			RE::PlayerCharacter::GetSingleton());
	}

	[[nodiscard]] RE::TESObjectREFR* RememberDialogueSpeakerReference()
	{
		auto* speaker = GetSpeakerReference();
		if (speaker) {
			g_lastDialogueSpeakerReference.store(
				reinterpret_cast<std::uintptr_t>(speaker),
				std::memory_order_release);
		}
		return speaker;
	}

	[[nodiscard]] RE::NiAVObject* RememberDialogueSpeakerRoot()
	{
		auto* root =
			GetReferenceRoot3D(RememberDialogueSpeakerReference());
		if (root) {
			g_lastDialogueSpeakerRoot.store(
				reinterpret_cast<std::uintptr_t>(root),
				std::memory_order_release);
		}
		return root;
	}

	void ResetPlayerHeadAnchorCache()
	{
		g_cachedPlayerHeadRoot.store(0, std::memory_order_release);
		g_cachedPlayerHeadObject.store(0, std::memory_order_release);
		g_cachedPlayerHeadNameIndex.store(-2, std::memory_order_release);
		g_nextHeadVisibilityCheckAt.store(0, std::memory_order_release);
	}


	struct PlayerHeadAnchor
	{
		RE::NiAVObject* object{ nullptr };
		RE::NiPoint3 point{};
		const char* name{ "fallback" };
	};

	constexpr const char* kPlayerHeadAnchorNames[] = {
		"C_Head",
		"C_Neck",
		"C_Neck1",
		"Head",
		"NPC Head [Head]",
		"NPC Head",
		"Head1",
		"Neck",
		"Neck1",
		"Face"
	};

	[[nodiscard]] PlayerHeadAnchor ResolvePlayerHeadAnchor(
		RE::NiAVObject* root)
	{
		PlayerHeadAnchor result{};
		if (!root) {
			return result;
		}

		const auto rootAddress =
			reinterpret_cast<std::uintptr_t>(root);
		if (g_cachedPlayerHeadRoot.load(std::memory_order_acquire) ==
			rootAddress) {
			const int cachedIndex =
				g_cachedPlayerHeadNameIndex.load(
					std::memory_order_acquire);
			auto* cachedObject =
				reinterpret_cast<RE::NiAVObject*>(
					g_cachedPlayerHeadObject.load(
						std::memory_order_acquire));

			if (cachedObject && cachedIndex >= 0 &&
				static_cast<std::size_t>(cachedIndex) <
					std::size(kPlayerHeadAnchorNames)) {
				result.object = cachedObject;
				result.point = cachedObject->world.translate;
				result.name = kPlayerHeadAnchorNames[cachedIndex];
				return result;
			}

			if (cachedIndex == -1) {
				result.point = root->worldBound.center;
				result.point.z +=
					root->worldBound.radius *
					(static_cast<float>(g_playerFramingHeightPercent) / 100.0F);
				return result;
			}
		}

		// Resolve the recursive skeleton name once per external body root.
		// The old version repeated this search on every dialogue-camera update.
		for (std::size_t index = 0;
			index < std::size(kPlayerHeadAnchorNames);
			++index) {
			const RE::BSFixedString name{ kPlayerHeadAnchorNames[index] };
			if (auto* object = root->GetObjectByName(name)) {
				g_cachedPlayerHeadObject.store(
					reinterpret_cast<std::uintptr_t>(object),
					std::memory_order_release);
				g_cachedPlayerHeadNameIndex.store(
					static_cast<int>(index),
					std::memory_order_release);
				g_cachedPlayerHeadRoot.store(
					rootAddress,
					std::memory_order_release);

				result.object = object;
				result.point = object->world.translate;
				result.name = kPlayerHeadAnchorNames[index];
				return result;
			}
		}

		g_cachedPlayerHeadObject.store(0, std::memory_order_release);
		g_cachedPlayerHeadNameIndex.store(-1, std::memory_order_release);
		g_cachedPlayerHeadRoot.store(rootAddress, std::memory_order_release);

		result.point = root->worldBound.center;
		result.point.z +=
			root->worldBound.radius *
			(static_cast<float>(g_playerFramingHeightPercent) / 100.0F);
		return result;
	}

	// Cache-free head/neck lookup for use on arbitrary (e.g. speaker) roots.
	// Deliberately does not touch the player-head anchor cache.
	[[nodiscard]] RE::NiAVObject* FindHeadNodeUncached(
		RE::NiAVObject* root)
	{
		if (!root) {
			return nullptr;
		}

		for (const char* const candidate : kPlayerHeadAnchorNames) {
			const RE::BSFixedString name{ candidate };
			if (auto* object = root->GetObjectByName(name)) {
				return object;
			}
		}

		return nullptr;
	}

	class ScopedPlayerFramingOverride
	{
	public:
		explicit ScopedPlayerFramingOverride(const char* phase)
		{
			if (!g_usePlayerHeadFraming ||
				!g_playerViewRequested.load(std::memory_order_acquire)) {
				return;
			}

			if (g_playerFramingOverrideLock.test_and_set(
					std::memory_order_acquire)) {
				return;
			}
			lockHeld = true;

			root = GetPlayerRoot3D();
			if (!root) {
				return;
			}

			const auto anchor = ResolvePlayerHeadAnchor(root);
			originalCenter = root->worldBound.center;
			root->worldBound.center = anchor.point;
			active = true;

			const auto count = g_headFramingLogCount.fetch_add(
				1,
				std::memory_order_acq_rel) + 1;
			if (count <= 8) {
				REX::INFO(
					"Player head framing {}: anchor={}, object={}, "
					"center=({:.2f},{:.2f},{:.2f})->({:.2f},{:.2f},{:.2f})",
					phase,
					anchor.name,
					static_cast<void*>(anchor.object),
					originalCenter.x,
					originalCenter.y,
					originalCenter.z,
					anchor.point.x,
					anchor.point.y,
					anchor.point.z);
			}
		}

		~ScopedPlayerFramingOverride()
		{
			if (active && root) {
				root->worldBound.center = originalCenter;
			}
			if (lockHeld) {
				g_playerFramingOverrideLock.clear(
					std::memory_order_release);
			}
		}

		ScopedPlayerFramingOverride(
			const ScopedPlayerFramingOverride&) = delete;
		ScopedPlayerFramingOverride& operator=(
			const ScopedPlayerFramingOverride&) = delete;

	private:
		RE::NiAVObject* root{ nullptr };
		RE::NiPoint3 originalCenter{};
		bool active{ false };
		bool lockHeld{ false };
	};

	[[nodiscard]] std::uint64_t ReadPlayerRootFlags(
		RE::NiAVObject* root)
	{
		if (!root) {
			return 0;
		}

		return std::atomic_ref<std::uint64_t>(root->flags).load(
			std::memory_order_acquire);
	}

	[[nodiscard]] bool IsReadableAddress(
		const void* address,
		const std::size_t size)
	{
		if (!address || size == 0) {
			return false;
		}

		MEMORY_BASIC_INFORMATION info{};
		if (VirtualQuery(address, &info, sizeof(info)) == 0) {
			return false;
		}

		if (info.State != MEM_COMMIT ||
			(info.Protect & PAGE_GUARD) != 0 ||
			info.Protect == PAGE_NOACCESS) {
			return false;
		}

		const DWORD protection = info.Protect & 0xFF;
		const bool readable =
			protection == PAGE_READONLY ||
			protection == PAGE_READWRITE ||
			protection == PAGE_WRITECOPY ||
			protection == PAGE_EXECUTE_READ ||
			protection == PAGE_EXECUTE_READWRITE ||
			protection == PAGE_EXECUTE_WRITECOPY;

		if (!readable) {
			return false;
		}

		const auto start =
			reinterpret_cast<std::uintptr_t>(address);
		const auto regionStart =
			reinterpret_cast<std::uintptr_t>(info.BaseAddress);
		const auto regionEnd =
			regionStart + info.RegionSize;

		return start >= regionStart &&
			start + size <= regionEnd;
	}

	[[nodiscard]] bool IsExecutableAddress(const void* address)
	{
		if (!address) {
			return false;
		}

		MEMORY_BASIC_INFORMATION info{};
		if (VirtualQuery(address, &info, sizeof(info)) == 0) {
			return false;
		}

		if (info.State != MEM_COMMIT ||
			(info.Protect & PAGE_GUARD) != 0 ||
			info.Protect == PAGE_NOACCESS) {
			return false;
		}

		const DWORD protection = info.Protect & 0xFF;

		return protection == PAGE_EXECUTE ||
			protection == PAGE_EXECUTE_READ ||
			protection == PAGE_EXECUTE_READWRITE ||
			protection == PAGE_EXECUTE_WRITECOPY;
	}


	[[nodiscard]] std::uint64_t PerfNowTicks()
	{
		LARGE_INTEGER value{};
		QueryPerformanceCounter(&value);
		return static_cast<std::uint64_t>(value.QuadPart);
	}

	[[nodiscard]] std::uint64_t PerfElapsedUs(const std::uint64_t startedAt)
	{
		static const std::uint64_t frequency = []() {
			LARGE_INTEGER value{};
			QueryPerformanceFrequency(&value);
			return static_cast<std::uint64_t>(
				value.QuadPart > 0 ? value.QuadPart : 1);
		}();

		const auto now = PerfNowTicks();
		const auto elapsed = now >= startedAt ? now - startedAt : 0;
		return (elapsed * 1000000ULL) / frequency;
	}

	void PerfUpdateMax(
		std::atomic_uint64_t& destination,
		const std::uint64_t value)
	{
		auto current = destination.load(std::memory_order_relaxed);
		while (value > current &&
			!destination.compare_exchange_weak(
				current,
				value,
				std::memory_order_relaxed,
				std::memory_order_relaxed)) {
		}
	}

	void PerfRecord(PerfCounter& counter, const std::uint64_t startedAt)
	{
		const auto durationUs = PerfElapsedUs(startedAt);
		counter.calls.fetch_add(1, std::memory_order_relaxed);
		counter.totalUs.fetch_add(durationUs, std::memory_order_relaxed);
		PerfUpdateMax(counter.maxUs, durationUs);
	}

	[[nodiscard]] double PerfAverageUs(const PerfCounter& counter)
	{
		const auto calls = counter.calls.load(std::memory_order_relaxed);
		return calls == 0 ?
			0.0 :
			static_cast<double>(
				counter.totalUs.load(std::memory_order_relaxed)) /
				static_cast<double>(calls);
	}

	void PerfResetCounter(PerfCounter& counter)
	{
		counter.calls.store(0, std::memory_order_relaxed);
		counter.totalUs.store(0, std::memory_order_relaxed);
		counter.maxUs.store(0, std::memory_order_relaxed);
	}

	void PerfObservePlayerRoot()
	{
		auto* root = GetPlayerRoot3D();
		const auto address = reinterpret_cast<std::uintptr_t>(root);
		const auto previous =
			g_perfLastPlayerRoot.exchange(address, std::memory_order_relaxed);

		if (previous != 0 && address != 0 && previous != address) {
			g_perfPlayerRootChanges.fetch_add(1, std::memory_order_relaxed);
			REX::INFO(
				"[PERF] player root changed {} -> {}",
				reinterpret_cast<void*>(previous),
				static_cast<void*>(root));
		}
	}

	void PerfObserveSpeakerRoot(RE::NiAVObject* root)
	{
		const auto address = reinterpret_cast<std::uintptr_t>(root);
		const auto previous =
			g_perfLastSpeakerRoot.exchange(address, std::memory_order_relaxed);

		if (previous != 0 && address != 0 && previous != address) {
			g_perfSpeakerRootChanges.fetch_add(1, std::memory_order_relaxed);
			REX::INFO(
				"[PERF] speaker root changed {} -> {}",
				reinterpret_cast<void*>(previous),
				static_cast<void*>(root));
		}
	}


	void ResetDialogueControllerDedupState()
	{
		g_primaryDialogueController.store(0, std::memory_order_relaxed);
		g_seenDialogueControllerCount.store(0, std::memory_order_relaxed);
		g_duplicateControllerUpdatesSuppressed.store(0, std::memory_order_relaxed);
		g_perfPrevDuplicateControllerUpdatesSuppressed.store(
			0,
			std::memory_order_relaxed);

		for (auto& slot : g_seenDialogueControllers) {
			slot.store(0, std::memory_order_relaxed);
		}
	}

	[[nodiscard]] std::uint64_t RegisterDialogueController(void* controller)
	{
		if (!controller) {
			return g_seenDialogueControllerCount.load(
				std::memory_order_relaxed);
		}

		const auto address =
			reinterpret_cast<std::uintptr_t>(controller);

		for (std::size_t index = 0;
			index < g_seenDialogueControllers.size();
			++index) {
			const auto existing =
				g_seenDialogueControllers[index].load(
					std::memory_order_relaxed);

			if (existing == address) {
				return g_seenDialogueControllerCount.load(
					std::memory_order_relaxed);
			}

			if (existing == 0) {
				std::uintptr_t expected = 0;
				if (g_seenDialogueControllers[index].compare_exchange_strong(
						expected,
						address,
						std::memory_order_relaxed,
						std::memory_order_relaxed)) {
					const auto count =
						g_seenDialogueControllerCount.fetch_add(
							1,
							std::memory_order_relaxed) + 1;

					REX::INFO(
						"[CTRL] New dialogue controller #{}: pointer={}",
						count,
						controller);

					return count;
				}
			}
		}

		return g_seenDialogueControllerCount.load(
			std::memory_order_relaxed);
	}

	[[nodiscard]] bool ShouldSuppressDuplicateDialogueController(
		void* controller)
	{
		if (!g_dialogueActiveForHooks.load(std::memory_order_acquire) ||
			!controller) {
			return false;
		}

		(void)RegisterDialogueController(controller);

		const auto address =
			reinterpret_cast<std::uintptr_t>(controller);

		auto primary =
			g_primaryDialogueController.load(
				std::memory_order_acquire);

		if (primary == 0) {
			std::uintptr_t expected = 0;
			if (g_primaryDialogueController.compare_exchange_strong(
					expected,
					address,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				REX::INFO(
					"[CTRL] Primary dialogue controller selected: {}",
					controller);
				return false;
			}

			primary = expected;
		}

		if (primary == address) {
			return false;
		}

		g_duplicateControllerUpdatesSuppressed.fetch_add(
			1,
			std::memory_order_relaxed);

		return true;
	}

	void ResetPerformanceDiagnosticsForDialogue()
	{
		ResetDialogueControllerDedupState();
		PerfResetCounter(g_perfTick);
		PerfResetCounter(g_perfDialogueUpdateHook);
		PerfResetCounter(g_perfControllerHook);
		PerfResetCounter(g_perfResolverHook);
		PerfResetCounter(g_perfSetRootAppCulled);
		PerfResetCounter(g_perfObstructionUpdate);
		PerfResetCounter(g_perfForceDialogueActorsVisible);
		PerfResetCounter(g_perfHeadVisibility);
		PerfResetCounter(g_perfBodyPreparation);
		PerfResetCounter(g_perfAutomaticCamera);

		g_perfVisibilityNoOp.store(0, std::memory_order_relaxed);
		g_perfVisibilityNativeCalls.store(0, std::memory_order_relaxed);
		g_perfVisibilityShowCalls.store(0, std::memory_order_relaxed);
		g_perfVisibilityCullCalls.store(0, std::memory_order_relaxed);
		g_perfPlayerVisibilityNativeCalls.store(0, std::memory_order_relaxed);
		g_perfSpeakerVisibilityNativeCalls.store(0, std::memory_order_relaxed);
		g_perfHeadVisibilityNativeCalls.store(0, std::memory_order_relaxed);
		g_perfObstructionChecks.store(0, std::memory_order_relaxed);
		g_perfObstructionHits.store(0, std::memory_order_relaxed);
		g_perfObstructionSpeakerCull.store(0, std::memory_order_relaxed);
		g_perfObstructionSpeakerRestore.store(0, std::memory_order_relaxed);
		g_perfResolverSuppressed.store(0, std::memory_order_relaxed);
		g_perfControllerPlayerOverrides.store(0, std::memory_order_relaxed);
		g_perfCameraRebuildCalls.store(0, std::memory_order_relaxed);
		g_perfCameraRebuildTotalUs.store(0, std::memory_order_relaxed);
		g_perfCameraRebuildMaxUs.store(0, std::memory_order_relaxed);
		g_perfViewTransitions.store(0, std::memory_order_relaxed);
		g_perfForceThirdPersonCalls.store(0, std::memory_order_relaxed);
		g_perfSetDialogueCameraStateCalls.store(0, std::memory_order_relaxed);
		g_perfExitCleanupUpdates.store(0, std::memory_order_relaxed);
		g_perfExitPreRenderRepairs.store(0, std::memory_order_relaxed);
		g_perfPlayerRootChanges.store(0, std::memory_order_relaxed);
		g_perfSpeakerRootChanges.store(0, std::memory_order_relaxed);
		g_perfLastPlayerRoot.store(0, std::memory_order_relaxed);
		g_perfLastSpeakerRoot.store(0, std::memory_order_relaxed);
		g_perfPrevTickCalls.store(0, std::memory_order_relaxed);
		g_perfPrevDialogueHookCalls.store(0, std::memory_order_relaxed);
		g_perfPrevControllerHookCalls.store(0, std::memory_order_relaxed);
		g_perfPrevResolverHookCalls.store(0, std::memory_order_relaxed);
		g_perfPrevVisibilityNativeCalls.store(0, std::memory_order_relaxed);
		g_perfPrevObstructionChecks.store(0, std::memory_order_relaxed);

		g_viewTransitionRetryAfter.store(0, std::memory_order_relaxed);
		g_viewTransitionRetryWarningArmed.store(false, std::memory_order_relaxed);
		g_perfViewTransitionRequests.store(0, std::memory_order_relaxed);
		g_perfViewTransitionCoalesced.store(0, std::memory_order_relaxed);
		g_perfViewTransitionBackoffSkipped.store(0, std::memory_order_relaxed);
		g_perfViewTransitionNoStateAborts.store(0, std::memory_order_relaxed);
		g_perfPrevViewTransitionRequests.store(0, std::memory_order_relaxed);
		g_perfPrevViewTransitionCoalesced.store(0, std::memory_order_relaxed);
		g_perfPrevViewTransitionBackoffSkipped.store(0, std::memory_order_relaxed);
		g_perfPrevViewTransitionNoStateAborts.store(0, std::memory_order_relaxed);

		g_perfLastSnapshotAt.store(GetTickCount64(), std::memory_order_relaxed);

		PerfObservePlayerRoot();
		PerfObserveSpeakerRoot(GetReferenceRoot3D(GetSpeakerReference()));
	}

	void LogPerformanceSnapshot(const char* phase)
	{
		const auto tickCalls = g_perfTick.calls.load(std::memory_order_relaxed);
		const auto dialogueCalls =
			g_perfDialogueUpdateHook.calls.load(std::memory_order_relaxed);
		const auto controllerCalls =
			g_perfControllerHook.calls.load(std::memory_order_relaxed);
		const auto resolverCalls =
			g_perfResolverHook.calls.load(std::memory_order_relaxed);
		const auto visibilityNative =
			g_perfVisibilityNativeCalls.load(std::memory_order_relaxed);
		const auto obstructionChecks =
			g_perfObstructionChecks.load(std::memory_order_relaxed);

		const auto tickHz =
			tickCalls - g_perfPrevTickCalls.exchange(
				tickCalls, std::memory_order_relaxed);
		const auto dialogueHz =
			dialogueCalls - g_perfPrevDialogueHookCalls.exchange(
				dialogueCalls, std::memory_order_relaxed);
		const auto controllerHz =
			controllerCalls - g_perfPrevControllerHookCalls.exchange(
				controllerCalls, std::memory_order_relaxed);
		const auto resolverHz =
			resolverCalls - g_perfPrevResolverHookCalls.exchange(
				resolverCalls, std::memory_order_relaxed);
		const auto visibilityPerSecond =
			visibilityNative - g_perfPrevVisibilityNativeCalls.exchange(
				visibilityNative, std::memory_order_relaxed);
		const auto obstructionPerSecond =
			obstructionChecks - g_perfPrevObstructionChecks.exchange(
				obstructionChecks, std::memory_order_relaxed);

		const auto duplicateSuppressed =
			g_duplicateControllerUpdatesSuppressed.load(
				std::memory_order_relaxed);
		const auto duplicateSuppressedPerSecond =
			duplicateSuppressed -
				g_perfPrevDuplicateControllerUpdatesSuppressed.exchange(
					duplicateSuppressed,
					std::memory_order_relaxed);

		const auto transitionRequests =
			g_perfViewTransitionRequests.load(std::memory_order_relaxed);
		const auto transitionCoalesced =
			g_perfViewTransitionCoalesced.load(std::memory_order_relaxed);
		const auto transitionBackoffSkipped =
			g_perfViewTransitionBackoffSkipped.load(std::memory_order_relaxed);
		const auto transitionNoStateAborts =
			g_perfViewTransitionNoStateAborts.load(std::memory_order_relaxed);

		const auto transitionRequestsPerSecond =
			transitionRequests -
				g_perfPrevViewTransitionRequests.exchange(
					transitionRequests,
					std::memory_order_relaxed);
		const auto transitionCoalescedPerSecond =
			transitionCoalesced -
				g_perfPrevViewTransitionCoalesced.exchange(
					transitionCoalesced,
					std::memory_order_relaxed);
		const auto transitionBackoffSkippedPerSecond =
			transitionBackoffSkipped -
				g_perfPrevViewTransitionBackoffSkipped.exchange(
					transitionBackoffSkipped,
					std::memory_order_relaxed);
		const auto transitionNoStateAbortsPerSecond =
			transitionNoStateAborts -
				g_perfPrevViewTransitionNoStateAborts.exchange(
					transitionNoStateAborts,
					std::memory_order_relaxed);

		const auto rebuildCalls =
			g_perfCameraRebuildCalls.load(std::memory_order_relaxed);
		const double rebuildAverageUs =
			rebuildCalls == 0 ?
				0.0 :
				static_cast<double>(
					g_perfCameraRebuildTotalUs.load(std::memory_order_relaxed)) /
					static_cast<double>(rebuildCalls);

		auto* playerRoot = GetPlayerRoot3D();
		auto* speakerRoot = GetReferenceRoot3D(GetSpeakerReference());
		PerfObservePlayerRoot();
		PerfObserveSpeakerRoot(speakerRoot);

		REX::INFO(
			"[PERF] {} dlg#{} view={} tickHz={} dialogueHz={} controllerHz={} "
			"resolverHz={} nativeVisibility/s={} obstructionChecks/s={} "
			"controllers={} duplicateSuppressed/s={} duplicateSuppressed={} "
			"transitionReq/s={} coalesced/s={} backoffSkip/s={} noStateAbort/s={} | "
			"tick avg={:.1f}us max={} dialogue avg={:.1f}us max={} "
			"controller avg={:.1f}us max={} resolver avg={:.1f}us max={} | "
			"visibility avg={:.1f}us max={} native={} noop={} show={} cull={} "
			"[player={},speaker={},head={}] | "
			"obstruction checks={} hits={} culls={} restores={} avg={:.1f}us max={} | "
			"rebuilds={} avg={:.1f}us max={} transitions={} "
			"resolverSuppressed={} controllerPlayerOverrides={} | "
			"bodyPrep avg={:.1f}us max={} force3P={} setDialogueState={} | "
			"forceVisible calls={} avg={:.1f}us max={} exitUpdates={} "
			"preRenderRepairs={} | playerRoot={} changes={} speakerRoot={} changes={}",
			phase ? phase : "snapshot",
			g_perfDialogueIndex.load(std::memory_order_relaxed),
			g_playerViewRequested.load(std::memory_order_relaxed) ? "player" : "npc",
			tickHz,
			dialogueHz,
			controllerHz,
			resolverHz,
			visibilityPerSecond,
			obstructionPerSecond,
			g_seenDialogueControllerCount.load(std::memory_order_relaxed),
			duplicateSuppressedPerSecond,
			duplicateSuppressed,
			transitionRequestsPerSecond,
			transitionCoalescedPerSecond,
			transitionBackoffSkippedPerSecond,
			transitionNoStateAbortsPerSecond,
			PerfAverageUs(g_perfTick),
			g_perfTick.maxUs.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfDialogueUpdateHook),
			g_perfDialogueUpdateHook.maxUs.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfControllerHook),
			g_perfControllerHook.maxUs.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfResolverHook),
			g_perfResolverHook.maxUs.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfSetRootAppCulled),
			g_perfSetRootAppCulled.maxUs.load(std::memory_order_relaxed),
			visibilityNative,
			g_perfVisibilityNoOp.load(std::memory_order_relaxed),
			g_perfVisibilityShowCalls.load(std::memory_order_relaxed),
			g_perfVisibilityCullCalls.load(std::memory_order_relaxed),
			g_perfPlayerVisibilityNativeCalls.load(std::memory_order_relaxed),
			g_perfSpeakerVisibilityNativeCalls.load(std::memory_order_relaxed),
			g_perfHeadVisibilityNativeCalls.load(std::memory_order_relaxed),
			obstructionChecks,
			g_perfObstructionHits.load(std::memory_order_relaxed),
			g_perfObstructionSpeakerCull.load(std::memory_order_relaxed),
			g_perfObstructionSpeakerRestore.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfObstructionUpdate),
			g_perfObstructionUpdate.maxUs.load(std::memory_order_relaxed),
			rebuildCalls,
			rebuildAverageUs,
			g_perfCameraRebuildMaxUs.load(std::memory_order_relaxed),
			g_perfViewTransitions.load(std::memory_order_relaxed),
			g_perfResolverSuppressed.load(std::memory_order_relaxed),
			g_perfControllerPlayerOverrides.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfBodyPreparation),
			g_perfBodyPreparation.maxUs.load(std::memory_order_relaxed),
			g_perfForceThirdPersonCalls.load(std::memory_order_relaxed),
			g_perfSetDialogueCameraStateCalls.load(std::memory_order_relaxed),
			g_perfForceDialogueActorsVisible.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfForceDialogueActorsVisible),
			g_perfForceDialogueActorsVisible.maxUs.load(std::memory_order_relaxed),
			g_perfExitCleanupUpdates.load(std::memory_order_relaxed),
			g_perfExitPreRenderRepairs.load(std::memory_order_relaxed),
			static_cast<void*>(playerRoot),
			g_perfPlayerRootChanges.load(std::memory_order_relaxed),
			static_cast<void*>(speakerRoot),
			g_perfSpeakerRootChanges.load(std::memory_order_relaxed));
	}

	void UpdatePerformanceSnapshot()
	{
		if (!g_dialogueOpen ||
			g_shipDialogueBypass.load(std::memory_order_relaxed)) {
			return;
		}

		const auto now = GetTickCount64();
		auto previous = g_perfLastSnapshotAt.load(std::memory_order_relaxed);
		if (now - previous < 1000) {
			return;
		}

		if (!g_perfLastSnapshotAt.compare_exchange_strong(
				previous, now, std::memory_order_relaxed)) {
			return;
		}

		LogPerformanceSnapshot("1s");
	}


	[[nodiscard]] ChangeAnimFaceArchetype_t
	GetChangeAnimFaceArchetypeFunction()
	{
		const auto moduleBase =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr));

		if (moduleBase == 0) {
			return nullptr;
		}

		auto function =
			reinterpret_cast<ChangeAnimFaceArchetype_t>(
				moduleBase +
				kChangeAnimFaceArchetypeRva);

		return IsExecutableAddress(
				reinterpret_cast<const void*>(function)) ?
			function :
			nullptr;
	}


	[[nodiscard]] RE::BGSKeyword* ResolveExpressionKeyword(
		const char* expression,
		std::string& editorID)
	{
		editorID = "AnimFaceArchetype";
		editorID += expression;

		const RE::BSFixedString fixedEditorID{
			editorID.c_str()
		};

		auto* rawForm =
			RE::TESForm::LookupByEditorID(fixedEditorID);

		if (!rawForm) {
			return nullptr;
		}

		return rawForm->As<RE::BGSKeyword>();
	}

	[[nodiscard]] std::uint32_t NextFunRandomValue()
	{
		auto state = g_funRandomState;

		if (state == 0) {
			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);

			state =
				static_cast<std::uint32_t>(counter.QuadPart) ^
				static_cast<std::uint32_t>(counter.QuadPart >> 32) ^
				static_cast<std::uint32_t>(GetTickCount64()) ^
				GetCurrentProcessId() ^
				GetCurrentThreadId();

			if (state == 0) {
				state = 0xA341316Cu;
			}
		}

		// Xorshift32 is sufficient here. We need varied expressions, not a
		// cryptographically defensible grimace selection process.
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		g_funRandomState = state;
		return state;
	}

	LRESULT CALLBACK ExpressionNotificationWindowProc(
		HWND window,
		UINT message,
		WPARAM wParam,
		LPARAM lParam)
	{
		switch (message) {
		case WM_CREATE:
			g_expressionNotificationFont = CreateFontW(
				28,
				0,
				0,
				0,
				FW_SEMIBOLD,
				FALSE,
				FALSE,
				FALSE,
				DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY,
				DEFAULT_PITCH | FF_DONTCARE,
				L"Segoe UI");
			g_expressionNotificationBrush =
				CreateSolidBrush(RGB(12, 12, 12));
			return 0;

		case kExpressionNotificationMessage:
		{
			auto* text = reinterpret_cast<std::wstring*>(lParam);
			if (text) {
				g_expressionNotificationText = std::move(*text);
				delete text;
			}

			KillTimer(window, kExpressionNotificationTimerId);
			SetTimer(
				window,
				kExpressionNotificationTimerId,
				g_expressionNotificationDurationMs,
				nullptr);
			SetWindowPos(
				window,
				HWND_TOPMOST,
				32,
				72,
				520,
				58,
				SWP_NOACTIVATE | SWP_SHOWWINDOW);
			InvalidateRect(window, nullptr, TRUE);
			return 0;
		}

		case WM_TIMER:
			if (wParam == kExpressionNotificationTimerId) {
				KillTimer(window, kExpressionNotificationTimerId);
				ShowWindow(window, SW_HIDE);
				return 0;
			}
			break;

		case WM_NCHITTEST:
			return HTTRANSPARENT;

		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT paint{};
			auto device = BeginPaint(window, &paint);
			RECT client{};
			GetClientRect(window, &client);

			FillRect(
				device,
				&client,
				g_expressionNotificationBrush ?
					g_expressionNotificationBrush :
					GetSysColorBrush(COLOR_WINDOW));

			auto previousFont =
				g_expressionNotificationFont ?
					SelectObject(device, g_expressionNotificationFont) :
					nullptr;
			SetBkMode(device, TRANSPARENT);
			SetTextColor(device, RGB(245, 245, 245));

			RECT textRect{
				18,
				0,
				client.right - 18,
				client.bottom
			};
			DrawTextW(
				device,
				g_expressionNotificationText.c_str(),
				-1,
				&textRect,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

			if (previousFont) {
				SelectObject(device, previousFont);
			}
			EndPaint(window, &paint);
			return 0;
		}

		case WM_DESTROY:
			if (g_expressionNotificationFont) {
				DeleteObject(g_expressionNotificationFont);
				g_expressionNotificationFont = nullptr;
			}
			if (g_expressionNotificationBrush) {
				DeleteObject(g_expressionNotificationBrush);
				g_expressionNotificationBrush = nullptr;
			}
			g_expressionNotificationWindow.store(
				0,
				std::memory_order_release);
			PostQuitMessage(0);
			return 0;
		}

		return DefWindowProcW(window, message, wParam, lParam);
	}

	DWORD WINAPI ExpressionNotificationThreadProc(void*)
	{
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.lpfnWndProc = ExpressionNotificationWindowProc;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
		windowClass.lpszClassName = kExpressionNotificationWindowClass;

		RegisterClassExW(&windowClass);

		auto window = CreateWindowExW(
			WS_EX_TOPMOST |
				WS_EX_TOOLWINDOW |
				WS_EX_NOACTIVATE |
				WS_EX_LAYERED |
				WS_EX_TRANSPARENT,
			kExpressionNotificationWindowClass,
			L"",
			WS_POPUP,
			32,
			72,
			520,
			58,
			nullptr,
			nullptr,
			windowClass.hInstance,
			nullptr);

		if (!window) {
			REX::WARN(
				"Expression notification thread could not create overlay window");
			g_expressionNotificationThreadStarted.store(
				false,
				std::memory_order_release);
			return 0;
		}

		SetLayeredWindowAttributes(
			window,
			0,
			225,
			LWA_ALPHA);
		g_expressionNotificationWindow.store(
			reinterpret_cast<std::uintptr_t>(window),
			std::memory_order_release);

		MSG message{};
		while (GetMessageW(&message, nullptr, 0, 0) > 0) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}

		return 0;
	}

	void StartExpressionNotificationThread()
	{
		if (!g_expressionNotifications) {
			return;
		}

		bool expected = false;
		if (!g_expressionNotificationThreadStarted.compare_exchange_strong(
				expected,
				true,
				std::memory_order_acq_rel)) {
			return;
		}

		auto thread = CreateThread(
			nullptr,
			0,
			ExpressionNotificationThreadProc,
			nullptr,
			0,
			nullptr);

		if (!thread) {
			g_expressionNotificationThreadStarted.store(
				false,
				std::memory_order_release);
			REX::WARN(
				"Expression notification thread could not be started");
			return;
		}

		CloseHandle(thread);
	}

	void ShowExpressionNotification(const char* expression)
	{
		if (!g_expressionNotifications || !expression) {
			return;
		}

		StartExpressionNotificationThread();

		auto window = reinterpret_cast<HWND>(
			g_expressionNotificationWindow.load(
				std::memory_order_acquire));

		if (!window) {
			REX::INFO(
				"Expression notification deferred because overlay is not ready: {}",
				expression);
			return;
		}

		std::wstring message{ L"Expression: " };
		for (const unsigned char character :
			std::string_view{ expression }) {
			message.push_back(static_cast<wchar_t>(character));
		}

		auto* payload =
			new (std::nothrow) std::wstring{ std::move(message) };
		if (!payload) {
			return;
		}

		if (!PostMessageW(
				window,
				kExpressionNotificationMessage,
				0,
				reinterpret_cast<LPARAM>(payload))) {
			delete payload;
			REX::WARN(
				"Expression notification could not be posted: {}",
				expression);
		}
	}

	void ClearPlayerExpressionOverride(
		const char* phase)
	{
		if (!g_expressionOverrideActive.exchange(
				false,
				std::memory_order_acq_rel)) {
			return;
		}

		auto* player =
			RE::PlayerCharacter::GetSingleton();
		const auto changeExpression =
			GetChangeAnimFaceArchetypeFunction();

		if (!player || !changeExpression) {
			REX::WARN(
				"Expression reset {} failed: player or native unavailable",
				phase);
			return;
		}

		// Passing no keyword is the native/Papyrus reset path. It returns
		// ownership of the face archetype to dialogue and animation logic.
		changeExpression(
			static_cast<RE::Actor*>(player),
			nullptr);
		g_activeExpressionKeyword.store(
			0,
			std::memory_order_release);

		REX::INFO("Expression override cleared: {}", phase);
	}

	void ReapplyPlayerExpressionOverride(const char* phase)
	{
		auto* keyword = reinterpret_cast<RE::BGSKeyword*>(
			g_activeExpressionKeyword.load(
				std::memory_order_acquire));

		if (!keyword ||
			!g_expressionOverrideActive.load(
				std::memory_order_acquire)) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto changeExpression =
			GetChangeAnimFaceArchetypeFunction();

		if (!player || !changeExpression) {
			return;
		}

		changeExpression(
			static_cast<RE::Actor*>(player),
			keyword);

		REX::INFO("Expression override reapplied: {}", phase);
	}

	[[nodiscard]] bool ApplyConfiguredDefaultExpression()
	{
		if (g_defaultExpressionIndex >= kExpressionCount) {
			return false;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto changeExpression =
			GetChangeAnimFaceArchetypeFunction();

		if (!player || !changeExpression) {
			REX::WARN(
				"Default expression skipped: player or native unavailable");
			return false;
		}

		const char* const expression =
			kExpressionNames[g_defaultExpressionIndex];

		if (g_defaultExpressionIndex == 0) {
			changeExpression(
				static_cast<RE::Actor*>(player),
				nullptr);
			g_activeExpressionKeyword.store(
				0,
				std::memory_order_release);
			g_expressionOverrideActive.store(
				false,
				std::memory_order_release);

			ShowExpressionNotification(expression);
			REX::INFO(
				"Default expression dispatched: Neutral/reset");
			return true;
		}

		std::string editorID;
		auto* keyword = ResolveExpressionKeyword(
			expression,
			editorID);

		if (!keyword) {
			REX::WARN(
				"Default expression skipped: {} did not resolve to BGSKeyword",
				editorID);
			return false;
		}

		changeExpression(
			static_cast<RE::Actor*>(player),
			keyword);

		g_activeExpressionKeyword.store(
			reinterpret_cast<std::uintptr_t>(keyword),
			std::memory_order_release);
		g_expressionOverrideActive.store(
			true,
			std::memory_order_release);

		ShowExpressionNotification(expression);

		REX::INFO(
			"Default expression dispatched: {}, keyword={}, formID=0x{:08X}",
			expression,
			static_cast<void*>(keyword),
			keyword->GetFormID());
		return true;
	}

	[[nodiscard]] bool ApplyRandomFunExpression()
	{
		if (!g_funMode || kExpressionCount <= 1) {
			return false;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto changeExpression =
			GetChangeAnimFaceArchetypeFunction();

		if (!player || !changeExpression) {
			REX::WARN(
				"Fun mode expression skipped: player or native unavailable");
			return false;
		}

		// Index 0 is Neutral/reset. Fun mode deliberately chooses a visible
		// expression on every player shot rather than occasionally doing nothing.
		constexpr std::size_t firstExpressionIndex = 1;
		constexpr std::size_t selectableExpressionCount =
			kExpressionCount - firstExpressionIndex;

		for (std::size_t attempt = 0;
			attempt < kExpressionCount * 2;
			++attempt) {
			const auto index =
				firstExpressionIndex +
				(NextFunRandomValue() % selectableExpressionCount);

			if (selectableExpressionCount > 1 &&
				index == g_lastFunExpressionIndex) {
				continue;
			}

			const char* const expression = kExpressionNames[index];
			std::string editorID;
			auto* keyword =
				ResolveExpressionKeyword(
					expression,
					editorID);

			if (!keyword) {
				REX::WARN(
					"Fun mode expression skipped: {} did not resolve to BGSKeyword",
					editorID);
				continue;
			}

			changeExpression(
				static_cast<RE::Actor*>(player),
				keyword);

			g_activeExpressionKeyword.store(
				reinterpret_cast<std::uintptr_t>(keyword),
				std::memory_order_release);
			g_expressionOverrideActive.store(
				true,
				std::memory_order_release);
			g_lastFunExpressionIndex = index;

			ShowExpressionNotification(expression);

			REX::INFO(
				"Fun mode expression dispatched: {} ({}/{}), keyword={}, "
				"formID=0x{:08X}",
				expression,
				index + 1,
				kExpressionCount,
				static_cast<void*>(keyword),
				keyword->GetFormID());
			return true;
		}

		REX::WARN(
			"Fun mode could not resolve a random facial archetype keyword");
		return false;
	}


	void CancelPendingPlayerShotExpression(const char* phase)
	{
		if (g_pendingPlayerShotExpression.exchange(
				false,
				std::memory_order_acq_rel)) {
			REX::INFO("Pending player-shot expression cancelled: {}", phase);
		}
		g_pendingPlayerShotExpressionAt.store(
			0,
			std::memory_order_release);
	}

	void SchedulePlayerShotExpression()
	{
		const bool hasConfiguredExpression =
			g_funMode || g_defaultExpressionIndex < kExpressionCount;

		if (!hasConfiguredExpression) {
			return;
		}

		g_pendingPlayerShotExpressionAt.store(
			GetTickCount64() + g_expressionApplyDelayMs,
			std::memory_order_release);
		g_pendingPlayerShotExpression.store(
			true,
			std::memory_order_release);

		REX::INFO(
			"Player-shot expression scheduled after {}ms",
			g_expressionApplyDelayMs);
	}

	void PerformPendingPlayerShotExpressionOnTaskQueue()
	{
		struct CompletionGuard
		{
			~CompletionGuard()
			{
				g_automaticExpressionTaskQueued.store(
					false,
					std::memory_order_release);
			}
		} completionGuard;

		if (!g_dialogueOpen ||
			!g_playerViewRequested.load(std::memory_order_acquire)) {
			return;
		}

		bool expressionApplied = false;
		if (g_funMode) {
			expressionApplied = ApplyRandomFunExpression();
		} else {
			expressionApplied = ApplyConfiguredDefaultExpression();
		}

		if (!expressionApplied) {
			ReapplyPlayerExpressionOverride("delayed player camera expression");
		}
	}

	void UpdatePendingPlayerShotExpression()
	{
		if (!g_pendingPlayerShotExpression.load(
				std::memory_order_acquire)) {
			return;
		}

		if (!g_dialogueOpen ||
			!g_playerViewRequested.load(std::memory_order_acquire)) {
			CancelPendingPlayerShotExpression("view no longer player");
			return;
		}

		const auto applyAt =
			g_pendingPlayerShotExpressionAt.load(
				std::memory_order_acquire);
		if (GetTickCount64() < applyAt) {
			return;
		}

		bool expected = false;
		if (!g_automaticExpressionTaskQueued.compare_exchange_strong(
				expected,
				true,
				std::memory_order_acq_rel)) {
			return;
		}

		g_pendingPlayerShotExpression.store(
			false,
			std::memory_order_release);
		g_pendingPlayerShotExpressionAt.store(
			0,
			std::memory_order_release);

		if (!g_taskInterface) {
			g_automaticExpressionTaskQueued.store(
				false,
				std::memory_order_release);
			return;
		}

		g_taskInterface->AddTask([]() {
			PerformPendingPlayerShotExpressionOnTaskQueue();
		});
	}

	void PerformExpressionCycleOnTaskQueue()
	{
		struct CompletionGuard
		{
			~CompletionGuard()
			{
				g_expressionActionQueued.store(
					false,
					std::memory_order_release);
			}
		} completionGuard;

		auto* ui = RE::UI::GetSingleton();
		static const RE::BSFixedString dialogueMenu{
			kDialogueMenuName
		};

		if (!ui || !ui->IsMenuOpen(dialogueMenu)) {
			REX::WARN(
				"Expression cycle aborted: dialogue is no longer open");
			return;
		}

		auto* player =
			RE::PlayerCharacter::GetSingleton();

		if (!player) {
			REX::WARN(
				"Expression cycle aborted: player is unavailable");
			return;
		}

		const auto changeExpression =
			GetChangeAnimFaceArchetypeFunction();

		if (!changeExpression) {
			REX::CRITICAL(
				"Expression cycle aborted: native wrapper RVA "
				"0x{:X} is unavailable",
				kChangeAnimFaceArchetypeRva);
			return;
		}

		// Skip missing editor IDs rather than getting permanently stuck on
		// one expression when Bethesda changes a keyword between versions.
		for (std::size_t attempt = 0;
			attempt < kExpressionCount;
			++attempt) {
			const auto index =
				g_expressionIndex % kExpressionCount;
			const char* const expression =
				kExpressionNames[index];

			g_expressionIndex =
				(index + 1) % kExpressionCount;

			if (std::string_view(expression) == "Neutral") {
				changeExpression(
					static_cast<RE::Actor*>(player),
					nullptr);
				g_expressionOverrideActive.store(
					false,
					std::memory_order_release);
				g_activeExpressionKeyword.store(
					0,
					std::memory_order_release);

				ShowExpressionNotification("Neutral");
				REX::INFO(
					"Expression dispatched: Neutral/reset ({}/{})",
					index + 1,
					kExpressionCount);
				return;
			}

			std::string editorID;
			auto* keyword =
				ResolveExpressionKeyword(
					expression,
					editorID);

			if (!keyword) {
				REX::WARN(
					"Expression skipped: {} did not resolve to BGSKeyword",
					editorID);
				continue;
			}

			changeExpression(
				static_cast<RE::Actor*>(player),
				keyword);

			g_activeExpressionKeyword.store(
				reinterpret_cast<std::uintptr_t>(keyword),
				std::memory_order_release);
			g_expressionOverrideActive.store(
				true,
				std::memory_order_release);

			ShowExpressionNotification(expression);

			REX::INFO(
				"Expression dispatched: {} ({}/{}), keyword={}, "
				"formID=0x{:08X}, type={}, wrapperRVA=0x{:X}",
				expression,
				index + 1,
				kExpressionCount,
				static_cast<void*>(keyword),
				keyword->GetFormID(),
				static_cast<std::uint32_t>(
					keyword->GetFormType()),
				kChangeAnimFaceArchetypeRva);
			return;
		}

		REX::CRITICAL(
			"Expression cycle failed: no facial archetype keyword resolved");
	}

	void QueueExpressionCycle()
	{
		CancelPendingPlayerShotExpression("manual expression cycle");
		if (!g_taskInterface) {
			REX::CRITICAL(
				"Expression cycle skipped: task interface unavailable");
			return;
		}

		bool expected = false;
		if (!g_expressionActionQueued.compare_exchange_strong(
				expected,
				true,
				std::memory_order_acq_rel)) {
			return;
		}

		g_taskInterface->AddTask([]() {
			PerformExpressionCycleOnTaskQueue();
		});
	}

	[[nodiscard]] bool SetRootAppCulled(
		RE::NiAVObject* root,
		const bool culled,
		const char* subject,
		const char* phase)
	{
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			~PerfScope() { PerfRecord(g_perfSetRootAppCulled, startedAt); }
		} perfScope{ perfStartedAt };
		if (!root ||
			!IsReadableAddress(root, sizeof(void*))) {
			if (!g_visibilityCallFailed.exchange(
					true,
					std::memory_order_acq_rel)) {
				REX::WARN(
					"Native visibility {} {} skipped: 3D root is null or unreadable",
					phase,
					subject);
			}
			return false;
		}

		const auto beforeFlags = ReadPlayerRootFlags(root);
		const bool alreadyCulled =
			(beforeFlags & kAppCulledBit) != 0;

		// Most visibility requests are no-ops. Return before walking the
		// vtable or calling VirtualQuery on its target function.
		if (alreadyCulled == culled) {
			g_perfVisibilityNoOp.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		auto** vtable = *reinterpret_cast<void***>(root);
		if (!vtable) {
			if (!g_visibilityCallFailed.exchange(
					true,
					std::memory_order_acq_rel)) {
				REX::WARN(
					"Native visibility {} {} skipped: root vtable is null",
					phase,
					subject);
			}
			return false;
		}

		void* const rawFunction =
			vtable[kSetAppCulledVtableIndex];

		if (!IsExecutableAddress(rawFunction)) {
			if (!g_visibilityCallFailed.exchange(
					true,
					std::memory_order_acq_rel)) {
				REX::WARN(
					"Native visibility {} {} skipped: vtable[{}]={} "
					"is not executable",
					phase,
					subject,
					kSetAppCulledVtableIndex,
					rawFunction);
			}
			return false;
		}

		auto setAppCulled =
			reinterpret_cast<SetAppCulled_t>(rawFunction);

		setAppCulled(root, culled);

		g_perfVisibilityNativeCalls.fetch_add(1, std::memory_order_relaxed);
		if (culled) {
			g_perfVisibilityCullCalls.fetch_add(1, std::memory_order_relaxed);
		} else {
			g_perfVisibilityShowCalls.fetch_add(1, std::memory_order_relaxed);
		}
		const std::string_view perfSubject{ subject ? subject : "" };
		if (perfSubject == "player") {
			g_perfPlayerVisibilityNativeCalls.fetch_add(1, std::memory_order_relaxed);
		} else if (perfSubject == "speaker") {
			g_perfSpeakerVisibilityNativeCalls.fetch_add(1, std::memory_order_relaxed);
		} else if (perfSubject.find("head") != std::string_view::npos) {
			g_perfHeadVisibilityNativeCalls.fetch_add(1, std::memory_order_relaxed);
		}

		const auto afterFlags = ReadPlayerRootFlags(root);

		const auto count =
			g_visibilityHookLogCount.fetch_add(
				1,
				std::memory_order_acq_rel) + 1;

		if (count <= 20) {
			REX::INFO(
				"Native visibility {} {} {}: root={}, vfunc={}, "
				"flags 0x{:016X}->0x{:016X}, appCulled {}->{}",
				phase,
				subject,
				culled ? "CULL" : "SHOW",
				static_cast<void*>(root),
				rawFunction,
				beforeFlags,
				afterFlags,
				(beforeFlags & kAppCulledBit) != 0 ? 1 : 0,
				(afterFlags & kAppCulledBit) != 0 ? 1 : 0);
		}

		return true;
	}

	[[nodiscard]] bool SetPlayerRootAppCulled(
		const bool culled,
		const char* phase)
	{
		return SetRootAppCulled(
			GetPlayerRoot3D(),
			culled,
			"player",
			phase);
	}



	void EnsurePlayerHeadVisible(const char* phase)
	{
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			~PerfScope() { PerfRecord(g_perfHeadVisibility, startedAt); }
		} perfScope{ perfStartedAt };
		if (!g_fixPlayerHeadVisibility ||
			!g_playerViewRequested.load(std::memory_order_acquire)) {
			return;
		}

		auto* root = GetPlayerRoot3D();
		if (!root) {
			return;
		}

		const auto anchor = ResolvePlayerHeadAnchor(root);
		if (!anchor.object) {
			const auto count = g_headVisibilityLogCount.fetch_add(
				1,
				std::memory_order_acq_rel) + 1;
			if (count == 1) {
				REX::WARN(
					"Player head visibility {}: no named head/neck node found; "
					"using framing fallback only",
					phase);
			}
			return;
		}

		const auto before = ReadPlayerRootFlags(anchor.object);
		const bool wasCulled = (before & kAppCulledBit) != 0;
		(void)SetRootAppCulled(
			anchor.object,
			false,
			"player-head",
			phase);

		const auto count = g_headVisibilityLogCount.fetch_add(
			1,
			std::memory_order_acq_rel) + 1;
		if (count <= 8) {
			REX::INFO(
				"Player head visibility {}: anchor={}, object={}, culled={}",
				phase,
				anchor.name,
				static_cast<void*>(anchor.object),
				wasCulled ? "yes" : "no");
		}
	}

	void MaybeEnsurePlayerHeadVisible(const char* phase)
	{
		if (!g_fixPlayerHeadVisibility ||
			!g_playerViewRequested.load(std::memory_order_acquire)) {
			return;
		}

		const auto now = GetTickCount64();
		const auto next = g_nextHeadVisibilityCheckAt.load(
			std::memory_order_acquire);
		if (now < next) {
			return;
		}

		g_nextHeadVisibilityCheckAt.store(
			now + g_headVisibilityCheckIntervalMs,
			std::memory_order_release);
		EnsurePlayerHeadVisible(phase);
	}

	void RestoreSpeakerObstruction(const char* phase)
	{
		auto* root = reinterpret_cast<RE::NiAVObject*>(
			g_culledSpeakerRoot.exchange(
				0,
				std::memory_order_acq_rel));

		if (!root) {
			return;
		}

		g_perfObstructionSpeakerRestore.fetch_add(1, std::memory_order_relaxed);

		(void)SetRootAppCulled(
			root,
			false,
			"speaker",
			phase);
	}

	void ForceDialogueActorsVisible(
		const char* phase,
		const bool includePlayer = true)
	{
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			~PerfScope() { PerfRecord(g_perfForceDialogueActorsVisible, startedAt); }
		} perfScope{ perfStartedAt };
		if (includePlayer) {
			(void)SetPlayerRootAppCulled(false, phase);
		}

		auto* rememberedReference =
			reinterpret_cast<RE::TESObjectREFR*>(
				g_lastDialogueSpeakerReference.load(
					std::memory_order_acquire));

		const bool rememberedReadable =
			rememberedReference &&
			IsReadableAddress(rememberedReference, sizeof(void*));

		// The engine swaps an actor's scene root during dialogue teardown, so a
		// root pointer we cached moments ago can already be stale. Always
		// re-resolve the *live* root from the stable actor references as well as
		// replaying the cached roots, so whichever root is currently onscreen
		// gets un-culled. This is why the old code left an invisible NPC: it
		// only touched the roots it remembered, never the freshly swapped one.
		RE::NiAVObject* roots[] = {
			reinterpret_cast<RE::NiAVObject*>(
				g_culledSpeakerRoot.load(std::memory_order_acquire)),
			reinterpret_cast<RE::NiAVObject*>(
				g_lastDialogueSpeakerRoot.load(std::memory_order_acquire)),
			rememberedReadable ?
				GetReferenceRoot3D(rememberedReference) :
				nullptr,
			GetReferenceRoot3D(GetSpeakerReference())
		};

		for (std::size_t index = 0; index < std::size(roots); ++index) {
			auto* root = roots[index];
			if (!root) {
				continue;
			}

			bool duplicate = false;
			for (std::size_t previous = 0; previous < index; ++previous) {
				if (roots[previous] == root) {
					duplicate = true;
					break;
				}
			}
			if (duplicate) {
				continue;
			}

			(void)SetRootAppCulled(root, false, "speaker", phase);

			// The app-culled bit is sometimes left on the head/body subtree
			// rather than the root itself. Un-cull a resolved head/neck node too
			// so the NPC's face can't stay hidden after exit. This uses a
			// cache-free lookup on purpose: ResolvePlayerHeadAnchor writes into
			// the player-head cache and must not be fed a speaker root.
			if (auto* head = FindHeadNodeUncached(root);
				head && head != root) {
				(void)SetRootAppCulled(
					head,
					false,
					"speaker-head",
					phase);
			}
		}
	}

	void RequestExitVisibilityPulse()
	{
		if (g_exitVisibilityCleanupUntil.load(
				std::memory_order_acquire) == 0) {
			return;
		}

		// A late DialogueCameraState update means teardown is still active.
		// Record activity only. Do not fight the engine by toggling actor
		// visibility after every update, which caused the one-second flicker.
		g_exitVisibilityLastLateUpdateAt.store(
			GetTickCount64(),
			std::memory_order_release);
	}

	void FinishExitVisibilityCleanup(const char* phase)
	{
		// One final repair after dialogue-camera updates have gone quiet.
		// SetRootAppCulled already returns without touching a root that is
		// visible, so this does not create another visible transition.
		ForceDialogueActorsVisible(phase);

		g_exitVisibilityCleanupUntil.store(0, std::memory_order_release);
		g_exitVisibilityLastLateUpdateAt.store(0, std::memory_order_release);
		g_exitVisibilityStartedAt.store(0, std::memory_order_release);
		g_culledSpeakerRoot.store(0, std::memory_order_release);
		g_lastDialogueSpeakerRoot.store(0, std::memory_order_release);
		g_lastDialogueSpeakerReference.store(0, std::memory_order_release);
	}

	void BeginExitVisibilityCleanup()
	{
		const auto now = GetTickCount64();
		g_exitVisibilityCleanupUntil.store(
			now + g_exitVisibilityCleanupMs,
			std::memory_order_release);
		g_exitVisibilityLastLateUpdateAt.store(
			now,
			std::memory_order_release);
		g_exitVisibilityStartedAt.store(
			now,
			std::memory_order_release);

		// Restore once at menu close, then leave Starfield's teardown alone.
		ForceDialogueActorsVisible("dialogue-close-immediate");
	}

	void UpdateExitVisibilityCleanup()
	{
		g_perfExitCleanupUpdates.fetch_add(1, std::memory_order_relaxed);
		const auto cleanupUntil =
			g_exitVisibilityCleanupUntil.load(
				std::memory_order_acquire);
		if (cleanupUntil == 0) {
			return;
		}

		const auto now = GetTickCount64();
		const auto lastLateUpdate =
			g_exitVisibilityLastLateUpdateAt.load(
				std::memory_order_acquire);
		const auto startedAt =
			g_exitVisibilityStartedAt.load(
				std::memory_order_acquire);

		// The engine can swap the NPC's scene root at any point across teardown.
		// Re-resolve and repair the *live* root every frame; SetRootAppCulled is
		// a no-op on an already-visible root, so this cannot cause flicker but
		// reliably catches whichever root is currently onscreen.
		ForceDialogueActorsVisible("post-dialogue-repair");

		// Do not finish early. The previous code stopped ~150ms after the last
		// late update, but the root swap that hides the NPC can land later than
		// that, and finishing cleared the speaker reference we need to
		// re-resolve it. Keep repairing for a guaranteed minimum window, then
		// require teardown to have gone quiet before finishing.
		const bool minimumElapsed =
			startedAt != 0 &&
			now >= startedAt + kExitVisibilityMinRepairMs;
		const bool teardownQuiet =
			lastLateUpdate == 0 ||
			now >= lastLateUpdate + g_exitVisibilitySettleDelayMs;

		if (minimumElapsed && teardownQuiet) {
			FinishExitVisibilityCleanup("post-dialogue-settled");
			return;
		}

		if (now >= cleanupUntil) {
			FinishExitVisibilityCleanup("post-dialogue-timeout");
		}
	}

	[[nodiscard]] bool PointIntersectsPlayerShot(
		const RE::NiPoint3& cameraPoint,
		const RE::NiPoint3& playerPoint,
		float sightLineLengthSquared,
		const RE::NiPoint3& testPoint,
		float testRadius)
	{
		const auto sightLine = playerPoint - cameraPoint;
		const auto cameraToTest = testPoint - cameraPoint;
		const float projection =
			cameraToTest.Dot(sightLine) / sightLineLengthSquared;

		// Ignore points behind the camera or effectively at the player.
		if (projection <= 0.02F || projection >= 0.98F) {
			return false;
		}

		const auto nearestPoint = cameraPoint + sightLine * projection;
		const float distanceSquared =
			testPoint.GetSquaredDistance(nearestPoint);
		const float radius = std::max(testRadius, 0.01F);

		return distanceSquared <= radius * radius;
	}

	[[nodiscard]] bool SpeakerIntersectsPlayerShot(
		RE::NiAVObject* speakerRoot)
	{
		if (!g_obstructionHandling || !speakerRoot) {
			return false;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		auto* playerRoot = GetPlayerRoot3D();

		if (!camera || !camera->cameraRoot ||
			!playerRoot ||
			playerRoot == speakerRoot) {
			return false;
		}

		const auto cameraPoint =
			camera->cameraRoot->world.translate;
		const auto playerPoint =
			playerRoot->worldBound.center;

		const auto sightLine =
			playerPoint - cameraPoint;
		const auto sightLineLengthSquared =
			sightLine.SqrLength();

		if (sightLineLengthSquared <= 0.0001F) {
			return false;
		}

		const float radiusScale =
			static_cast<float>(g_obstructionRadiusPercent) /
			100.0F;

		// Test 1: the whole-body bounding sphere (torso-centred). Catches the
		// common case where the NPC's body stands in the sightline.
		const auto speakerPoint = speakerRoot->worldBound.center;
		const float bodyRadius =
			speakerRoot->worldBound.radius * radiusScale;

		if (PointIntersectsPlayerShot(
				cameraPoint,
				playerPoint,
				sightLineLengthSquared,
				speakerPoint,
				bodyRadius)) {
			return true;
		}

		// Test 2: the head/neck node specifically. The body sphere is centred on
		// the torso, so when the NPC leans in and only their head enters the
		// frame near the camera, the torso centre projects outside the sightline
		// window and Test 1 misses it — that is the "NPC head covering the
		// player" case. Test the head node directly with a small fixed radius.
		if (auto* head = FindHeadNodeUncached(speakerRoot)) {
			const auto headPoint = head->world.translate;
			// A head is roughly ~12-15 game units across; use a generous radius
			// scaled by the same setting so it can be tuned from the INI.
			const float headRadius = 16.0F * radiusScale;

			if (PointIntersectsPlayerShot(
					cameraPoint,
					playerPoint,
					sightLineLengthSquared,
					headPoint,
					headRadius)) {
				return true;
			}
		}

		return false;
	}

	void UpdateSpeakerObstruction(const bool force = false)
	{
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			~PerfScope() { PerfRecord(g_perfObstructionUpdate, startedAt); }
		} perfScope{ perfStartedAt };
		const auto now = GetTickCount64();
		const auto next = g_nextObstructionCheckAt.load(
			std::memory_order_acquire);
		if (!force && now < next) {
			return;
		}
		g_nextObstructionCheckAt.store(
			now + g_obstructionCheckIntervalMs,
			std::memory_order_release);
		g_perfObstructionChecks.fetch_add(1, std::memory_order_relaxed);

		auto* speakerRoot =
			RememberDialogueSpeakerRoot();
		auto* culledRoot =
			reinterpret_cast<RE::NiAVObject*>(
				g_culledSpeakerRoot.load(
					std::memory_order_acquire));

		// A scene may change speakers while the player shot remains active.
		// Always restore the exact root that was hidden, not whichever actor
		// MenuTopicManager happens to report on the next frame.
		if (culledRoot && culledRoot != speakerRoot) {
			RestoreSpeakerObstruction("speaker-changed");
			culledRoot = nullptr;
		}

		const bool shouldCull =
			g_playerViewRequested.load(
				std::memory_order_acquire) &&
			SpeakerIntersectsPlayerShot(speakerRoot);

		if (shouldCull) {
			g_perfObstructionHits.fetch_add(1, std::memory_order_relaxed);
		}

		if (shouldCull && !culledRoot) {
			if (SetRootAppCulled(
					speakerRoot,
					true,
					"speaker",
					"obstruction-detected")) {
				g_culledSpeakerRoot.store(
					reinterpret_cast<std::uintptr_t>(speakerRoot),
					std::memory_order_release);
				g_perfObstructionSpeakerCull.fetch_add(1, std::memory_order_relaxed);
			}
		} else if (!shouldCull && culledRoot) {
			RestoreSpeakerObstruction("obstruction-cleared");
		}
	}

	void QueueView(bool showPlayer, ViewReason reason);

	[[nodiscard]] bool IsFirstPersonGameplayCamera(
		const RE::PlayerCamera* camera)
	{
		return camera &&
			(camera->QCameraEquals(RE::CameraState::kFirstPerson) ||
			 camera->QCameraEquals(RE::CameraState::kIronSights));
	}

	[[nodiscard]] bool IsThirdPersonGameplayCamera(
		const RE::PlayerCamera* camera)
	{
		return camera &&
			camera->QCameraEquals(RE::CameraState::kThirdPerson);
	}

	void UpdateGameplayViewHistory()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera ||
			g_firstPersonRestorePending ||
			camera->QCameraEquals(RE::CameraState::kDialogue)) {
			return;
		}

		if (camera->QCameraEquals(RE::CameraState::kFirstPerson)) {
			g_lastGameplayFirstPerson = true;
			g_gameplayViewKnown = true;
		} else if (IsThirdPersonGameplayCamera(camera)) {
			g_lastGameplayFirstPerson = false;
			g_gameplayViewKnown = true;
		} else if (camera->QCameraEquals(RE::CameraState::kIronSights) &&
			!g_gameplayViewKnown) {
			// Iron Sights is a separate camera state. If this is the first
			// stable sample, preserve it as first person rather than allowing
			// dialogue startup to forget the original perspective.
			g_lastGameplayFirstPerson = true;
			g_gameplayViewKnown = true;
		}
	}

	[[nodiscard]] bool BeginThirdPersonPlayerBodyPreparation(
		const ViewReason reason)
	{
		if (!g_fixFirstPersonBody ||
			!g_restoreFirstPersonAfterDialogue ||
			g_forcedThirdPersonForDialogue) {
			return false;
		}

		if (g_bodyPreparationPending) {
			return true;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera ||
			!camera->QCameraEquals(RE::CameraState::kDialogue)) {
			return false;
		}

		// ForceThirdPerson and immediately returning to DialogueCameraState in
		// the same task gave the engine no rendered frame in which to build the
		// external skeleton. That left the first-person body transforms intact.
		// Stage the transition and resume the player shot after several frames.
		ResetSpeakerHeadAnchorLock();
		g_perfForceThirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
		camera->ForceThirdPerson();

		const auto now = GetTickCount64();
		g_bodyPreparationPending = true;
		g_bodyPreparationResumeAt =
			now + g_firstPersonBodyPrepDelayMs;
		g_bodyPreparationDeadlineAt =
			now + std::max<std::uint32_t>(
				g_firstPersonBodyPrepDelayMs * 5,
				500u);
		g_deferredPlayerViewReason = reason;
		g_bodyPrepHasHeadSample = false;
		g_bodyPrepStableFrames = 0;
		g_bodyPrepTerminal = BodyPrepTerminal::kShowPlayer;

		REX::INFO(
			"First-person body fix: entered staged third-person preparation "
			"for {}ms",
			g_firstPersonBodyPrepDelayMs);
		return true;
	}

	// Dialogue-open path: sheathe, force third person to build a posed external
	// body during the opening beat, then settle back into the engine's natural
	// NPC-facing dialogue view. This is why the conversation can *start* face to
	// face even though we briefly showed third person to build the body.
	void BeginDialogueOpenBodyPreparation()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		if (g_forceSheatheOnDialogue) {
			(void)SheathePlayerWeapon("dialogue-open");
		}

		ResetSpeakerHeadAnchorLock();
		if (!camera->IsInThirdPerson()) {
			g_perfForceThirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
		camera->ForceThirdPerson();
		}

		const auto now = GetTickCount64();
		g_bodyPreparationPending = true;
		// Use a deterministic opening delay. Waiting for the head bone to
		// become perfectly stationary can fail forever during normal idle
		// animation, leaving the visible camera in third person until the
		// player selects a dialogue option.
		g_bodyPreparationResumeAt =
			now + g_dialogueOpenCameraDelayMs;
		g_bodyPreparationDeadlineAt =
			now + std::max<std::uint32_t>(
				g_dialogueOpenCameraDelayMs + 1500u,
				2000u);
		g_deferredPlayerViewReason = ViewReason::kSpeakerStarted;
		g_bodyPrepHasHeadSample = false;
		g_bodyPrepStableFrames = 0;
		g_bodyPrepTerminal = BodyPrepTerminal::kShowNpc;

		REX::INFO(
			"Dialogue-open: forced third person for {}ms before restoring "
			"the NPC dialogue shot",
			g_dialogueOpenCameraDelayMs);
	}

	void CancelPendingBodyPreparation(const char* phase)
	{
		if (!g_bodyPreparationPending) {
			return;
		}

		g_bodyPreparationPending = false;
		g_bodyPreparationResumeAt = 0;
		g_bodyPreparationDeadlineAt = 0;
		g_bodyPrepHasHeadSample = false;
		g_bodyPrepStableFrames = 0;

		REX::INFO(
			"First-person body preparation cancelled: {}",
			phase);
	}

	void UpdatePendingBodyPreparation()
	{
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			~PerfScope() { PerfRecord(g_perfBodyPreparation, startedAt); }
		} perfScope{ perfStartedAt };
		if (!g_bodyPreparationPending) {
			return;
		}

		if (!g_dialogueOpen || !IsDialogueMenuOpen()) {
			CancelPendingBodyPreparation("dialogue closed");
			return;
		}

		// During the opening third-person body warm-up, keep the player hidden
		// from the NPC-facing composition. This also catches a player-root swap
		// caused by ForceThirdPerson before DialogueCameraState is restored.
		if (g_bodyPrepTerminal == BodyPrepTerminal::kShowNpc) {
			(void)SetPlayerRootAppCulled(
				true,
				"dialogue-open-warmup");
		}

		const auto now = GetTickCount64();
		if (now < g_bodyPreparationResumeAt) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		if (g_bodyPrepTerminal == BodyPrepTerminal::kShowNpc) {
			// Opening dialogue uses a fixed third-person warm-up. Starfield often
			// restores DialogueCameraState by itself before this timer expires, so
			// avoid requesting the same state again unless it is actually needed.
			const bool dialogueStateAlreadyActive =
				camera->QCameraEquals(RE::CameraState::kDialogue);
			if (!dialogueStateAlreadyActive) {
				g_perfSetDialogueCameraStateCalls.fetch_add(1, std::memory_order_relaxed);
				g_perfSetDialogueCameraStateCalls.fetch_add(1, std::memory_order_relaxed);
		camera->SetCameraState(RE::CameraState::kDialogue);
			}
			if (!camera->QCameraEquals(RE::CameraState::kDialogue)) {
				if (now < g_bodyPreparationDeadlineAt) {
					g_bodyPreparationResumeAt = now + 16;
					return;
				}

				REX::CRITICAL(
					"Dialogue-open camera restore timed out; leaving Starfield's "
					"current camera untouched");
				CancelPendingBodyPreparation("opening camera restore timeout");
				return;
			}

			void* const dialogueState = GetActiveDialogueState();
			const auto speakerHandle = GetSpeakerHandle();
			if ((!dialogueState || speakerHandle == 0) &&
				now < g_bodyPreparationDeadlineAt) {
				// DialogueMenu can appear before the speaker handle is published.
				// Keep retrying instead of finalising with no NPC target and
				// waiting for the first selected line to repair the shot.
				g_bodyPreparationResumeAt = now + 16;
				return;
			}

			g_bodyPreparationPending = false;
			g_bodyPreparationResumeAt = 0;
			g_bodyPreparationDeadlineAt = 0;
			g_bodyPrepHasHeadSample = false;
			g_bodyPrepStableFrames = 0;
			g_forcedThirdPersonForDialogue = true;
			ResetPlayerHeadAnchorCache();
			g_playerViewRequested.store(false, std::memory_order_release);

			if (dialogueState && speakerHandle != 0) {
				const bool speakerTargetAlreadyActive =
					ReadHandle(dialogueState, kRequestedTargetHandle) == speakerHandle &&
					ReadHandle(dialogueState, kCurrentTargetHandle) == speakerHandle;

				WriteHandle(
					dialogueState,
					kRequestedTargetHandle,
					speakerHandle);
				WriteHandle(
					dialogueState,
					kCurrentTargetHandle,
					speakerHandle);

				g_stabilizedSpeakerHeadUpdateCount.store(
					0,
					std::memory_order_release);

				// Rebuild only when we genuinely had to restore DialogueCameraState
				// or repair its target. Rebuilding an already-correct opening shot
				// creates the single-frame wide flash seen at the warm-up deadline.
				const bool rebuildRequired =
					!dialogueStateAlreadyActive || !speakerTargetAlreadyActive;
				if (rebuildRequired) {
					const auto rebuild = GetRebuildFunction();
					if (rebuild) {
						const auto perfRebuildStartedAt = PerfNowTicks();
						rebuild(dialogueState, true);
						const auto perfRebuildUs = PerfElapsedUs(perfRebuildStartedAt);
						g_perfCameraRebuildCalls.fetch_add(1, std::memory_order_relaxed);
						g_perfCameraRebuildTotalUs.fetch_add(perfRebuildUs, std::memory_order_relaxed);
						PerfUpdateMax(g_perfCameraRebuildMaxUs, perfRebuildUs);
					}
				} else {
					REX::INFO(
						"Opening NPC shot already valid; redundant rebuild skipped");
				}

				ResetSpeakerHeadAnchorLock();
				g_playerShotEstablished.store(
					true,
					std::memory_order_release);
				REX::INFO(
					"Opening NPC shot established");
			}

			REX::INFO(
				"Dialogue-open camera restored after {}ms; speaker=0x{:08X}",
				g_dialogueOpenCameraDelayMs,
				speakerHandle);
			return;
		}

		if (!camera->IsInThirdPerson() &&
			now < g_bodyPreparationDeadlineAt) {
			g_perfForceThirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
		camera->ForceThirdPerson();
			g_bodyPreparationResumeAt = now + 16;
			return;
		}

		// A resolvable head *node* does not mean the skeleton has been posed
		// yet: for a few frames the anim graph is still snapping the bones from
		// bind pose into the real pose, and showing the shot then is what
		// produces the twisted/backwards body (worse with a weapon drawn, which
		// defers even more skeleton work). Require a settle point to hold still
		// across several consecutive frames before treating the body as ready.
		// Prefer the head bone; fall back to the root's world-bound centre when
		// the head fix is off or no head node exists yet. The deadline is the
		// hard safety cap so this can never hang.
		const bool deadlineReached = now >= g_bodyPreparationDeadlineAt;

		auto* root = GetPlayerRoot3D();

		RE::NiAVObject* headObject = nullptr;
		if (g_fixPlayerHeadVisibility && root) {
			const auto anchor = ResolvePlayerHeadAnchor(root);
			headObject = anchor.object;
			if (headObject) {
				(void)SetRootAppCulled(
					headObject,
					false,
					"player-head",
					"third-person-body-ready");
			} else if (!deadlineReached) {
				// Head node not present yet; keep waiting for the skeleton.
				g_bodyPrepHasHeadSample = false;
				g_bodyPrepStableFrames = 0;
				g_bodyPreparationResumeAt = now + 16;
				return;
			}
		}

		if (!deadlineReached && root) {
			const RE::NiPoint3 settlePoint =
				headObject ?
					headObject->world.translate :
					root->worldBound.center;

			if (g_bodyPrepHasHeadSample) {
				const float movedSq =
					settlePoint.GetSquaredDistance(
						g_bodyPrepLastHeadPoint);
				g_bodyPrepStableFrames =
					movedSq <= kBodyPrepStableEpsilonSq ?
						g_bodyPrepStableFrames + 1 :
						0;
			}

			g_bodyPrepLastHeadPoint = settlePoint;
			g_bodyPrepHasHeadSample = true;

			if (g_bodyPrepStableFrames <
				kBodyPrepRequiredStableFrames) {
				// Poll every frame while the pose settles.
				g_bodyPreparationResumeAt = now;
				return;
			}
		}

		g_perfSetDialogueCameraStateCalls.fetch_add(1, std::memory_order_relaxed);
		camera->SetCameraState(RE::CameraState::kDialogue);
		if (!camera->QCameraEquals(RE::CameraState::kDialogue)) {
			if (now < g_bodyPreparationDeadlineAt) {
				g_bodyPreparationResumeAt = now + 16;
				return;
			}

			REX::CRITICAL(
				"First-person body fix timed out restoring DialogueCameraState");
			CancelPendingBodyPreparation("restore timeout");
			return;
		}

		const auto reason = g_deferredPlayerViewReason;
		const auto terminal = g_bodyPrepTerminal;
		g_bodyPreparationPending = false;
		g_bodyPreparationResumeAt = 0;
		g_bodyPreparationDeadlineAt = 0;
		g_bodyPrepHasHeadSample = false;
		g_bodyPrepStableFrames = 0;
		g_forcedThirdPersonForDialogue = true;
		ResetPlayerHeadAnchorCache();

		ReapplyPlayerExpressionOverride("third-person body initialised");
		REX::INFO(
			"Body prep complete; DialogueCameraState restored, terminal={}",
			terminal == BodyPrepTerminal::kShowPlayer ? "player" : "NPC");

		if (terminal == BodyPrepTerminal::kShowNpc) {
			// Settle into the engine's natural NPC-facing dialogue view. Point
			// the restored dialogue state explicitly at the speaker so the
			// conversation starts face to face, then let automatic/manual
			// switching take over from this clean baseline.
			g_playerViewRequested.store(
				false,
				std::memory_order_release);

			void* const dialogueState = GetActiveDialogueState();
			const auto speakerHandle = GetSpeakerHandle();
			if (dialogueState && speakerHandle != 0) {
				WriteHandle(
					dialogueState,
					kRequestedTargetHandle,
					speakerHandle);
				WriteHandle(
					dialogueState,
					kCurrentTargetHandle,
					speakerHandle);

				const auto rebuild = GetRebuildFunction();
				if (rebuild) {
					const auto perfRebuildStartedAt = PerfNowTicks();
					rebuild(dialogueState, true);
					const auto perfRebuildUs = PerfElapsedUs(perfRebuildStartedAt);
					g_perfCameraRebuildCalls.fetch_add(1, std::memory_order_relaxed);
					g_perfCameraRebuildTotalUs.fetch_add(perfRebuildUs, std::memory_order_relaxed);
					PerfUpdateMax(g_perfCameraRebuildMaxUs, perfRebuildUs);
				}
			}

			REX::INFO(
				"Dialogue-open: settled to NPC face-to-face view "
				"(speaker=0x{:08X})",
				speakerHandle);
			return;
		}

		QueueView(true, reason);
	}

	void UpdatePendingFirstPersonRestore()
	{
		if (!g_firstPersonRestorePending) {
			return;
		}

		const auto now = GetTickCount64();
		if (now < g_firstPersonRestoreAt) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		// This recovery must be allowed to run while the camera still reports
		// DialogueCameraState. Refusing to act in that state created a deadlock:
		// the first-person restore waited for Dialogue to end while the separate
		// unstuck path waited for the restore to finish.
		const bool restoreFirstPerson =
			g_restoreFirstPersonAfterDialogue;

		if (restoreFirstPerson) {
			if (!IsFirstPersonGameplayCamera(camera)) {
				camera->ForceFirstPerson();
				g_firstPersonRestoreAt = now + 32;

				if (now < g_gameplayCameraRestoreDeadlineAt) {
					return;
				}

				// ForceFirstPerson normally performs the full transition. Use the
				// direct state assignment only as a bounded last-resort recovery.
				camera->SetCameraState(RE::CameraState::kFirstPerson);
				if (!IsFirstPersonGameplayCamera(camera)) {
					REX::CRITICAL(
						"Gameplay camera recovery timed out entering first person");
				}
			}
		} else {
			if (!IsThirdPersonGameplayCamera(camera)) {
				g_perfForceThirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
		camera->ForceThirdPerson();
				g_firstPersonRestoreAt = now + 32;

				if (now < g_gameplayCameraRestoreDeadlineAt) {
					return;
				}

				camera->SetCameraState(RE::CameraState::kThirdPerson);
				if (!IsThirdPersonGameplayCamera(camera)) {
					REX::CRITICAL(
						"Gameplay camera recovery timed out entering third person");
				}
			}
		}

		g_firstPersonRestorePending = false;
		g_gameplayCameraRestoreDeadlineAt = 0;
		g_restoreFirstPersonAfterDialogue = false;
		g_forcedThirdPersonForDialogue = false;
		g_lastGameplayFirstPerson = restoreFirstPerson;
		g_gameplayViewKnown = true;

		REX::INFO(
			"Gameplay camera restored after dialogue: {}",
			restoreFirstPerson ? "first person" : "third person");
	}



	[[nodiscard]] bool IsAccessibleControllerField(
		void* controller,
		const std::ptrdiff_t offset,
		const bool requireWritable)
	{
		if (!controller) {
			return false;
		}

		auto* const address =
			static_cast<std::byte*>(controller) + offset;

		MEMORY_BASIC_INFORMATION info{};
		if (VirtualQuery(address, &info, sizeof(info)) == 0) {
			return false;
		}

		if (info.State != MEM_COMMIT ||
			(info.Protect & PAGE_GUARD) != 0 ||
			info.Protect == PAGE_NOACCESS) {
			return false;
		}

		const DWORD protection = info.Protect & 0xFF;

		const bool readable =
			protection == PAGE_READONLY ||
			protection == PAGE_READWRITE ||
			protection == PAGE_WRITECOPY ||
			protection == PAGE_EXECUTE_READ ||
			protection == PAGE_EXECUTE_READWRITE ||
			protection == PAGE_EXECUTE_WRITECOPY;

		const bool writable =
			protection == PAGE_READWRITE ||
			protection == PAGE_WRITECOPY ||
			protection == PAGE_EXECUTE_READWRITE ||
			protection == PAGE_EXECUTE_WRITECOPY;

		if (!readable || (requireWritable && !writable)) {
			return false;
		}

		const auto start =
			reinterpret_cast<std::uintptr_t>(address);
		const auto regionStart =
			reinterpret_cast<std::uintptr_t>(info.BaseAddress);
		const auto regionEnd =
			regionStart + info.RegionSize;

		return start >= regionStart &&
			start + sizeof(std::uint32_t) <= regionEnd;
	}

	[[nodiscard]] std::uint32_t ReadControllerField(
		void* controller,
		const std::ptrdiff_t offset)
	{
		if (!IsAccessibleControllerField(
				controller,
				offset,
				false)) {
			return 0xFFFFFFFF;
		}

		const auto* field =
			reinterpret_cast<const std::uint32_t*>(
				static_cast<const std::byte*>(controller) + offset);

		return std::atomic_ref<const std::uint32_t>(*field).load(
			std::memory_order_acquire);
	}

	[[nodiscard]] bool WriteControllerField(
		void* controller,
		const std::ptrdiff_t offset,
		const std::uint32_t value)
	{
		if (!IsAccessibleControllerField(
				controller,
				offset,
				true)) {
			return false;
		}

		auto* field =
			reinterpret_cast<std::uint32_t*>(
				static_cast<std::byte*>(controller) + offset);

		std::atomic_ref<std::uint32_t>(*field).store(
			value,
			std::memory_order_release);

		return true;
	}

	[[nodiscard]] void* GetCapturedController()
	{
		const auto primary =
			g_primaryDialogueController.load(
				std::memory_order_acquire);

		if (primary != 0) {
			return reinterpret_cast<void*>(primary);
		}

		return reinterpret_cast<void*>(
			g_capturedController.load(
				std::memory_order_acquire));
	}

	[[nodiscard]] bool ValidateCapturedController(
		void* controller,
		const std::uint32_t speakerHandle,
		const std::uint32_t playerHandle)
	{
		if (!controller ||
			!IsAccessibleControllerField(
				controller,
				kControllerTargetHandle,
				true) ||
			!IsAccessibleControllerField(
				controller,
				kControllerState,
				true)) {
			return false;
		}

		const auto state =
			ReadControllerField(
				controller,
				kControllerState);

		if (state > 5) {
			REX::WARN(
				"Captured controller rejected: state +0x9C={} "
				"is outside the mapped range 0..5",
				state);
			return false;
		}

		const auto liveTarget =
			ReadControllerField(
				controller,
				kControllerTargetHandle);

		const auto capturedTarget =
			g_capturedControllerTarget.load(
				std::memory_order_acquire);

		const bool plausibleTarget =
			liveTarget == 0 ||
			liveTarget == speakerHandle ||
			liveTarget == playerHandle ||
			capturedTarget == speakerHandle ||
			capturedTarget == playerHandle;

		if (!plausibleTarget) {
			REX::WARN(
				"Captured controller rejected: +0x60=0x{:08X}, "
				"last call target=0x{:08X}, speaker=0x{:08X}, "
				"player=0x{:08X}",
				liveTarget,
				capturedTarget,
				speakerHandle,
				playerHandle);
			return false;
		}

		return true;
	}

	[[nodiscard]] bool ApplyCapturedControllerTarget(
		const std::uint32_t targetHandle,
		const bool showPlayer)
	{
		void* const controller = GetCapturedController();
		const auto speakerHandle = GetSpeakerHandle();
		const auto playerHandle = GetPlayerHandle();

		if (!ValidateCapturedController(
				controller,
				speakerHandle,
				playerHandle)) {
			REX::WARN(
				"Mapped controller update skipped: no valid "
				"controller has been captured yet");
			return false;
		}

		const auto beforeTarget =
			ReadControllerField(
				controller,
				kControllerTargetHandle);

		const auto beforeState =
			ReadControllerField(
				controller,
				kControllerState);

		const bool targetWritten =
			WriteControllerField(
				controller,
				kControllerTargetHandle,
				targetHandle);

		// Only the player shot needs the mapped controller forced into a
		// fresh state. NPC shots are released back to Starfield's native
		// resolver instead of resetting +0x9C and making the controller
		// rebuild its internal composition over several frames.
		const bool stateReset =
			showPlayer ?
				WriteControllerField(
					controller,
					kControllerState,
					0) :
				true;

		const auto afterTarget =
			ReadControllerField(
				controller,
				kControllerTargetHandle);

		const auto afterState =
			ReadControllerField(
				controller,
				kControllerState);

		REX::INFO(
			"Exact controller {}: pointer={}, "
			"+0x60 0x{:08X}->0x{:08X}, "
			"+0x9C {}->{}, writes=[target:{},state:{}]",
			showPlayer ? "player" : "NPC",
			controller,
			beforeTarget,
			afterTarget,
			beforeState,
			afterState,
			targetWritten ? "ok" : "failed",
			stateReset ? "ok" : "failed");

		return targetWritten && stateReset;
	}


	[[nodiscard]] RebuildDialogueCamera_t GetRebuildFunction()
	{
		const auto moduleBase =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr));

		if (moduleBase == 0) {
			return nullptr;
		}

		return reinterpret_cast<RebuildDialogueCamera_t>(
			moduleBase + kRebuildDialogueCameraRva);
	}

	void ForcePlayerHandles(void* dialogueState)
	{
		const auto playerHandle = GetPlayerHandle();
		if (!dialogueState || playerHandle == 0) {
			return;
		}

		WriteHandle(
			dialogueState,
			kRequestedTargetHandle,
			playerHandle);

		WriteHandle(
			dialogueState,
			kCurrentTargetHandle,
			playerHandle);
	}

	void ResetSpeakerHeadAnchorLock()
	{
		g_lockedSpeakerHeadRoot = nullptr;
		g_lockedSpeakerHeadObject = nullptr;
		g_lockedSpeakerHeadWorld = {};
		g_lockedSpeakerHeadPreviousWorld = {};
		g_lockedSpeakerHeadBoundCenter = {};
		g_lockedSpeakerHeadBoundRadius = 0.0F;
		g_speakerHeadAnchorLocked = false;
		g_stabilizedSpeakerHeadUpdateCount.store(
			0,
			std::memory_order_release);
	}

	class ScopedSpeakerHeadAnchorOverride
	{
	public:
		explicit ScopedSpeakerHeadAnchorOverride(
			const bool playerMode,
			const char* phase)
		{
			if (!g_suppressCameraRecentering ||
				playerMode ||
				!g_dialogueActiveForHooks.load(
					std::memory_order_acquire) ||
				!g_playerShotEstablished.load(
					std::memory_order_acquire)) {
				return;
			}

			root = GetReferenceRoot3D(GetSpeakerReference());
			if (!root) {
				return;
			}

			head = FindHeadNodeUncached(root);
			if (!head) {
				return;
			}

			originalWorld = head->world;
			originalPreviousWorld = head->previousWorld;
			originalBoundCenter = head->worldBound.center;
			originalBoundRadius = head->worldBound.radius;

			if (!g_speakerHeadAnchorLocked ||
				g_lockedSpeakerHeadRoot != root ||
				g_lockedSpeakerHeadObject != head) {
				g_lockedSpeakerHeadRoot = root;
				g_lockedSpeakerHeadObject = head;
				g_lockedSpeakerHeadWorld = originalWorld;
				g_lockedSpeakerHeadPreviousWorld = originalPreviousWorld;
				g_lockedSpeakerHeadBoundCenter = originalBoundCenter;
				g_lockedSpeakerHeadBoundRadius = originalBoundRadius;
				g_speakerHeadAnchorLocked = true;

				REX::INFO(
					"NPC dialogue head anchor captured {}: "
					"root={}, head={}, translate=({:.2f},{:.2f},{:.2f})",
					phase ? phase : "unspecified",
					static_cast<void*>(root),
					static_cast<void*>(head),
					g_lockedSpeakerHeadWorld.translate.x,
					g_lockedSpeakerHeadWorld.translate.y,
					g_lockedSpeakerHeadWorld.translate.z);
			}

			head->world = g_lockedSpeakerHeadWorld;
			head->previousWorld = g_lockedSpeakerHeadPreviousWorld;
			head->worldBound.center = g_lockedSpeakerHeadBoundCenter;
			head->worldBound.radius = g_lockedSpeakerHeadBoundRadius;
			active = true;

			const auto count =
				g_stabilizedSpeakerHeadUpdateCount.fetch_add(
					1,
					std::memory_order_acq_rel) + 1;
			if (false && count <= 4) {
				REX::INFO(
					"NPC dialogue head anchor stabilized {} {}: "
					"animated translate delta=({:.3f},{:.3f},{:.3f})",
					count,
					phase ? phase : "unspecified",
					originalWorld.translate.x -
						g_lockedSpeakerHeadWorld.translate.x,
					originalWorld.translate.y -
						g_lockedSpeakerHeadWorld.translate.y,
					originalWorld.translate.z -
						g_lockedSpeakerHeadWorld.translate.z);
			}
		}

		~ScopedSpeakerHeadAnchorOverride()
		{
			if (active && head) {
				head->world = originalWorld;
				head->previousWorld = originalPreviousWorld;
				head->worldBound.center = originalBoundCenter;
				head->worldBound.radius = originalBoundRadius;
			}
		}

		ScopedSpeakerHeadAnchorOverride(
			const ScopedSpeakerHeadAnchorOverride&) = delete;
		ScopedSpeakerHeadAnchorOverride& operator=(
			const ScopedSpeakerHeadAnchorOverride&) = delete;

	private:
		RE::NiAVObject* root{ nullptr };
		RE::NiAVObject* head{ nullptr };
		RE::NiTransform originalWorld{};
		RE::NiTransform originalPreviousWorld{};
		RE::NiPoint3 originalBoundCenter{};
		float originalBoundRadius{ 0.0F };
		bool active{ false };
	};

	class DialogueUpdateVisibilityHook
	{
	public:
		static void Thunk(void* dialogueState)
		{
			const auto perfStartedAt = PerfNowTicks();
			struct PerfScope
			{
				std::uint64_t startedAt;
				~PerfScope() { PerfRecord(g_perfDialogueUpdateHook, startedAt); }
			} perfScope{{ perfStartedAt }};

			if (g_shipDialogueBypass.load(std::memory_order_acquire)) {
				Hook(dialogueState);
				return;
			}

			const bool dialogueActive =
				g_dialogueActiveForHooks.load(
					std::memory_order_acquire);
			// The live menu state is authoritative for visibility. The lifecycle
			// atomic intentionally lags by up to one Tick at both edges; treating
			// that stale value as an open menu allowed a teardown update to keep
			// actors culled for the first gameplay frame.
			const bool dialogueMenuOpen = IsDialogueMenuOpen();
			const bool playerMode =
				dialogueMenuOpen &&
				g_playerViewRequested.load(
					std::memory_order_acquire) &&
				dialogueState;

			// Hide the player before Starfield calculates an NPC-facing dialogue
			// frame. This runs earlier than Tick's menu-open handling and prevents
			// the previous third-person player model leaking into the first shot.
			if (dialogueMenuOpen && !playerMode) {
				(void)SetPlayerRootAppCulled(
					true,
					"NPC-update-entry");
			}

			// Legacy builds culled and unculled the player around every player
			// camera update. Stable mode leaves the player visible instead.
			if (playerMode && !g_stablePlayerVisibility) {
				(void)SetPlayerRootAppCulled(
					true,
					"update-entry-legacy");
			}

			// Render-safe path: do not rewrite actor/head transforms or bounds from
			// the per-frame dialogue-camera hook. Those scene-graph writes can race
			// the renderer and invalidate temporal AA / motion-vector history.
			Hook(dialogueState);

			if (!dialogueMenuOpen) {
				// Starfield can perform one final DialogueCameraState update after
				// DialogueMenu closes. That update lands after Tick's immediate
				// visibility repair and can leave the outgoing speaker/player hidden
				// for exactly one rendered gameplay frame. Repair at the end of the
				// first such update, then let the existing task-thread cleanup handle
				// any later scene-root swaps. This is deliberately one-shot: older
				// builds fought every teardown update and produced visible flicker.
				bool expected = false;
				if (g_exitPreRenderVisibilityRepaired.compare_exchange_strong(
						expected,
						true,
						std::memory_order_acq_rel)) {
					RestoreSpeakerObstruction(
						"dialogue-close-pre-render");
					ForceDialogueActorsVisible(
						"dialogue-close-pre-render");
					g_perfExitPreRenderRepairs.fetch_add(1, std::memory_order_relaxed);
					REX::INFO(
						"Dialogue exit pre-render visibility repaired");
				}

				RequestExitVisibilityPulse();
				return;
			}

			if (!dialogueActive) {
				// This is the opening edge: DialogueMenu is already live but Tick
				// has not armed the remaining dialogue lifecycle yet. The early
				// player hide above is all that is required for this frame.
				return;
			}

			// Reproduce the old engine's presentation behaviour only
			// after camera calculation, so the model is available to
			// the renderer without changing the update branch.
			if (playerMode) {
				(void)SetPlayerRootAppCulled(
					false,
					"update-exit");
				MaybeEnsurePlayerHeadVisible("update-exit");
				UpdateSpeakerObstruction();
			} else {
				RestoreSpeakerObstruction("NPC-update");
			}
		}

		static inline REL::THookVFT<void(void*)> Hook{
			"PDC_DialogueUpdateVisibility",
			REL::Offset(kDialogueStateVtableRva),
			kDialogueUpdateVtableIndex,
			Thunk
		};
	};

	class ControllerUpdateCaptureHook
	{
	public:
		static void Thunk(
			void* controller,
			std::uint32_t targetHandle)
		{
			const auto perfStartedAt = PerfNowTicks();
			struct PerfScope
			{
				std::uint64_t startedAt;
				~PerfScope() { PerfRecord(g_perfControllerHook, startedAt); }
			} perfScope{{ perfStartedAt }};

			if (g_shipDialogueBypass.load(std::memory_order_acquire)) {
				Hook(controller, targetHandle);
				return;
			}

			if (ShouldSuppressDuplicateDialogueController(controller)) {
				// Do not overwrite g_capturedController with a duplicate and do
				// not forward its expensive native controller update.
				return;
			}

			g_capturedController.store(
				reinterpret_cast<std::uintptr_t>(controller),
				std::memory_order_release);

			g_capturedControllerTarget.store(
				targetHandle,
				std::memory_order_release);

			const bool dialogueActive =
				g_dialogueActiveForHooks.load(
					std::memory_order_acquire);
			const bool playerMode =
				dialogueActive &&
				g_playerViewRequested.load(
					std::memory_order_acquire);

			// Always run Starfield's controller update. Earlier builds skipped this
			// call in an attempt to stop visible recentering, but the routine also
			// performs native dialogue, lighting, and input bookkeeping. During NPC
			// shots, only the target actor's head anchor is held stable.
			const auto playerHandle = GetPlayerHandle();

			std::uint32_t effectiveTarget = targetHandle;
			if (playerMode && playerHandle != 0) {
				effectiveTarget = playerHandle;
				g_perfControllerPlayerOverrides.fetch_add(1, std::memory_order_relaxed);
			}

			const auto beforeTarget =
				ReadControllerField(
					controller,
					kControllerTargetHandle);

			const auto beforeState =
				ReadControllerField(
					controller,
					kControllerState);

			// Never mutate scene-graph transforms or bounds from this hook. The
			// controller may run on several engine worker threads.
			Hook(controller, effectiveTarget);

			if (playerMode) {
				(void)SetPlayerRootAppCulled(
					false,
					"controller-exit");
			}

			const auto count =
				g_controllerHookLogCount.fetch_add(
					1,
					std::memory_order_acq_rel) + 1;

			if (count <= 12) {
				REX::INFO(
					"Controller hook {}: pointer={}, "
					"argument 0x{:08X}->0x{:08X}; "
					"+0x60 0x{:08X}->0x{:08X}, "
					"+0x9C {}->{}",
					count,
					controller,
					targetHandle,
					effectiveTarget,
					beforeTarget,
					ReadControllerField(
						controller,
						kControllerTargetHandle),
					beforeState,
					ReadControllerField(
						controller,
						kControllerState));
			}
		}

		static inline REL::THook<
			void(void*, std::uint32_t)> Hook{
			"PDC_ControllerUpdateCapture",
			REL::Offset(kControllerUpdateCallsiteRva),
			0,
			Thunk
		};
	};

	class ResolveDialogueTargetHook
	{
	public:
		static void Thunk(void* dialogueState, void* dialogueContext)
		{
			const auto perfStartedAt = PerfNowTicks();
			struct PerfScope
			{
				std::uint64_t startedAt;
				~PerfScope() { PerfRecord(g_perfResolverHook, startedAt); }
			} perfScope{{ perfStartedAt }};

			if (g_shipDialogueBypass.load(std::memory_order_acquire)) {
				Hook(dialogueState, dialogueContext);
				return;
			}

			const bool playerModeLocked =
				g_suppressCameraRecentering &&
				g_dialogueActiveForHooks.load(
					std::memory_order_acquire) &&
				IsDialogueMenuOpen() &&
				g_playerViewRequested.load(
					std::memory_order_acquire) &&
				g_playerShotEstablished.load(
					std::memory_order_acquire) &&
				dialogueState;

			if (playerModeLocked) {
				// Starfield's resolver repeatedly replaces the established player
				// target with the NPC target. The log showed this exact ping-pong on
				// every resolver call: [player,NPC] -> [player,player]. Besides the
				// visible one-second snap, that repeated target transition can mark
				// the view as a camera cut and continuously discard temporal-AA
				// history. Once the intentional player rebuild has completed, keep
				// the already-established handles and skip only this target resolver.
				//
				// The menu-open check is deliberate: as soon as DialogueMenu closes,
				// this path becomes impossible and Starfield's original resolver runs
				// normally for teardown/input cleanup.
				ForcePlayerHandles(dialogueState);
				g_playerCameraLocked.store(
					true,
					std::memory_order_release);

				g_perfResolverSuppressed.fetch_add(1, std::memory_order_relaxed);
				const auto count =
					g_suppressedTargetResolveCount.fetch_add(
						1,
						std::memory_order_acq_rel) + 1;
				if (count <= 12) {
					REX::INFO(
						"Suppressed player target resolver {}: "
						"fields=[0x{:08X},0x{:08X}]",
						count,
						ReadHandle(
							dialogueState,
							kRequestedTargetHandle),
						ReadHandle(
							dialogueState,
							kCurrentTargetHandle));
				}
				return;
			}

			g_playerCameraLocked.store(
				false,
				std::memory_order_release);
			Hook(dialogueState, dialogueContext);
		}

		static inline REL::THook<void(void*, void*)> Hook{
			"PDC_ResolveDialogueTarget",
			REL::Offset(kResolveDialogueTargetCallsiteRva),
			0,
			Thunk
		};
	};

	void PerformSetViewOnTaskQueue(
		const bool showPlayer,
		const ViewReason reason)
	{
		struct CompletionGuard
		{
			~CompletionGuard()
			{
				g_actionQueued.store(
					false,
					std::memory_order_release);
			}
		} completionGuard;

		// Hard guard: if the DialogueMenu has already closed by the time this
		// queued task runs, do nothing. Rebuilding the dialogue camera after the
		// menu is gone shoves the player back into a menu-less dialogue camera
		// with no way out — the soft-lock. Let the engine's normal teardown run.
		if (!g_dialogueOpen || !IsDialogueMenuOpen()) {
			REX::INFO(
				"View toggle aborted: dialogue no longer open");
			return;
		}

		if (showPlayer &&
			BeginThirdPersonPlayerBodyPreparation(reason)) {
			// The player shot will be queued again after the engine has spent
			// real frames in third person and DialogueCameraState is restored.
			return;
		}

		void* const dialogueState = GetActiveDialogueState();
		if (!dialogueState) {
			g_perfViewTransitionNoStateAborts.fetch_add(
				1,
				std::memory_order_relaxed);

			g_viewTransitionRetryAfter.store(
				GetTickCount64() + kViewTransitionRetryBackoffMs,
				std::memory_order_release);

			if (!g_viewTransitionRetryWarningArmed.exchange(
					true,
					std::memory_order_acq_rel)) {
				REX::WARN(
					"Dialogue camera state temporarily unavailable; retrying");
			}

			return;
		}

		// A live DialogueCameraState means the retry episode is over.
		g_viewTransitionRetryAfter.store(
			0,
			std::memory_order_release);
		g_viewTransitionRetryWarningArmed.store(
			false,
			std::memory_order_release);

		const bool currentPlayerView =
			g_playerViewRequested.load(
				std::memory_order_acquire);

		if (currentPlayerView == showPlayer) {
			return;
		}

		g_perfViewTransitions.fetch_add(1, std::memory_order_relaxed);

		const auto targetHandle =
			showPlayer ?
				GetPlayerHandle() :
				GetSpeakerHandle();

		if (targetHandle == 0) {
			REX::WARN(
				"Toggle aborted: selected target handle is zero");
			return;
		}

		g_earlyOverrideCount.store(
			0,
			std::memory_order_release);
		g_controllerHookLogCount.store(
			0,
			std::memory_order_release);
		g_visibilityHookLogCount.store(
			0,
			std::memory_order_release);
		g_visibilityCallFailed.store(
			false,
			std::memory_order_release);
		g_playerCameraLocked.store(
			false,
			std::memory_order_release);
		g_playerShotEstablished.store(
			false,
			std::memory_order_release);
		ResetSpeakerHeadAnchorLock();
		g_stabilizedSpeakerHeadUpdateCount.store(
			0,
			std::memory_order_release);
		g_suppressedTargetResolveCount.store(
			0,
			std::memory_order_release);
		g_headVisibilityLogCount.store(
			0,
			std::memory_order_release);
		g_headFramingLogCount.store(
			0,
			std::memory_order_release);
		g_nextObstructionCheckAt.store(0, std::memory_order_release);
		g_nextHeadVisibilityCheckAt.store(0, std::memory_order_release);

		// The hooks must be active before the rebuilt camera performs
		// its first update.
		g_playerViewRequested.store(
			showPlayer,
			std::memory_order_release);

		// Stage actor visibility before rebuilding the incoming shot. The
		// previous order waited until after the rebuild, allowing one rendered
		// frame where the outgoing actor could stand directly in front of the
		// new camera. SetRootAppCulled is idempotent, so the post-rebuild calls
		// below remain as cheap confirmation rather than a visible transition.
		if (showPlayer) {
			// Do not pre-emptively app-cull the outgoing speaker. Even a hide/show
			// pair completed inside this task invalidates the actor's temporal
			// rendering history. On the next NPC shot Starfield then reconstructs
			// the actor over several dithered frames, which is the visible flicker.
			//
			// Once the incoming player composition exists, UpdateSpeakerObstruction
			// performs the real camera-to-player intersection test and hides the
			// speaker only when they genuinely block the shot.
			(void)SetPlayerRootAppCulled(false, "pre-player-rebuild");
		} else {
			// Do not app-cull the player before rebuilding the NPC shot. The
			// native visibility call can force a render-scene update in the same
			// frame as the camera rebuild, which is where the player-to-NPC FPS
			// hitch was observed. The player is culled once, after the rebuilt
			// composition exists and before the next frame is presented.
			RestoreSpeakerObstruction("pre-NPC-rebuild");

			// Reacquire the current speaker root because Starfield can replace
			// scene roots during dialogue-camera transitions.
			if (auto* speakerRoot = RememberDialogueSpeakerRoot()) {
				(void)SetRootAppCulled(
					speakerRoot,
					false,
					"speaker",
					"pre-NPC-rebuild");
			}
		}

		WriteHandle(
			dialogueState,
			kRequestedTargetHandle,
			targetHandle);

		WriteHandle(
			dialogueState,
			kCurrentTargetHandle,
			targetHandle);

		// The mapped controller override is required only to establish the
		// non-native player target. When returning to the NPC, release the
		// resolver lock and let Starfield update its own controller naturally.
		// Forcing +0x60 and resetting +0x9C on the NPC path caused an avoidable
		// internal reinitialisation and correlated with the transition FPS drop.
		const bool controllerUpdated =
			showPlayer &&
			ApplyCapturedControllerTarget(
				targetHandle,
				true);

		REX::INFO(
			"Prepared {} target 0x{:08X}; "
			"fields=[0x{:08X},0x{:08X}], controller={}",
			showPlayer ? "player" : "NPC",
			targetHandle,
			ReadHandle(
				dialogueState,
				kRequestedTargetHandle),
			ReadHandle(
				dialogueState,
				kCurrentTargetHandle),
			showPlayer ?
				(controllerUpdated ? "updated" : "not updated") :
				"native");

		const auto rebuild = GetRebuildFunction();
		if (!rebuild) {
			g_playerCameraLocked.store(
				false,
				std::memory_order_release);
			g_playerViewRequested.store(
				false,
				std::memory_order_release);

			REX::CRITICAL(
				"Could not resolve dialogue-camera rebuild function");
			return;
		}

		if (showPlayer) {
			EnsurePlayerHeadVisible("pre-rebuild");
		}

		const auto transitionStartedAt = GetTickCount64();
		const auto rebuildPerfStartedAt = PerfNowTicks();
		{
			ScopedPlayerFramingOverride framingOverride{ "camera-rebuild" };
			rebuild(dialogueState, true);
		}
		const auto rebuildDurationUs = PerfElapsedUs(rebuildPerfStartedAt);
		g_perfCameraRebuildCalls.fetch_add(1, std::memory_order_relaxed);
		g_perfCameraRebuildTotalUs.fetch_add(rebuildDurationUs, std::memory_order_relaxed);
		PerfUpdateMax(g_perfCameraRebuildMaxUs, rebuildDurationUs);
		const auto rebuildDurationMs =
			GetTickCount64() - transitionStartedAt;

		if (showPlayer) {
			EnsurePlayerHeadVisible("post-rebuild");
		}

		// The intentional camera rebuild is complete. The resolver hook may now
		// hold the established player target stable without touching scene nodes.
		ResetSpeakerHeadAnchorLock();
		g_playerShotEstablished.store(
			true,
			std::memory_order_release);

		// iSuppressCameraRecentering now suppresses only the target resolver while
		// an established player shot is active and DialogueMenu is still open.
		// Teardown always runs the native resolver.

		(void)SetPlayerRootAppCulled(
			!showPlayer,
			showPlayer ?
				"toggle-player" :
				"toggle-NPC");

		if (showPlayer) {
			// Do not mutate the facial animation graph inside the same task that
			// rebuilds DialogueCameraState. Apply it on a later frame instead.
			SchedulePlayerShotExpression();
			UpdateSpeakerObstruction(true);
		} else {
			CancelPendingPlayerShotExpression("NPC camera selected");
		}

		REX::INFO(
			"Rebuild returned; requested view={}, reason={}, duration={}ms",
			showPlayer ? "player" : "NPC",
			DescribeViewReason(reason),
			rebuildDurationMs);
	}

	void QueueView(
		const bool showPlayer,
		const ViewReason reason)
	{
		g_perfViewTransitionRequests.fetch_add(
			1,
			std::memory_order_relaxed);

		if (!g_taskInterface) {
			REX::CRITICAL("SFSE TaskInterface is unavailable");
			return;
		}

		const auto now = GetTickCount64();
		const auto retryAfter =
			g_viewTransitionRetryAfter.load(
				std::memory_order_acquire);

		if (retryAfter != 0 && now < retryAfter) {
			g_perfViewTransitionBackoffSkipped.fetch_add(
				1,
				std::memory_order_relaxed);
			return;
		}

		bool expected = false;
		if (!g_actionQueued.compare_exchange_strong(
				expected,
				true,
				std::memory_order_acq_rel)) {
			g_perfViewTransitionCoalesced.fetch_add(
				1,
				std::memory_order_relaxed);
			return;
		}

		REX::INFO(
			"Queuing dialogue view={}, reason={}",
			showPlayer ? "player" : "NPC",
			DescribeViewReason(reason));

		g_taskInterface->AddTask([showPlayer, reason]() {
			PerformSetViewOnTaskQueue(
				showPlayer,
				reason);
		});
	}

	void QueueManualToggle()
	{
		const bool showPlayer =
			!g_playerViewRequested.load(
				std::memory_order_acquire);

		const auto now = GetTickCount64();
		g_autoIgnoreUntil = std::max(
			g_autoIgnoreUntil,
			now + kManualCameraOverrideGraceMs);
		g_autoCandidateInitialized = false;
		g_autoCandidateSince = 0;

		REX::INFO(
			"Manual V camera override: requestedView={}, mode={}, autoResumeIn={}ms",
			showPlayer ? "player" : "NPC",
			g_switchMode == SwitchMode::kAutomatic ? "automatic" : "manual",
			kManualCameraOverrideGraceMs);

		QueueView(showPlayer, ViewReason::kManual);
	}

	void UpdateAutomaticCamera()
	{
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			~PerfScope() { PerfRecord(g_perfAutomaticCamera, startedAt); }
		} perfScope{ perfStartedAt };
		if (g_switchMode != SwitchMode::kAutomatic ||
			!g_dialogueOpen ||
			g_bodyPreparationPending ||
			g_actionQueued.load(std::memory_order_acquire)) {
			return;
		}

		const auto now = GetTickCount64();

		if (now < g_autoIgnoreUntil) {
			return;
		}

		bool validSpeaker = false;
		const bool speakerTalking =
			IsSpeakerTalking(validSpeaker);

		if (!validSpeaker) {
			return;
		}

		const bool desiredPlayerView = !speakerTalking;

		if (!g_autoCandidateInitialized ||
			desiredPlayerView != g_autoCandidatePlayer) {
			g_autoCandidateInitialized = true;
			g_autoCandidatePlayer = desiredPlayerView;
			g_autoCandidateSince = now;

			if (speakerTalking != g_lastSpeakerTalking) {
				REX::INFO(
					"Automatic phase: speaker={}, desiredView={}",
					speakerTalking ? "talking" : "silent",
					desiredPlayerView ? "player" : "NPC");
			}

			g_lastSpeakerTalking = speakerTalking;
			return;
		}

		const auto requiredDelay =
			desiredPlayerView ?
				g_autoPlayerDelayMs :
				g_autoNpcDelayMs;

		if (now - g_autoCandidateSince < requiredDelay) {
			return;
		}

		const bool currentPlayerView =
			g_playerViewRequested.load(
				std::memory_order_acquire);

		if (currentPlayerView == desiredPlayerView) {
			return;
		}

		QueueView(
			desiredPlayerView,
			desiredPlayerView ?
				ViewReason::kSpeakerStopped :
				ViewReason::kSpeakerStarted);
	}

	void Tick()
	{
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			~PerfScope() { PerfRecord(g_perfTick, startedAt); }
		} perfScope{ perfStartedAt };

		TickGuard guard;
		if (!guard) {
			return;
		}

		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return;
		}

		static const RE::BSFixedString dialogueMenu{
			kDialogueMenuName
		};

		const bool dialogueOpen =
			ui->IsMenuOpen(dialogueMenu);

		UpdateSpaceshipPilotingHistory();

		const bool dialogueOpening =
			dialogueOpen && !g_dialogueOpen;
		const bool openingShipDialogue =
			dialogueOpening && WasRecentlyPilotingShip();

		if (dialogueOpen &&
			!openingShipDialogue &&
			!g_shipDialogueBypass.load(std::memory_order_acquire)) {
			// Remember the actor, not just its current scene root. Root objects
			// can be replaced during perspective and dialogue teardown.
			(void)RememberDialogueSpeakerReference();
		}

		if (!dialogueOpen && !g_dialogueOpen) {
			// Intentionally do not force any gameplay camera after dialogue.
			// The opening body preparation leaves the game in a valid third-
			// person state; another teardown-time perspective transition is what
			// produced the camera-inside-head / floating-eyeballs bug.
			UpdateGameplayViewHistory();
		}

		if (dialogueOpen != g_dialogueOpen) {
			g_dialogueOpen = dialogueOpen;

			if (dialogueOpen) {
				if (openingShipDialogue) {
					g_shipDialogueBypass.store(
						true,
						std::memory_order_release);
					g_dialogueActiveForHooks.store(
						false,
						std::memory_order_release);
					g_playerViewRequested.store(
						false,
						std::memory_order_release);
					g_actionQueued.store(
						false,
						std::memory_order_release);
					g_bodyPreparationPending = false;
					g_bodyPreparationResumeAt = 0;
					g_bodyPreparationDeadlineAt = 0;
					ResetAutomaticState();

					REX::INFO(
						"Ship-piloting DialogueMenu detected; "
						"PointCameraAtPlayer fully bypassed for this session");
				} else {
				g_shipDialogueBypass.store(
					false,
					std::memory_order_release);
				g_exitPreRenderVisibilityRepaired.store(
					false,
					std::memory_order_release);
				SetCinematicPlayerLighting(g_cinematicPlayerLighting);
				ForceDialogueActorsVisible("dialogue-open-safety", false);
				// ForceDialogueActorsVisible repairs stale state from the previous
				// conversation, but it also reveals the player. Hide the player again
				// before the third-person warm-up can render a behind-the-back frame.
				if (g_forceThirdPersonOnDialogue) {
					(void)SetPlayerRootAppCulled(
						true,
						"dialogue-open-prehide");
				}
				g_exitVisibilityCleanupUntil.store(
					0,
					std::memory_order_release);
				g_exitVisibilityLastLateUpdateAt.store(
					0,
					std::memory_order_release);
				g_lastDialogueSpeakerReference.store(
					0,
					std::memory_order_release);
				g_lastDialogueSpeakerRoot.store(
					0,
					std::memory_order_release);
				g_culledSpeakerRoot.store(
					0,
					std::memory_order_release);
				g_nextObstructionCheckAt.store(
					0,
					std::memory_order_release);
				ResetPlayerHeadAnchorCache();
				g_dialogueActiveForHooks.store(
					true,
					std::memory_order_release);
				// Deliberately leave gameplay in third person after dialogue.
				g_restoreFirstPersonAfterDialogue = false;
				g_forcedThirdPersonForDialogue = false;
				g_firstPersonRestorePending = false;
				g_gameplayCameraRestoreDeadlineAt = 0;
				g_bodyPreparationPending = false;
				g_bodyPreparationResumeAt = 0;
				g_bodyPreparationDeadlineAt = 0;
				g_lastFunExpressionIndex = kExpressionCount;
				CancelPendingPlayerShotExpression("dialogue-open reset");

				// Force a known-good body state up front. Sheathing the weapon
				// and dropping to third person the instant dialogue opens lets
				// the engine build a fully posed external skeleton during the
				// dialogue-open moment, instead of trying to fix a half-built
				// first-person body mid-shot. The staged prep then settles into
				// the engine's natural NPC-facing view so the conversation
				// starts face to face (after a brief third-person beat).
				if (g_forceThirdPersonOnDialogue) {
					BeginDialogueOpenBodyPreparation();
				} else if (g_forceSheatheOnDialogue) {
					(void)SheathePlayerWeapon("dialogue-open");
				}

				(void)RememberDialogueSpeakerRoot();

				g_playerViewRequested.store(
					false,
					std::memory_order_release);
				g_playerCameraLocked.store(
					false,
					std::memory_order_release);
				g_playerShotEstablished.store(
					false,
					std::memory_order_release);
				ResetSpeakerHeadAnchorLock();
				ResetAutomaticState();
				g_autoIgnoreUntil =
					GetTickCount64() +
					g_autoStartupGraceMs;
				g_expressionActionQueued.store(
					false,
					std::memory_order_release);
				g_capturedController.store(
					0,
					std::memory_order_release);
				g_capturedControllerTarget.store(
					0,
					std::memory_order_release);

				g_perfDialogueIndex.fetch_add(1, std::memory_order_relaxed);
				ResetPerformanceDiagnosticsForDialogue();

				REX::INFO(
					"DialogueMenu opened; speaker=0x{:08X}, player=0x{:08X}, "
					"restoreFirstPerson={}",
					GetSpeakerHandle(),
					GetPlayerHandle(),
					g_restoreFirstPersonAfterDialogue ? "yes" : "no");
				}
			} else {
				const bool wasShipDialogue =
					g_shipDialogueBypass.exchange(
						false,
						std::memory_order_acq_rel);
				g_lastSpaceshipHudSeenAt = 0;

				if (wasShipDialogue) {
					g_dialogueActiveForHooks.store(
						false,
						std::memory_order_release);
					g_playerViewRequested.store(
						false,
						std::memory_order_release);
					g_actionQueued.store(
						false,
						std::memory_order_release);
					ResetAutomaticState();

					REX::INFO(
						"Ship dialogue closed; bypass cleared, "
						"native teardown left untouched");
				} else {
				// Capture the last live root before MenuTopicManager clears its
				// speaker handle, then reacquire from the actor during cleanup.
				(void)RememberDialogueSpeakerRoot();
				SetCinematicPlayerLighting(false);
				g_dialogueActiveForHooks.store(
					false,
					std::memory_order_release);
				CancelPendingBodyPreparation("dialogue-close");
				g_playerViewRequested.store(
					false,
					std::memory_order_release);
				g_playerCameraLocked.store(
					false,
					std::memory_order_release);
				g_playerShotEstablished.store(
					false,
					std::memory_order_release);
				ResetSpeakerHeadAnchorLock();
				BeginExitVisibilityCleanup();
				ResetPlayerHeadAnchorCache();
				CancelPendingPlayerShotExpression("dialogue-close");
				ClearPlayerExpressionOverride("dialogue-close");

				// Perform no camera reset during dialogue teardown. The game remains
				// in third person after the forced opening warm-up. This avoids
				// restoring first person while Starfield still has the head subtree
				// hidden, which placed the camera inside the player model.
				g_firstPersonRestorePending = false;
				g_firstPersonRestoreAt = 0;
				g_gameplayCameraRestoreDeadlineAt = 0;
				g_restoreFirstPersonAfterDialogue = false;
				g_forcedThirdPersonForDialogue = false;

				RestoreSpeakerObstruction("dialogue-close");
				ForceDialogueActorsVisible("dialogue-close-force-visible");

				ResetAutomaticState();
				g_expressionActionQueued.store(
					false,
					std::memory_order_release);
				g_capturedController.store(
					0,
					std::memory_order_release);
				g_capturedControllerTarget.store(
					0,
					std::memory_order_release);


				ResetDialogueControllerDedupState();
				g_viewTransitionRetryAfter.store(0, std::memory_order_relaxed);
				g_viewTransitionRetryWarningArmed.store(false, std::memory_order_relaxed);

				REX::INFO(
					"DialogueMenu closed; early target override disabled");
				}
			}
		}

		if (g_shipDialogueBypass.load(std::memory_order_acquire)) {
			return;
		}

		if (!dialogueOpen) {
			UpdateExitVisibilityCleanup();
		}

		UpdatePendingBodyPreparation();
		UpdateAutomaticCamera();
		UpdatePendingPlayerShotExpression();

		// Hotkeys only matter while dialogue is open. Polling GetAsyncKeyState
		// every gameplay frame is wasted work, so skip it when closed and reset
		// the edge-detect latches so the first in-dialogue press still fires.
		if (!dialogueOpen) {
			g_vWasDown = false;
			g_expressionWasDown = false;
			g_cinematicLightingToggleWasDown = false;
			return;
		}

		const bool vIsDown =
			(GetAsyncKeyState(g_manualSwitchKey) & 0x8000) != 0;

		if (vIsDown &&
			!g_vWasDown) {
			REX::INFO(
				"Camera switch key pressed; mode={}",
				g_switchMode == SwitchMode::kAutomatic ? "automatic" : "manual");
			QueueManualToggle();
		}

		g_vWasDown = vIsDown;

		const bool expressionIsDown =
			(GetAsyncKeyState(g_expressionCycleKey) & 0x8000) != 0;

		if (expressionIsDown &&
			!g_expressionWasDown) {
			REX::INFO("Expression cycle key pressed");
			QueueExpressionCycle();
		}

		g_expressionWasDown = expressionIsDown;

		const bool lightingToggleIsDown =
			(GetAsyncKeyState(g_cinematicLightingToggleKey) & 0x8000) != 0;

		if (lightingToggleIsDown &&
			!g_cinematicLightingToggleWasDown) {
			g_cinematicPlayerLighting = !g_cinematicPlayerLighting;
			SetCinematicPlayerLighting(g_cinematicPlayerLighting);

			REX::INFO(
				"Cinematic lighting toggle key pressed; lighting={}",
				g_cinematicPlayerLighting ? "on" : "off");
		}

		g_cinematicLightingToggleWasDown = lightingToggleIsDown;
	}
}

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	// Two CALL5 hooks need 28 bytes of branch-stub storage.
	SFSE::Init(a_sfse, {
		.trampoline = true,
		.trampolineSize = 96
	});
	return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);
	LoadSettings();
	if (g_expressionNotifications) {
		StartExpressionNotificationThread();
	}

	REX::INFO(
		"PointCameraAtPlayer 1.18.28 cinematic-light cleanup test loaded");
	REX::WARN(
		"Experimental build for Starfield 1.16.244.0 only");

	g_taskInterface = SFSE::GetTaskInterface();
	if (!g_taskInterface) {
		REX::CRITICAL("SFSE TaskInterface is unavailable");
		return false;
	}

	g_taskInterface->AddPermanentTask(Tick);

	REX::INFO(
		"1.18.28 keeps the 1.18.27 release-candidate fixes and changes cinematic "
		"dialogue lighting cleanup so stale/non-matching dialogue lights may be removed "
		"during repeated camera-rig rebuilds. No new INI option is required.");

	return true;
}
