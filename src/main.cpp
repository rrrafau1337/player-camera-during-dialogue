/*
POINTCAMERAATPLAYER - BEGINNER READING GUIDE
============================================

This file uses the chosen 1.18.16 issue-fix build as its behavioural base.
1.18.18 adds only the dialogue-selection native-response handoff described below.

No camera/body/dialogue timing or decision logic is intentionally changed.

Before each function/method:
  SIMPLE GUIDE
  What    = what this code does
  Why     = why the mod needs it
  Trigger = what causes it to run

Useful words:
- NPC: the character talking to the player.
- DialogueCameraState: Starfield's real cinematic conversation camera.
- hook: a doorway where this plugin observes or narrowly modifies a Starfield function.
- task queue: SFSE's safe list of work to run later.
- handle: Starfield's small numeric actor identifier.
- 3D root: the top scene object containing an actor's rendered body.
- AppCulled: an engine visibility flag used to hide/show scene objects.

Logging:
- [Debug] iDebugLogging=1 logs normal event/state diagnostics; method tracing is disabled in this test build.
- [Debug] iPerformanceLogging=1 independently enables performance timing/snapshots.
- warnings/errors remain visible regardless of either switch.
*/

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
#include "RE/IDs_VTABLE.h"
#include "RE/B/BGSKeyword.h"
#include "RE/N/NiAVObject.h"
#include "RE/N/NiNode.h"
#include "RE/P/PlayerCamera.h"
#include "RE/S/ScaleformGFxFunctionHandler.h"
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
#include <vector>
#include <cwchar>

// Runtime logging policy.
//
// [Debug] iDebugLogging=1:
//   - enables all normal diagnostic/event/state logs;
//   - method-level [CALL] tracing is disabled in this test build.
//
// [Debug] iPerformanceLogging=1:
//   - independently enables QPC timing for existing performance scopes;
//   - emits periodic and dialogue-boundary [PERF] snapshots.
//
// WARN / ERROR / CRITICAL remain unconditional.
#define PDC_DIAGNOSTIC_INFO(...) \
	do { \
		if (g_debugLogging) { \
			REX::INFO(__VA_ARGS__); \
		} \
	} while (false)

#define PDC_RELEASE_INFO(...) \
	do { \
		if (g_debugLogging) { \
			REX::INFO(__VA_ARGS__); \
		} \
	} while (false)

#define PDC_PERF_INFO(...) \
	do { \
		if (g_performanceLogging) { \
			REX::INFO(__VA_ARGS__); \
		} \
	} while (false)

#define PDC_CONCAT_INNER(a, b) a##b
#define PDC_CONCAT(a, b) PDC_CONCAT_INNER(a, b)

#define PDC_TRACE_SCOPE() ((void)0)

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
	[[nodiscard]] RE::TESObjectREFR* GetSpeakerReference();
	[[nodiscard]] RE::NiAVObject* GetReferenceRoot3D(RE::TESObjectREFR* reference);
	[[nodiscard]] RE::NiAVObject* GetPlayerRoot3D();
	void PerfUpdateMax(std::atomic_uint64_t& destination, std::uint64_t value);
	[[nodiscard]] bool IsAccessibleControllerField(
		void* controller,
		std::ptrdiff_t offset,
		bool requireWritable);

	const SFSE::TaskInterface* g_taskInterface = nullptr;

	SwitchMode g_switchMode = SwitchMode::kAutomatic;
	int g_manualSwitchKey = 'V';
	int g_expressionCycleKey = 'X';
	// Global emergency/native-mode toggle. F10 by default; configurable via INI.
	int g_pluginBypassToggleKey = VK_F10;

	// Logging only. These flags must never be consulted by camera/dialogue/body
	// logic; they control observation, not behaviour.
	bool g_debugLogging = false;
	bool g_performanceLogging = false;
	std::atomic_uint64_t g_debugTraceLinesWritten{ 0 };
	constexpr std::uint64_t kPerformanceSnapshotIntervalMs = 1000;

	// Performance diagnostic controls. These do not change gameplay behavior.
	// Keep method tracing OFF for the primary FPS reproduction because writing
	// two log lines for every hot-path call can become the bottleneck itself.
	std::uint32_t g_autoPlayerDelayMs = 250;
	std::uint32_t g_autoNpcDelayMs = 60;
	std::uint32_t g_autoStartupGraceMs = 350;

	// Avoid switching to the player during short silent gaps between ordered
	// NPC voice fragments. Older branches used a conservative 2s hold for this
	// exact reason; make it configurable and slightly less sluggish by default.
	std::uint32_t g_autoFragmentSilenceHoldMs = 1500;
	// Explicit V camera changes remain authoritative briefly even in automatic
	// mode. Automatic switching resumes after this bounded grace period.
	constexpr std::uint32_t kManualCameraOverrideGraceMs = 1000;
	bool g_obstructionHandling = true;
	std::uint32_t g_obstructionRadiusPercent = 70;
	bool g_fixFirstPersonBody = true;
	std::uint32_t g_firstPersonBodyPrepDelayMs = 250;
	// Legacy opening retry/deadline value. DialogueCameraState itself is now
	// requested immediately, so this no longer delays visible dialogue entry.
	std::uint32_t g_dialogueOpenCameraDelayMs = 350;
	// DialogueMenu's logical open flag drops before Starfield has actually left
	// DialogueCameraState. Treat that interval as a real Closing lifecycle phase
	// and require the gameplay camera to remain out of DialogueCameraState for
	// this small settle window before final exit cleanup begins.
	std::uint32_t g_dialogueExitCameraSettleMs = 50;
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
	RE::NiTransform g_lockedSpeakerHeadWorld{

	};
	RE::NiTransform g_lockedSpeakerHeadPreviousWorld{};
	RE::NiPoint3 g_lockedSpeakerHeadBoundCenter{};
	float g_lockedSpeakerHeadBoundRadius = 0.0F;
	bool g_speakerHeadAnchorLocked = false;
	std::atomic_uint32_t g_suppressedTargetResolveCount = 0;
	std::atomic_uint32_t g_headVisibilityLogCount = 0;
	std::atomic_uint32_t g_headFramingLogCount = 0;
	std::atomic_flag g_playerFramingOverrideLock = ATOMIC_FLAG_INIT;
	// Master ownership switch. OFF means all dialogue hooks are native pass-through.
	std::atomic_bool g_dialogueLogicEnabled = false;
	std::atomic_bool g_dialogueActiveForHooks = false;
	std::atomic_bool g_shipDialogueBypass = false;
	std::atomic_bool g_shipGameplayBypass = false;
	std::atomic_bool g_scriptedSceneBypass = false;

	// Dialogue-choice native-response handoff.
	//
	// Exact flow:
	//   DialogueMenu_OnDialogueSelect callback fires
	//     -> publish this flag BEFORE Starfield processes the selection
	//     -> our dialogue/controller/resolver hooks become native pass-through
	//     -> Tick waits until the NPC actually starts speaking
	//     -> all ordered voice fragments remain native-owned
	//     -> after g_autoFragmentSilenceHoldMs of continuous post-speech silence,
	//        normal plugin camera logic resumes.
	//
	// This is intentionally separate from F10's user bypass. It is automatic,
	// scoped to one selected response, and does not tear down dialogue logic.
	std::atomic_bool g_dialogueChoiceNativeWindow = false;
	std::atomic_bool g_dialogueChoiceSawNpcSpeech = false;
	std::atomic_bool g_dialogueChoiceCleanupPending = false;
	std::atomic_uint64_t g_dialogueChoiceStartedAt = 0;
	std::atomic_uint64_t g_dialogueChoiceSilenceSince = 0;

	RE::CameraState g_lastObservedDialogueCameraState = RE::CameraState::kTotal;
	RE::CameraState g_scriptedCameraCandidateState = RE::CameraState::kTotal;
	ULONGLONG g_scriptedCameraCandidateSince = 0;

	constexpr ULONGLONG kScriptedNonDialogueConfirmMs = 250;
	// User-controlled global bypass. While set, every plugin hook becomes a
	// transparent pass-through and Tick performs no camera/expression/visibility
	// work beyond watching this toggle so the user can turn the mod back on.
	std::atomic_bool g_userPluginBypass = false;
	ULONGLONG g_lastSpaceshipHudSeenAt = 0;
	constexpr std::uint32_t kRecentSpaceshipHudWindowMs = 2000;
	std::atomic_uintptr_t g_lastDialogueSpeakerReference = 0;
	std::atomic_uintptr_t g_lastDialogueSpeakerRoot = 0;
	std::atomic_uint64_t g_exitVisibilityCleanupUntil = 0;
	std::atomic_uint64_t g_exitVisibilityLastLateUpdateAt = 0;
	std::atomic_uint64_t g_exitVisibilityStartedAt = 0;
	std::atomic_uint64_t g_exitVisibilityLastRepairAt = 0;
	std::atomic_uint32_t g_exitVisibilityRepairStage = 0;
	std::atomic_bool g_exitVisibilityPulsePending = false;
	std::atomic_uint64_t g_exitVisibilityRepairCount = 0;
	std::atomic_uint64_t g_exitVisibilityPulseCount = 0;

	// Diagnostic recovery for the deeper post-dialogue 3D state bug. Real logs
	// show both actor roots present with AppCulled clear while neither actor is
	// rendered; a manual first/third-person transition restores both. Reproduce
	// that engine transition once after teardown settles so we can prove whether
	// the missing-model state lives below our visibility flags.
	std::atomic_uint32_t g_exitPerspectiveRefreshStage = 0;
	std::atomic_uint64_t g_exitPerspectiveRefreshAt = 0;
	std::atomic_uintptr_t g_exitPerspectiveRefreshSpeakerReference = 0;
	std::atomic_bool g_exitPerspectiveRefreshOriginalFirstPerson = false;
	constexpr std::uint64_t kExitPerspectiveRefreshStepMs = 32;
	constexpr std::uint64_t kExitPerspectiveRefreshVerifyMs = 64;

	// A DialogueCameraState teardown update can run after Tick has repaired
	// visibility but before the first gameplay frame is rendered. Repair once
	// more at the end of that native update so the outgoing speaker/player do
	// not vanish for the single transition frame.
	std::atomic_bool g_exitPreRenderVisibilityRepaired = false;
	// Keep the teardown safety net alive long enough to catch Starfield's late
	// speaker scene-root swap without fighting gameplay player visibility.
	// Late native updates are observed only; they never directly trigger a repair.
	constexpr std::uint64_t kExitVisibilityMinRepairMs = 600;
	constexpr std::array<std::uint64_t, 5> kExitVisibilityRepairScheduleMs{
		100,
		300,
		600,
		1000,
		1500
	};
	// If DialogueCameraState is still updating when the configured cleanup
	// window expires, keep only the speaker-repair safety net alive briefly.
	// Never extend beyond this hard ceiling from menu close.
	constexpr std::uint64_t kExitVisibilityLateTailMs = 500;
	constexpr std::uint64_t kExitVisibilityHardMaxMs = 3000;

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

	// DialogueMenu closes before the visual/camera teardown has actually
	// completed. During this bounded phase all camera targeting logic is already
	// disabled, but the outgoing player shot may still need its presentation
	// preserved until PlayerCamera genuinely leaves DialogueCameraState.
	// Dialogue opening has the same ownership problem as closing, just in the
	// opposite direction: DialogueMenu can become visible while Starfield is
	// still constructing/publishing DialogueCameraState. During this handshake
	// all three hooked native paths are strict pass-throughs. Once we have seen
	// several real DialogueCameraState updates, the plugin attaches to the live
	// native state instead of trying to manufacture/rebuild it prematurely.
	std::atomic_bool g_dialogueOpeningNative = false;
	std::atomic_uint64_t g_dialogueOpeningStartedAt = 0;
	std::atomic_uint64_t g_dialogueOpeningLastUpdateAt = 0;
	std::atomic_uint64_t g_dialogueOpeningUpdateCount = 0;
	std::atomic_uintptr_t g_dialogueOpeningObservedState = 0;
	std::atomic_bool g_dialogueOpeningFailedPassive = false;

	// Some dialogue scenes do not create/run DialogueCameraState until several
	// seconds after DialogueMenu opens (sometimes only around the first response).
	// Prime the external player body BEFORE that cinematic camera exists:
	// genuine gameplay FirstPerson -> ThirdPerson -> FirstPerson. This lets the
	// engine build the third-person body cache, restores the user's original
	// gameplay view immediately, and then leaves camera ownership entirely native.
	enum class PreDialogueBodyPrimeStage : std::uint32_t
	{
		kNone = 0,
		kWaitArm,
		kAcquireThirdPerson,
		kAcquireFirstPerson,
		kComplete,
		kMissedNativeDialogue,
		kFailed
	};

	PreDialogueBodyPrimeStage g_preDialogueBodyPrimeStage =
		PreDialogueBodyPrimeStage::kNone;
	ULONGLONG g_preDialogueBodyPrimeArmAt = 0;
	ULONGLONG g_preDialogueBodyPrimeDeadlineAt = 0;
	ULONGLONG g_preDialogueBodyPrimeNextRetryAt = 0;
	std::uint32_t g_preDialogueBodyPrimeThirdAttempts = 0;
	std::uint32_t g_preDialogueBodyPrimeFirstAttempts = 0;

	constexpr ULONGLONG kPreDialogueBodyPrimeArmDelayMs = 80;
	constexpr ULONGLONG kPreDialogueBodyPrimeRetryMs = 48;
	constexpr ULONGLONG kPreDialogueBodyPrimeTimeoutMs = 700;

	// First-person-origin conversations need one real FP -> TP perspective
	// boundary crossing to initialise the external player body correctly.
	// Do it while native Starfield still owns the NPC opening line, BEFORE
	// enabling any mod camera logic or accepting the first player-facing shot.
	std::atomic_bool g_dialogueOpeningBodyPulseIssued = false;
	std::atomic_bool g_dialogueOpeningBodyPulseRestoreRequested = false;
	std::atomic_uint64_t g_dialogueOpeningBodyPulseIssuedAt = 0;
	std::atomic_uint64_t g_dialogueOpeningBodyPulseRestoreAt = 0;
	std::atomic_uint64_t g_dialogueOpeningBodyPulseBaselineUpdates = 0;
	std::atomic_uint64_t g_dialogueOpeningBodyPulseNextRestoreAt = 0;
	std::atomic_uint32_t g_dialogueOpeningBodyPulseRestoreAttempts = 0;

	constexpr std::uint32_t kDialogueOpeningStableUpdates = 3;
	constexpr std::uint64_t kDialogueLogicNativeMinMs = 150;
	constexpr std::uint64_t kDialogueLogicFallbackArmMs = 1200;

	constexpr std::uint64_t kOpeningBodyPulseRestoreDelayMs = 16;
	constexpr std::uint64_t kOpeningBodyPulseSettleMs = 80;
	constexpr std::uint64_t kOpeningBodyPulseRestoreRetryMs = 48;
	constexpr std::uint64_t kOpeningBodyPulseTimeoutMs = 1000;
	constexpr std::uint64_t kOpeningBodyPulseStableUpdates = 4;

	std::atomic_bool g_dialogueClosing = false;
	std::atomic_bool g_dialogueClosingKeepPlayerVisible = false;
	std::atomic_uint64_t g_dialogueClosingStartedAt = 0;
	std::atomic_uint64_t g_dialogueClosingCameraLeftAt = 0;
	std::atomic_uint64_t g_dialogueClosingUpdateCount = 0;

	bool g_vWasDown = false;
	bool g_expressionWasDown = false;
	bool g_pluginBypassToggleWasDown = false;
	std::size_t g_expressionIndex = 0;
	std::size_t g_lastFunExpressionIndex = kExpressionCount;
	std::uint32_t g_funRandomState = 0;
	bool g_gameplayViewKnown = false;
	bool g_lastGameplayFirstPerson = false;

	// Starfield can briefly pass through ThirdPerson while beginning dialogue
	// from first person, before DialogueMenu's logical open edge is visible to
	// Tick(). Committing that transient sample overwrote the real entry
	// perspective and prevented the staged external-body fix from running.
	//
	// Track perspective as a short-lived candidate and only commit a CHANGE
	// after it has remained stable long enough to represent actual gameplay
	// rather than dialogue-camera construction.
	bool g_gameplayViewCandidateKnown = false;
	bool g_gameplayViewCandidateFirstPerson = false;
	std::uint64_t g_gameplayViewCandidateSince = 0;
	constexpr std::uint64_t kGameplayViewPerspectiveCommitMs = 120;

	bool g_restoreFirstPersonAfterDialogue = false;
	bool g_forcedThirdPersonForDialogue = false;
	bool g_firstPersonRestorePending = false;
	ULONGLONG g_firstPersonRestoreAt = 0;
	ULONGLONG g_gameplayCameraRestoreDeadlineAt = 0;
	bool g_bodyPreparationPending = false;
	ULONGLONG g_bodyPreparationResumeAt = 0;
	ULONGLONG g_bodyPreparationDeadlineAt = 0;

	// A reliable repair requires a real-time FPP -> TPP transition, not merely
	// camera-state pointer changes between ~900Hz permanent-task ticks.
	enum class FirstPersonBodyRepairStage : std::uint32_t
	{
		kNone = 0,
		kAcquireFirstPerson,
		kHoldFirstPerson,
		kAcquireThirdPerson,
		kHoldThirdPerson,
		kRestoreDialogue,
		kSettleDialogue
	};

	FirstPersonBodyRepairStage g_firstPersonBodyRepairStage =
		FirstPersonBodyRepairStage::kNone;
	ULONGLONG g_firstPersonBodyRepairStageStartedAt = 0;
	ULONGLONG g_firstPersonBodyRepairNextRetryAt = 0;
	ULONGLONG g_firstPersonBodyRepairDialogueRestoreAt = 0;
	std::uint32_t g_firstPersonBodyRepairFirstAttempts = 0;
	std::uint32_t g_firstPersonBodyRepairThirdAttempts = 0;
	std::uint32_t g_firstPersonBodyRepairDialogueAttempts = 0;
	std::atomic_uint64_t g_firstPersonBodyRepairDialogueUpdates{ 0 };

	constexpr ULONGLONG kFirstPersonBodyRepairHoldFirstMs = 48;
	constexpr ULONGLONG kFirstPersonBodyRepairHoldThirdMs = 96;
	constexpr ULONGLONG kFirstPersonBodyRepairDialogueSettleMs = 80;
	constexpr ULONGLONG kFirstPersonBodyRepairRetryMs = 16;
	constexpr ULONGLONG kFirstPersonBodyRepairTimeoutMs = 1800;
	constexpr std::uint64_t kFirstPersonBodyRepairDialogueStableUpdates = 4;

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

	// Hot-path diagnostics/optimisation counters. Controller field pages are
	// validated once when the primary live controller is first observed, then
	// direct atomic field access is used for the rest of that dialogue.
	std::atomic_bool g_primaryDialogueControllerFieldsValidated{ false };
	std::atomic_uint64_t g_perfControllerFieldValidationAttempts{ 0 };

	// SetAppCulled is a virtual call. Cache the resolved/validated slot per
	// vtable so hot visibility paths do not call VirtualQuery every frame.
	struct VisibilityVfuncCacheEntry
	{
		std::atomic_uintptr_t vtable{ 0 };
		std::atomic_uintptr_t function{ 0 };
	};
	std::array<VisibilityVfuncCacheEntry, 8> g_visibilityVfuncCache{};
	std::atomic_uint64_t g_perfVisibilityVfuncCacheHits{ 0 };
	std::atomic_uint64_t g_perfVisibilityVfuncCacheMisses{ 0 };
	std::atomic_uint64_t g_perfVisibilityRootValidationCalls{ 0 };

	std::atomic_uintptr_t g_primaryDialogueController{ 0 };
	std::array<std::atomic_uintptr_t, 16> g_seenDialogueControllers{};
	std::atomic_uint64_t g_seenDialogueControllerCount{ 0 };
	std::atomic_uint64_t g_secondaryControllerUpdatesPassthrough{ 0 };
	std::atomic_uint64_t g_perfPrevSecondaryControllerUpdatesPassthrough{ 0 };

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

	// -----------------------------------------------------------------------
	// Performance / resource diagnostics
	// -----------------------------------------------------------------------

	// SIMPLE GUIDE
	// What: Writes one [CALL] line when a method starts and one when it finishes.
	// Why: With iDebugLogging=1 the log becomes a readable execution trace without
	//      changing camera decisions or timing constants.
	// Trigger: Constructed automatically by PDC_TRACE_SCOPE().
	class DebugMethodScope
	{
	public:
		// SIMPLE GUIDE
		// What: Creates a tiny logger for one method call.
		// Why: When general debug logging is on, this produces the '[CALL] ->' line without changing the method's decisions.
		// Trigger: PDC_TRACE_SCOPE() creates it automatically when a traced method starts.
		explicit DebugMethodScope(const char* methodName) :
			name(methodName ? methodName : "unknown"),
			enabled(g_debugLogging),
			threadId(enabled ? GetCurrentThreadId() : 0)
		{
			if (!enabled) {
				return;
			}

			g_debugTraceLinesWritten.fetch_add(1, std::memory_order_relaxed);
			REX::INFO("[CALL] -> {} tid={}", name, threadId);
		}

		// SIMPLE GUIDE
		// What: Writes the matching '[CALL] <-' line when an instrumented method ends.
		// Why: Paired start/end lines make the full execution path readable even when a method returns early.
		// Trigger: Runs automatically when the method's DebugMethodScope object goes away.
		~DebugMethodScope()
		{
			if (!enabled) {
				return;
			}

			g_debugTraceLinesWritten.fetch_add(1, std::memory_order_relaxed);
			REX::INFO("[CALL] <- {} tid={}", name, threadId);
		}

		DebugMethodScope(const DebugMethodScope&) = delete;
		DebugMethodScope& operator=(const DebugMethodScope&) = delete;

	private:
		const char* name;
		bool enabled;
		DWORD threadId;
	};

	// SIMPLE GUIDE
	// What: Keeps an old diagnostic call site harmless; this function does no resource sampling.
	// Why: The chosen base intentionally does not bring the old memory telemetry subsystem back.
	// Trigger: Runs only if an existing compatibility call site invokes it.
	void CaptureDialogueMemoryBaseline() noexcept
	{
		PDC_TRACE_SCOPE();
	}

	// SIMPLE GUIDE
	// What: Keeps old resource-telemetry call sites harmless; it only participates in method tracing.
	// Why: This preserves the base runtime profile instead of reintroducing PDH/Psapi monitoring.
	// Trigger: Called from existing Tick/close diagnostic call sites.
	void LogResourceTelemetry(const char*) noexcept
	{
		PDC_TRACE_SCOPE();
	}


	// SIMPLE GUIDE
	// What: Turns a camera-switch reason into a short readable word.
	// Why: Readable names make logs and debugging easier than raw numbers.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] const char* DescribeViewReason(
		const ViewReason reason)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Checks whether one character is a plain space, tab, or new line.
	// Why: Settings text often has extra blank characters that should be ignored.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsAsciiWhitespace(const char value)
	{
		PDC_TRACE_SCOPE();
		return value == ' ' ||
			value == '\t' ||
			value == '\r' ||
			value == '\n';
	}

	// SIMPLE GUIDE
	// What: Changes an English capital letter into its small-letter form.
	// Why: It lets the settings parser compare words without caring about capitals.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] char ToLowerAscii(const char value)
	{
		PDC_TRACE_SCOPE();
		return value >= 'A' && value <= 'Z' ?
			static_cast<char>(value + ('a' - 'A')) :
			value;
	}

	// SIMPLE GUIDE
	// What: Checks whether two simple English strings say the same thing, ignoring capitals.
	// Why: Users should not have to match exact upper/lower-case spelling in the INI.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool EqualsIgnoreCaseAscii(
		const std::string_view left,
		const std::string_view right)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Cuts blank spaces from the beginning and end of a piece of text.
	// Why: This makes INI values easier and safer to understand.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] std::string_view TrimAscii(
		std::string_view value)
	{
		PDC_TRACE_SCOPE();
		while (!value.empty() && IsAsciiWhitespace(value.front())) {
			value.remove_prefix(1);
		}

		while (!value.empty() && IsAsciiWhitespace(value.back())) {
			value.remove_suffix(1);
		}

		return value;
	}

	// SIMPLE GUIDE
	// What: Reads the chosen default face expression from the INI and finds its matching expression entry.
	// Why: The mod needs one clean, known expression choice before it can apply it to the player.
	// Trigger: Called by the subsystem described by this helper's role.
	void LoadDefaultExpressionSetting()
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Reads all user options from PointCameraAtPlayer.ini and keeps them in memory.
	// Why: Every later camera decision needs the user's chosen keys, delays, and feature switches.
	// Trigger: Called by the subsystem described by this helper's role.
	void LoadSettings()
	{
		PDC_TRACE_SCOPE();

		g_debugLogging =
			GetPrivateProfileIntA(
				"Debug",
				"iDebugLogging",
				0,
				kIniPath) != 0;

		g_performanceLogging =
			GetPrivateProfileIntA(
				"Debug",
				"iPerformanceLogging",
				0,
				kIniPath) != 0;

		if (g_debugLogging) {
			REX::INFO(
				"[DEBUG] general debug logging enabled; method-level [CALL] tracing is active");
		}

		if (g_performanceLogging) {
			PDC_PERF_INFO(
				"[PERF] performance logging enabled; QPC timing and {}ms snapshots are active",
				kPerformanceSnapshotIntervalMs);
		}

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

		g_pluginBypassToggleKey =
			GetPrivateProfileIntA(
				"Settings",
				"iPluginBypassToggleKey",
				VK_F10,
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

		g_autoFragmentSilenceHoldMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iAutoFragmentSilenceHoldMs",
					1500,
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

		g_dialogueExitCameraSettleMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iDialogueExitCameraSettleMs",
					50,
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
		g_autoFragmentSilenceHoldMs =
			std::clamp(g_autoFragmentSilenceHoldMs, 250u, 5000u);

		g_obstructionRadiusPercent =
			std::clamp(g_obstructionRadiusPercent, 20u, 150u);
		g_firstPersonBodyPrepDelayMs =
			std::clamp(g_firstPersonBodyPrepDelayMs, 32u, 1000u);
		g_dialogueOpenCameraDelayMs =
			std::clamp(g_dialogueOpenCameraDelayMs, 100u, 1500u);
		g_dialogueExitCameraSettleMs =
			std::min(g_dialogueExitCameraSettleMs, 500u);
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

		PDC_DIAGNOSTIC_INFO(
			"Settings: mode={}, manualKey={}, expressionKey={}, bypassKey={}, "
			"playerDelay={}ms, npcDelay={}ms, startupGrace={}ms, "
			"obstruction={}, obstructionRadius={}%, firstPersonFix={}, "
			"bodyPrepDelay={}ms, dialogueOpenDelay={}ms, "
			"dialogueExitCameraSettle={}ms, firstPersonRestoreDelay={}ms, "
			"headVisibilityFix={}, headFraming={}, framingHeight={}%, "
			"stableVisibility={}, expressionNotifications={}, "
			"notificationDuration={}ms, exitCleanup={}ms settle={}ms, "
			"obstructionCheck={}ms, headCheck={}ms, lockPlayerCamera={}, "
			"forceSheathe={}, forceThirdPerson={}, suppressRecentering={}, "
			"funMode={}, nativeLighting=untouched, defaultExpression={}, "
			"debugLogging={}, performanceLogging={}",
			g_switchMode == SwitchMode::kAutomatic ?
				"automatic" :
				"manual",
			g_manualSwitchKey,
			g_expressionCycleKey,
			g_pluginBypassToggleKey,
			g_autoPlayerDelayMs,
			g_autoNpcDelayMs,
			g_autoStartupGraceMs,
			g_obstructionHandling ? "on" : "off",
			g_obstructionRadiusPercent,
			g_fixFirstPersonBody ? "on" : "off",
			g_firstPersonBodyPrepDelayMs,
			g_dialogueOpenCameraDelayMs,
			g_dialogueExitCameraSettleMs,
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
			g_defaultExpressionIndex < kExpressionCount ?
				kExpressionNames[g_defaultExpressionIndex] :
				"disabled",
			g_debugLogging ? "on" : "off",
			g_performanceLogging ? "on" : "off");


	}





	// SIMPLE GUIDE
	// What: Finds the NPC who is currently speaking in the active dialogue.
	// Why: The camera needs to know which actor is the conversation partner.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] RE::TESObjectREFR* GetSpeakerReference()
	{
		PDC_TRACE_SCOPE();
		auto* topicManager =
			RE::MenuTopicManager::GetSingleton();

		if (!topicManager) {
			return nullptr;
		}

		auto speaker = topicManager->speaker.get();
		return speaker ? speaker.get() : nullptr;
	}

	// SIMPLE GUIDE
	// What: Checks whether the current dialogue NPC exists and is speaking right now.
	// Why: Automatic mode uses speech starting and stopping to decide which camera should be shown.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsSpeakerTalking(
		bool& validSpeaker)
	{
		PDC_TRACE_SCOPE();
		auto* speaker = GetSpeakerReference();

		validSpeaker = speaker != nullptr;
		return speaker ? speaker->IsTalking() : false;
	}

	// SIMPLE GUIDE
	// What: Clears the automatic-camera timer and remembered speaker state.
	// Why: A new dialogue or manual action must not inherit timing from an older camera decision.
	// Trigger: Called by the subsystem described by this helper's role.
	void ResetAutomaticState()
	{
		PDC_TRACE_SCOPE();
		g_autoCandidateInitialized = false;
		g_autoCandidatePlayer = false;
		g_lastSpeakerTalking = false;
		g_autoCandidateSince = 0;
		g_autoIgnoreUntil = 0;
	}

	class TickGuard
	{
	public:
		// SIMPLE GUIDE
		// What: Tries to put a tiny 'busy' sign on the main Tick so only one Tick can run at a time.
		// Why: Two overlapping heartbeats could both move the camera and fight each other.
		// Trigger: Called by the subsystem described by this helper's role.
		TickGuard() :
			locked(!g_tickLock.test_and_set(std::memory_order_acquire))
		{
			PDC_TRACE_SCOPE();
		}

		// SIMPLE GUIDE
		// What: Takes down the Tick 'busy' sign when the Tick finishes.
		// Why: This lets the next heartbeat run, even when the current one returns early.
		// Trigger: Called by the subsystem described by this helper's role.
		~TickGuard()
		{
			PDC_TRACE_SCOPE();
			if (locked) {
				g_tickLock.clear(std::memory_order_release);
			}
		}

		// SIMPLE GUIDE
		// What: Answers whether TickGuard successfully got the one-at-a-time lock.
		// Why: Tick can simply ask the guard yes/no before doing any work.
		// Trigger: Called by the subsystem described by this helper's role.
		[[nodiscard]] explicit operator bool() const noexcept
		{
			PDC_TRACE_SCOPE();
			return locked;
		}

	private:
		bool locked;
	};

	// SIMPLE GUIDE
	// What: Gets Starfield's real DialogueCameraState only when that state is actually the current camera.
	// Why: The mod must not write dialogue-camera memory when Starfield is using a different camera.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] void* GetActiveDialogueState()
	{
		PDC_TRACE_SCOPE();
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
	// SIMPLE GUIDE
	// What: Asks Starfield whether the DialogueMenu is open right now.
	// Why: Queued work can happen later than Tick, so it needs a fresh answer instead of an old remembered flag.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsDialogueMenuOpen()
	{
		PDC_TRACE_SCOPE();
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		static const RE::BSFixedString dialogueMenu{ kDialogueMenuName };
		return ui->IsMenuOpen(dialogueMenu);
	}


	// SIMPLE GUIDE
	// What: Asks whether the spaceship flying HUD is currently open.
	// Why: Ship cameras have their own body rules, so dialogue fixes should avoid interfering with them.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsSpaceshipHudOpen()
	{
		PDC_TRACE_SCOPE();
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}

		static const RE::BSFixedString spaceshipHudMenu{
			kSpaceshipHudMenuName
		};
		return ui->IsMenuOpen(spaceshipHudMenu);
	}

	// SIMPLE GUIDE
	// What: Remembers the last moment the spaceship HUD was seen.
	// Why: A dialogue can begin just after leaving ship controls, so a tiny bit of history helps detect that case.
	// Trigger: Called by the subsystem described by this helper's role.
	void UpdateSpaceshipPilotingHistory()
	{
		PDC_TRACE_SCOPE();
		if (IsSpaceshipHudOpen()) {
			g_lastSpaceshipHudSeenAt = GetTickCount64();
		}
	}

	// SIMPLE GUIDE
	// What: Checks whether ship controls were active very recently.
	// Why: This helps the mod decide when normal dialogue camera work should stay out of the way.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool WasRecentlyPilotingShip()
	{
		PDC_TRACE_SCOPE();
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


	// SIMPLE GUIDE
	// What: Finds the enum name for the PlayerCamera state object currently in charge.
	// Why: Scripted-scene detection needs to know whether the camera is dialogue, free, furniture, third person, and so on.
	// Trigger: Called by cinematic/scripted-scene detection.
	[[nodiscard]] RE::CameraState GetCurrentPlayerCameraState()
	{
		PDC_TRACE_SCOPE();
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->currentState) {
			return RE::CameraState::kTotal;
		}

		for (std::size_t i = 0;
			i < static_cast<std::size_t>(RE::CameraState::kTotal);
			++i) {
			if (camera->cameraStates[i] == camera->currentState) {
				return static_cast<RE::CameraState>(i);
			}
		}

		return RE::CameraState::kTotal;
	}

	// SIMPLE GUIDE
	// What: Turns a camera-state enum into a readable word.
	// Why: Readable camera names make cinematic logs useful to humans.
	// Trigger: Called only when camera state is being logged or explained.
	[[nodiscard]] const char* DescribeCameraState(const RE::CameraState state)
	{
		PDC_TRACE_SCOPE();
		switch (state) {
		case RE::CameraState::kFirstPerson: return "first-person";
		case RE::CameraState::kIronSights: return "iron-sights";
		case RE::CameraState::kPCTransition: return "PC-transition";
		case RE::CameraState::kTween: return "tween";
		case RE::CameraState::kVehicle: return "vehicle";
		case RE::CameraState::kFlight: return "flight";
		case RE::CameraState::kShipFarTravel: return "ship-far-travel";
		case RE::CameraState::kShipAction: return "ship-action";
		case RE::CameraState::kShipTargeting: return "ship-targeting";
		case RE::CameraState::kShipCombatOrbit: return "ship-combat-orbit";
		case RE::CameraState::kFreeWalk: return "free-walk";
		case RE::CameraState::kFreeAdvanced: return "free-advanced";
		case RE::CameraState::kFreeFly: return "free-fly";
		case RE::CameraState::kFreeTethered: return "free-tethered";
		case RE::CameraState::kDialogue: return "dialogue";
		case RE::CameraState::kPhotoMode: return "photo-mode";
		case RE::CameraState::kThirdPerson: return "third-person";
		case RE::CameraState::kFurniture: return "furniture";
		case RE::CameraState::kBleedout: return "bleedout";
		default: return "other";
		}
	}

	// SIMPLE GUIDE
	// What: Checks whether the camera is a strong scripted/free/furniture state.
	// Why: Those states are good evidence that a custom cutscene should keep native camera ownership.
	// Trigger: Called by UpdateScriptedSceneDetection.
	[[nodiscard]] bool IsHardScriptedCameraState(const RE::CameraState state)
	{
		PDC_TRACE_SCOPE();
		return state == RE::CameraState::kFreeWalk ||
			state == RE::CameraState::kFreeAdvanced ||
			state == RE::CameraState::kFreeFly ||
			state == RE::CameraState::kFreeTethered ||
			state == RE::CameraState::kFurniture;
	}


	// SIMPLE GUIDE
	// What: Reads one actor handle number from a known place inside DialogueCameraState.
	// Why: The mod needs those small IDs to tell Starfield whether the camera target is the NPC or player.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] std::uint32_t ReadHandle(
		void* dialogueState,
		const std::ptrdiff_t offset)
	{
		PDC_TRACE_SCOPE();
		const auto* field =
			reinterpret_cast<const std::uint32_t*>(
				static_cast<std::byte*>(dialogueState) + offset);

		return std::atomic_ref<const std::uint32_t>(*field).load(
			std::memory_order_acquire);
	}

	// SIMPLE GUIDE
	// What: Writes one actor handle number into a known DialogueCameraState field.
	// Why: Changing the target handle is how the mod asks the dialogue camera to point at a different actor.
	// Trigger: Called by the subsystem described by this helper's role.
	void WriteHandle(
		void* dialogueState,
		const std::ptrdiff_t offset,
		const std::uint32_t value)
	{
		PDC_TRACE_SCOPE();
		auto* field =
			reinterpret_cast<std::uint32_t*>(
				static_cast<std::byte*>(dialogueState) + offset);

		std::atomic_ref<std::uint32_t>(*field).store(
			value,
			std::memory_order_release);
	}

	// SIMPLE GUIDE
	// What: Gets the current NPC speaker's small Starfield handle number.
	// Why: The camera controller uses handles rather than C++ actor pointers for its target.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] std::uint32_t GetSpeakerHandle()
	{
		PDC_TRACE_SCOPE();
		auto* topicManager = RE::MenuTopicManager::GetSingleton();
		return topicManager ?
			topicManager->speaker.get_handle() :
			0;
	}

	// SIMPLE GUIDE
	// What: Gets the player's small Starfield handle number.
	// Why: The player handle is needed when the dialogue camera is redirected toward the player.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] std::uint32_t GetPlayerHandle()
	{
		PDC_TRACE_SCOPE();
		auto* player = RE::PlayerCharacter::GetSingleton();
		return player ? player->nativeHandle : 0;
	}

	// Version-tolerant weapon sheathe. CommonLibSF branches differ in what they
	// expose on Actor, so probe for a usable method at compile time and no-op if
	// none is present (rather than hard-coding an RVA that could crash on a
	// mismatched build). Returns true if a sheathe call was actually issued.
	// SIMPLE GUIDE
	// What: Tries the safe weapon-sheathe function that this CommonLibSF build exposes for an actor.
	// Why: Putting weapons away avoids strange body poses when dialogue changes perspective.
	// Trigger: Called by the subsystem described by this helper's role.
	template <class T>
	[[nodiscard]] bool SheatheActorWeaponImpl(T* actor)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Asks the player character to put their weapon away.
	// Why: A calm, fully posed player body is safer to show in an external dialogue camera.
	// Trigger: Called by the subsystem described by this helper's role.
	bool SheathePlayerWeapon(const char* phase)
	{
		PDC_TRACE_SCOPE();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}

		auto* actor = static_cast<RE::Actor*>(player);
		const bool issued = SheatheActorWeaponImpl(actor);

		if (issued) {
			PDC_DIAGNOSTIC_INFO("Weapon sheathe issued: {}", phase);
		} else {
			REX::WARN(
				"Weapon sheathe unavailable in this CommonLibSF branch: {}",
				phase);
		}

		return issued;
	}


	// SIMPLE GUIDE
	// What: Gets the top 3D scene object for an actor or other world reference.
	// Why: Visibility, head lookup, and camera framing all start from this root object.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] RE::NiAVObject* GetReferenceRoot3D(
		RE::TESObjectREFR* reference)
	{
		PDC_TRACE_SCOPE();
		if (!reference) {
			return nullptr;
		}

		auto loadedData = reference->loadedData.LockRead();
		auto* data = *loadedData;

		return data ? data->data3D.get() : nullptr;
	}

	// SIMPLE GUIDE
	// What: Gets the player's current top 3D scene object.
	// Why: Starfield can replace player 3D during perspective changes, so callers should ask for the live root.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] RE::NiAVObject* GetPlayerRoot3D()
	{
		PDC_TRACE_SCOPE();
		return GetReferenceRoot3D(
			RE::PlayerCharacter::GetSingleton());
	}

	// SIMPLE GUIDE
	// What: Finds and remembers the current dialogue NPC reference.
	// Why: The remembered speaker lets cleanup still find the same actor during camera transitions.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] RE::TESObjectREFR* RememberDialogueSpeakerReference()
	{
		PDC_TRACE_SCOPE();
		auto* speaker = GetSpeakerReference();
		if (speaker) {
			g_lastDialogueSpeakerReference.store(
				reinterpret_cast<std::uintptr_t>(speaker),
				std::memory_order_release);
		}
		return speaker;
	}

	// SIMPLE GUIDE
	// What: Finds and remembers the current dialogue NPC's live 3D root.
	// Why: Camera and visibility code needs the newest scene object rather than an old replaced one.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] RE::NiAVObject* RememberDialogueSpeakerRoot()
	{
		PDC_TRACE_SCOPE();
		auto* root =
			GetReferenceRoot3D(RememberDialogueSpeakerReference());
		if (root) {
			g_lastDialogueSpeakerRoot.store(
				reinterpret_cast<std::uintptr_t>(root),
				std::memory_order_release);
		}
		return root;
	}

	// SIMPLE GUIDE
	// What: Forgets the cached player head/neck node and its position.
	// Why: A new player 3D root needs a fresh head search instead of using an old pointer.
	// Trigger: Called by the subsystem described by this helper's role.
	void ResetPlayerHeadAnchorCache()
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Finds a good head or neck point on the player's skeleton and caches it for that root.
	// Why: The camera should frame the player's face, not the middle of their body.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] PlayerHeadAnchor ResolvePlayerHeadAnchor(
		RE::NiAVObject* root)
	{
		PDC_TRACE_SCOPE();
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
	// SIMPLE GUIDE
	// What: Searches one actor root for a head/neck node without using the player's cache.
	// Why: NPC roots are different objects, so they need a fresh little search.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] RE::NiAVObject* FindHeadNodeUncached(
		RE::NiAVObject* root)
	{
		PDC_TRACE_SCOPE();
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
		// SIMPLE GUIDE
		// What: Temporarily moves the player's big camera-framing center to the head.
		// Why: Starfield then builds the dialogue shot around the face instead of the body's overall center.
		// Trigger: Called by the subsystem described by this helper's role.
		explicit ScopedPlayerFramingOverride([[maybe_unused]] const char* phase)
		{
			PDC_TRACE_SCOPE();
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
				PDC_DIAGNOSTIC_INFO(
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

		// SIMPLE GUIDE
		// What: Puts the player's original framing center back when the temporary override ends.
		// Why: The special face framing must not leak into normal gameplay or later camera work.
		// Trigger: Called by the subsystem described by this helper's role.
		~ScopedPlayerFramingOverride()
		{
			PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Reads the visibility flag bits from a 3D scene object.
	// Why: The mod needs to know whether Starfield currently considers that object hidden.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] std::uint64_t ReadPlayerRootFlags(
		RE::NiAVObject* root)
	{
		PDC_TRACE_SCOPE();
		if (!root) {
			return 0;
		}

		return std::atomic_ref<std::uint64_t>(root->flags).load(
			std::memory_order_acquire);
	}

	// SIMPLE GUIDE
	// What: Checks with Windows that a memory address exists and is safe to read.
	// Why: Touching bad memory can crash the game, so raw engine pointers get a safety check first.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsReadableAddress(
		const void* address,
		const std::size_t size)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Checks with Windows that an address points to executable code.
	// Why: The mod only calls engine function pointers after confirming they really look like code.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsExecutableAddress(const void* address)
	{
		PDC_TRACE_SCOPE();
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


	// SIMPLE GUIDE
	// What: Reads the Windows high-resolution timer only when performance logging is enabled.
	// Why: Performance measurement should be practically dormant when iPerformanceLogging=0.
	// Trigger: Called at the beginning of existing timed scopes.
	[[nodiscard]] std::uint64_t PerfNowTicks()
	{
		PDC_TRACE_SCOPE();
		if (!g_performanceLogging) {
			return 0;
		}

		LARGE_INTEGER value{};
		QueryPerformanceCounter(&value);
		return static_cast<std::uint64_t>(value.QuadPart);
	}

	// SIMPLE GUIDE
	// What: Converts a high-resolution timer difference into microseconds.
	// Why: Microseconds make hot hook/camera work easy to compare.
	// Trigger: Called when a timed performance scope finishes.
	[[nodiscard]] std::uint64_t PerfElapsedUs(const std::uint64_t startedAt)
	{
		PDC_TRACE_SCOPE();
		if (!g_performanceLogging || startedAt == 0) {
			return 0;
		}

		static const std::uint64_t frequency = []() {
			LARGE_INTEGER value{};
			QueryPerformanceFrequency(&value);
			return static_cast<std::uint64_t>(
				value.QuadPart > 0 ? value.QuadPart : 1);
		}();

		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);

		const auto current =
			static_cast<std::uint64_t>(now.QuadPart);
		const auto elapsed =
			current >= startedAt ? current - startedAt : 0;

		return (elapsed * 1000000ULL) / frequency;
	}

	// SIMPLE GUIDE
	// What: Updates a stored performance maximum when a bigger value appears.
	// Why: Diagnostic counters can remember the slowest observed operation.
	// Trigger: Called by the subsystem described by this helper's role.
	void PerfUpdateMax(
		std::atomic_uint64_t& destination,
		const std::uint64_t value)
	{
		PDC_TRACE_SCOPE();
		auto current = destination.load(std::memory_order_relaxed);
		while (value > current &&
			!destination.compare_exchange_weak(
				current,
				value,
				std::memory_order_relaxed,
				std::memory_order_relaxed)) {
		}
	}

	// SIMPLE GUIDE
	// What: Adds one measured call to a counter: calls, total time, and slowest time.
	// Why: Aggregate counters give useful performance information without one timing log per hot call.
	// Trigger: Called automatically by existing PerfScope cleanup objects.
	void PerfRecord(PerfCounter& counter, const std::uint64_t startedAt)
	{
		PDC_TRACE_SCOPE();
		if (!g_performanceLogging || startedAt == 0) {
			return;
		}

		const auto durationUs = PerfElapsedUs(startedAt);
		counter.calls.fetch_add(1, std::memory_order_relaxed);
		counter.totalUs.fetch_add(durationUs, std::memory_order_relaxed);
		PerfUpdateMax(counter.maxUs, durationUs);
	}

	// SIMPLE GUIDE
	// What: Calculates an average time from a performance counter.
	// Why: It is a small diagnostic helper for understanding how expensive repeated work was.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] double PerfAverageUs(const PerfCounter& counter)
	{
		PDC_TRACE_SCOPE();
		const auto calls = counter.calls.load(std::memory_order_relaxed);
		return calls == 0 ?
			0.0 :
			static_cast<double>(
				counter.totalUs.load(std::memory_order_relaxed)) /
				static_cast<double>(calls);
	}

	// SIMPLE GUIDE
	// What: Sets one performance counter back to zero.
	// Why: A new dialogue should start with fresh measurements instead of old ones.
	// Trigger: Called by the subsystem described by this helper's role.
	void PerfResetCounter(PerfCounter& counter)
	{
		PDC_TRACE_SCOPE();
		counter.calls.store(0, std::memory_order_relaxed);
		counter.totalUs.store(0, std::memory_order_relaxed);
		counter.maxUs.store(0, std::memory_order_relaxed);
	}

	// SIMPLE GUIDE
	// What: Notices when Starfield replaces the player's 3D root object.
	// Why: Root replacements explain many perspective and visibility problems during debugging.
	// Trigger: Called by the subsystem described by this helper's role.
	void PerfObservePlayerRoot()
	{
		PDC_TRACE_SCOPE();
		auto* root = GetPlayerRoot3D();
		const auto address = reinterpret_cast<std::uintptr_t>(root);
		const auto previous =
			g_perfLastPlayerRoot.exchange(address, std::memory_order_relaxed);

		if (previous != 0 && address != 0 && previous != address) {
			g_perfPlayerRootChanges.fetch_add(1, std::memory_order_relaxed);
			PDC_PERF_INFO(
				"[PERF] player root changed {} -> {}",
				reinterpret_cast<void*>(previous),
				static_cast<void*>(root));
		}
	}

	// SIMPLE GUIDE
	// What: Notices when Starfield replaces the NPC speaker's 3D root object.
	// Why: The mod must never assume an actor keeps the same scene object forever.
	// Trigger: Called by the subsystem described by this helper's role.
	void PerfObserveSpeakerRoot(RE::NiAVObject* root)
	{
		PDC_TRACE_SCOPE();
		const auto address = reinterpret_cast<std::uintptr_t>(root);
		const auto previous =
			g_perfLastSpeakerRoot.exchange(address, std::memory_order_relaxed);

		if (previous != 0 && address != 0 && previous != address) {
			g_perfSpeakerRootChanges.fetch_add(1, std::memory_order_relaxed);
			PDC_PERF_INFO(
				"[PERF] speaker root changed {} -> {}",
				reinterpret_cast<void*>(previous),
				static_cast<void*>(root));
		}
	}


	// SIMPLE GUIDE
	// What: Forgets all dialogue-controller instances remembered from the previous conversation.
	// Why: Each new dialogue must choose its own primary controller without stale pointers.
	// Trigger: Called by the subsystem described by this helper's role.
	void ResetDialogueControllerDedupState()
	{
		PDC_TRACE_SCOPE();
		g_primaryDialogueController.store(0, std::memory_order_relaxed);
		g_primaryDialogueControllerFieldsValidated.store(
			false,
			std::memory_order_relaxed);
		g_seenDialogueControllerCount.store(0, std::memory_order_relaxed);
		g_secondaryControllerUpdatesPassthrough.store(0, std::memory_order_relaxed);
		g_perfPrevSecondaryControllerUpdatesPassthrough.store(
			0,
			std::memory_order_relaxed);

		for (auto& slot : g_seenDialogueControllers) {
			slot.store(0, std::memory_order_relaxed);
		}
	}

	// SIMPLE GUIDE
	// What: Adds a dialogue controller pointer to the small list of controllers already seen.
	// Why: Starfield can create more than one controller, and the mod needs to tell them apart.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] std::uint64_t RegisterDialogueController(void* controller)
	{
		PDC_TRACE_SCOPE();
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

					PDC_DIAGNOSTIC_INFO(
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

	// SIMPLE GUIDE
	// What: Decides whether a controller is an extra controller rather than the primary one.
	// Why: Extra controllers must still run Starfield's native update but should not receive the mod's target writes.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsSecondaryDialogueController(
		void* controller)
	{
		PDC_TRACE_SCOPE();
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
				PDC_DIAGNOSTIC_INFO(
					"[CTRL] Primary dialogue controller selected: {}",
					controller);
				return false;
			}

			primary = expected;
		}

		if (primary == address) {
			return false;
		}

		// Secondary controllers are excluded from PointCameraAtPlayer's capture /
		// target-override path, but their native update MUST still run. The engine
		// can use these controller instances for dialogue/camera/input/lighting
		// bookkeeping that survives beyond the current conversation.
		g_secondaryControllerUpdatesPassthrough.fetch_add(
			1,
			std::memory_order_relaxed);

		return true;
	}

	// SIMPLE GUIDE
	// What: Resets the performance counters for a fresh conversation.
	// Why: Each dialogue should have clean measurements instead of inheriting the previous one.
	// Trigger: Called on a normal dialogue-open edge.
	void ResetPerformanceDiagnosticsForDialogue()
	{
		PDC_TRACE_SCOPE();

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

		auto reset64 = [](std::atomic_uint64_t& value) {
			value.store(0, std::memory_order_relaxed);
		};

		reset64(g_perfVisibilityNoOp);
		reset64(g_perfVisibilityNativeCalls);
		reset64(g_perfVisibilityShowCalls);
		reset64(g_perfVisibilityCullCalls);
		reset64(g_perfPlayerVisibilityNativeCalls);
		reset64(g_perfSpeakerVisibilityNativeCalls);
		reset64(g_perfHeadVisibilityNativeCalls);
		reset64(g_perfObstructionChecks);
		reset64(g_perfObstructionHits);
		reset64(g_perfObstructionSpeakerCull);
		reset64(g_perfObstructionSpeakerRestore);
		reset64(g_perfResolverSuppressed);
		reset64(g_perfControllerPlayerOverrides);
		reset64(g_perfControllerFieldValidationAttempts);
		reset64(g_perfVisibilityVfuncCacheHits);
		reset64(g_perfVisibilityVfuncCacheMisses);
		reset64(g_perfVisibilityRootValidationCalls);
		reset64(g_secondaryControllerUpdatesPassthrough);
		reset64(g_perfCameraRebuildCalls);
		reset64(g_perfCameraRebuildTotalUs);
		reset64(g_perfCameraRebuildMaxUs);
		reset64(g_perfViewTransitions);
		reset64(g_perfForceThirdPersonCalls);
		reset64(g_perfSetDialogueCameraStateCalls);
		reset64(g_perfExitCleanupUpdates);
		reset64(g_perfExitPreRenderRepairs);
		reset64(g_perfPlayerRootChanges);
		reset64(g_perfSpeakerRootChanges);
		reset64(g_perfViewTransitionRequests);
		reset64(g_perfViewTransitionCoalesced);
		reset64(g_perfViewTransitionBackoffSkipped);
		reset64(g_perfViewTransitionNoStateAborts);

		g_perfLastPlayerRoot.store(0, std::memory_order_relaxed);
		g_perfLastSpeakerRoot.store(0, std::memory_order_relaxed);
		g_perfLastSnapshotAt.store(GetTickCount64(), std::memory_order_relaxed);

		if (g_performanceLogging) {
			PDC_PERF_INFO(
				"[PERF] counters reset for dialogue #{}",
				g_perfDialogueIndex.load(std::memory_order_relaxed));
		}
	}

	// SIMPLE GUIDE
	// What: Writes grouped '[PERF]' lines for timings, transitions, visibility, controllers, obstruction, and current state.
	// Why: A compact snapshot is easier to diagnose than millions of individual timing records.
	// Trigger: Called periodically and at the existing dialogue-close FINAL point.
	void LogPerformanceSnapshot(const char* phase)
	{
		PDC_TRACE_SCOPE();
		if (!g_performanceLogging) {
			return;
		}

		PerfObservePlayerRoot();
		PerfObserveSpeakerRoot(
			reinterpret_cast<RE::NiAVObject*>(
				g_lastDialogueSpeakerRoot.load(std::memory_order_acquire)));

		const auto rebuildCalls =
			g_perfCameraRebuildCalls.load(std::memory_order_relaxed);
		const auto rebuildTotal =
			g_perfCameraRebuildTotalUs.load(std::memory_order_relaxed);
		const double rebuildAverage =
			rebuildCalls == 0 ?
				0.0 :
				static_cast<double>(rebuildTotal) /
					static_cast<double>(rebuildCalls);

		PDC_PERF_INFO(
			"[PERF] {} dialogue={} logic={} opening={} closing={} playerView={} "
			"bodyPrep={} actionQueued={} controllers={}",
			phase ? phase : "snapshot",
			g_dialogueOpen ? "yes" : "no",
			g_dialogueLogicEnabled.load(std::memory_order_acquire) ? "on" : "off",
			g_dialogueOpeningNative.load(std::memory_order_acquire) ? "yes" : "no",
			g_dialogueClosing.load(std::memory_order_acquire) ? "yes" : "no",
			g_playerViewRequested.load(std::memory_order_acquire) ? "player" : "NPC",
			g_bodyPreparationPending ? "yes" : "no",
			g_actionQueued.load(std::memory_order_acquire) ? "yes" : "no",
			g_seenDialogueControllerCount.load(std::memory_order_relaxed));

		PDC_PERF_INFO(
			"[PERF] calls/us tick={}/{:.1f}/{} dialogueHook={}/{:.1f}/{} "
			"controllerHook={}/{:.1f}/{} resolverHook={}/{:.1f}/{} "
			"visibility={}/{:.1f}/{} obstruction={}/{:.1f}/{}",
			g_perfTick.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfTick),
			g_perfTick.maxUs.load(std::memory_order_relaxed),
			g_perfDialogueUpdateHook.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfDialogueUpdateHook),
			g_perfDialogueUpdateHook.maxUs.load(std::memory_order_relaxed),
			g_perfControllerHook.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfControllerHook),
			g_perfControllerHook.maxUs.load(std::memory_order_relaxed),
			g_perfResolverHook.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfResolverHook),
			g_perfResolverHook.maxUs.load(std::memory_order_relaxed),
			g_perfSetRootAppCulled.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfSetRootAppCulled),
			g_perfSetRootAppCulled.maxUs.load(std::memory_order_relaxed),
			g_perfObstructionUpdate.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfObstructionUpdate),
			g_perfObstructionUpdate.maxUs.load(std::memory_order_relaxed));

		PDC_PERF_INFO(
			"[PERF] calls/us head={}/{:.1f}/{} bodyPrep={}/{:.1f}/{} "
			"automatic={}/{:.1f}/{} forceVisible={}/{:.1f}/{} "
			"rebuilds={}/{:.1f}/{}us",
			g_perfHeadVisibility.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfHeadVisibility),
			g_perfHeadVisibility.maxUs.load(std::memory_order_relaxed),
			g_perfBodyPreparation.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfBodyPreparation),
			g_perfBodyPreparation.maxUs.load(std::memory_order_relaxed),
			g_perfAutomaticCamera.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfAutomaticCamera),
			g_perfAutomaticCamera.maxUs.load(std::memory_order_relaxed),
			g_perfForceDialogueActorsVisible.calls.load(std::memory_order_relaxed),
			PerfAverageUs(g_perfForceDialogueActorsVisible),
			g_perfForceDialogueActorsVisible.maxUs.load(std::memory_order_relaxed),
			rebuildCalls,
			rebuildAverage,
			g_perfCameraRebuildMaxUs.load(std::memory_order_relaxed));

		PDC_PERF_INFO(
			"[PERF] visibility native={} noop={} show={} cull={} "
			"player={} speaker={} head={} validation={} vcacheHit={} vcacheMiss={}",
			g_perfVisibilityNativeCalls.load(std::memory_order_relaxed),
			g_perfVisibilityNoOp.load(std::memory_order_relaxed),
			g_perfVisibilityShowCalls.load(std::memory_order_relaxed),
			g_perfVisibilityCullCalls.load(std::memory_order_relaxed),
			g_perfPlayerVisibilityNativeCalls.load(std::memory_order_relaxed),
			g_perfSpeakerVisibilityNativeCalls.load(std::memory_order_relaxed),
			g_perfHeadVisibilityNativeCalls.load(std::memory_order_relaxed),
			g_perfVisibilityRootValidationCalls.load(std::memory_order_relaxed),
			g_perfVisibilityVfuncCacheHits.load(std::memory_order_relaxed),
			g_perfVisibilityVfuncCacheMisses.load(std::memory_order_relaxed));

		PDC_PERF_INFO(
			"[PERF] transitions requested={} actual={} coalesced={} backoff={} "
			"noState={} resolverSuppressed={} controllerOverrides={} "
			"controllerValidation={} secondaryNative={} force3P={} setDialogue={} "
			"obstruction checks={} hits={} cull={} restore={} rootChanges={}/{} "
			"traceLines={}",
			g_perfViewTransitionRequests.load(std::memory_order_relaxed),
			g_perfViewTransitions.load(std::memory_order_relaxed),
			g_perfViewTransitionCoalesced.load(std::memory_order_relaxed),
			g_perfViewTransitionBackoffSkipped.load(std::memory_order_relaxed),
			g_perfViewTransitionNoStateAborts.load(std::memory_order_relaxed),
			g_perfResolverSuppressed.load(std::memory_order_relaxed),
			g_perfControllerPlayerOverrides.load(std::memory_order_relaxed),
			g_perfControllerFieldValidationAttempts.load(std::memory_order_relaxed),
			g_secondaryControllerUpdatesPassthrough.load(std::memory_order_relaxed),
			g_perfForceThirdPersonCalls.load(std::memory_order_relaxed),
			g_perfSetDialogueCameraStateCalls.load(std::memory_order_relaxed),
			g_perfObstructionChecks.load(std::memory_order_relaxed),
			g_perfObstructionHits.load(std::memory_order_relaxed),
			g_perfObstructionSpeakerCull.load(std::memory_order_relaxed),
			g_perfObstructionSpeakerRestore.load(std::memory_order_relaxed),
			g_perfPlayerRootChanges.load(std::memory_order_relaxed),
			g_perfSpeakerRootChanges.load(std::memory_order_relaxed),
			g_debugTraceLinesWritten.load(std::memory_order_relaxed));
	}

	// SIMPLE GUIDE
	// What: Checks whether about one second has passed and writes a performance snapshot when due.
	// Why: This gives a steady performance timeline while keeping log volume bounded.
	// Trigger: Called from Tick in the same places as the original no-op function.
	void UpdatePerformanceSnapshot()
	{
		PDC_TRACE_SCOPE();
		if (!g_performanceLogging) {
			return;
		}

		const auto now = GetTickCount64();
		auto last =
			g_perfLastSnapshotAt.load(std::memory_order_acquire);

		if (last == 0) {
			g_perfLastSnapshotAt.store(now, std::memory_order_release);
			return;
		}

		if (now < last + kPerformanceSnapshotIntervalMs) {
			return;
		}

		if (!g_perfLastSnapshotAt.compare_exchange_strong(
				last,
				now,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			return;
		}

		LogPerformanceSnapshot("periodic");
	}


	// SIMPLE GUIDE
	// What: Finds Starfield's internal function that changes an actor's facial expression.
	// Why: Expressions need the game's own animation function rather than manually moving face bones.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] ChangeAnimFaceArchetype_t
	GetChangeAnimFaceArchetypeFunction()
	{
		PDC_TRACE_SCOPE();
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


	// SIMPLE GUIDE
	// What: Turns an expression name like 'Happy' into Starfield's matching keyword object.
	// Why: The face-animation function wants a real game keyword, not just text.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] RE::BGSKeyword* ResolveExpressionKeyword(
		const char* expression,
		std::string& editorID)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Makes the next small pseudo-random number used by Fun Mode.
	// Why: Fun Mode needs changing expression choices without depending on a heavy random-number system.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] std::uint32_t NextFunRandomValue()
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Handles Windows messages for the tiny expression notification overlay window.
	// Why: Windows sends paint, timer, and close jobs through this doorway.
	// Trigger: Called by the subsystem described by this helper's role.
	LRESULT CALLBACK ExpressionNotificationWindowProc(
		HWND window,
		UINT message,
		WPARAM wParam,
		LPARAM lParam)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Runs the notification overlay's little Windows message loop on its own thread.
	// Why: The overlay must stay responsive without blocking Starfield's game thread.
	// Trigger: Called by the subsystem described by this helper's role.
	DWORD WINAPI ExpressionNotificationThreadProc(void*)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Starts the overlay thread once if expression notifications are enabled.
	// Why: The notification window needs its helper thread before text can be shown.
	// Trigger: Called by the subsystem described by this helper's role.
	void StartExpressionNotificationThread()
	{
		PDC_TRACE_SCOPE();
		// This overlay is shared by expression messages and the global plugin
		// bypass notification. Keep the historical function/window names to avoid
		// needless churn, but the thread itself is now always available.
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

	// SIMPLE GUIDE
	// What: Sends a short piece of text to the little on-screen notification window.
	// Why: Expression and bypass messages should be visible without opening another game menu.
	// Trigger: Called by the subsystem described by this helper's role.
	void ShowOverlayNotification(const std::string_view text)
	{
		PDC_TRACE_SCOPE();
		if (text.empty()) {
			return;
		}

		StartExpressionNotificationThread();

		auto window = reinterpret_cast<HWND>(
			g_expressionNotificationWindow.load(
				std::memory_order_acquire));

		if (!window) {
			PDC_DIAGNOSTIC_INFO(
				"Overlay notification deferred because window is not ready: {}",
				std::string(text));
			return;
		}

		std::wstring message;
		message.reserve(text.size());
		for (const unsigned char character : text) {
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
				"Overlay notification could not be posted: {}",
				std::string(text));
		}
	}

	// SIMPLE GUIDE
	// What: Shows the name of the expression that was just selected.
	// Why: The player needs feedback because facial changes are hard to see while choosing them.
	// Trigger: Called by the subsystem described by this helper's role.
	void ShowExpressionNotification(const char* expression)
	{
		PDC_TRACE_SCOPE();
		if (!g_expressionNotifications || !expression) {
			return;
		}

		std::string message{ "Expression: " };
		message += expression;
		ShowOverlayNotification(message);
	}

	// SIMPLE GUIDE
	// What: Shows whether F10 put the mod into native bypass mode or turned it back on.
	// Why: The user needs a clear answer when the emergency toggle is pressed.
	// Trigger: Called by the subsystem described by this helper's role.
	void ShowPluginBypassNotification(const bool bypassed)
	{
		PDC_TRACE_SCOPE();
		ShowOverlayNotification(
			bypassed ?
				"PointCameraAtPlayer: BYPASSED" :
				"PointCameraAtPlayer: ACTIVE");
	}

	// SIMPLE GUIDE
	// What: Forgets the expression that this mod was keeping on the player.
	// Why: Old expression state should not leak into a new camera session or bypass mode.
	// Trigger: Called by the subsystem described by this helper's role.
	void ClearPlayerExpressionOverride(
		const char* phase)
	{
		PDC_TRACE_SCOPE();
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

		PDC_DIAGNOSTIC_INFO("Expression override cleared: {}", phase);
	}

	// SIMPLE GUIDE
	// What: Applies the remembered expression again if Starfield reset the player's face.
	// Why: Camera rebuilds can overwrite facial animation, so the chosen expression sometimes needs a gentle reapply.
	// Trigger: Called by the subsystem described by this helper's role.
	void ReapplyPlayerExpressionOverride([[maybe_unused]] const char* phase)
	{
		PDC_TRACE_SCOPE();
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

		PDC_DIAGNOSTIC_INFO("Expression override reapplied: {}", phase);
	}

	// SIMPLE GUIDE
	// What: Applies the expression selected in the INI, if one is configured and available.
	// Why: Automatic player shots can start with the user's preferred face.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool ApplyConfiguredDefaultExpression()
	{
		PDC_TRACE_SCOPE();
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
			PDC_DIAGNOSTIC_INFO(
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

		PDC_DIAGNOSTIC_INFO(
			"Default expression dispatched: {}, keyword={}, formID=0x{:08X}",
			expression,
			static_cast<void*>(keyword),
			keyword->GetFormID());
		return true;
	}

	// SIMPLE GUIDE
	// What: Chooses and applies a random expression for Fun Mode.
	// Why: Fun Mode deliberately changes the player's face instead of always using one expression.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool ApplyRandomFunExpression()
	{
		PDC_TRACE_SCOPE();
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

			PDC_DIAGNOSTIC_INFO(
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


	// SIMPLE GUIDE
	// What: Cancels a delayed expression job that has not happened yet.
	// Why: If the camera leaves the player early, the old expression job should not fire later.
	// Trigger: Called by the subsystem described by this helper's role.
	void CancelPendingPlayerShotExpression([[maybe_unused]] const char* phase)
	{
		PDC_TRACE_SCOPE();
		if (g_pendingPlayerShotExpression.exchange(
				false,
				std::memory_order_acq_rel)) {
			PDC_DIAGNOSTIC_INFO("Pending player-shot expression cancelled: {}", phase);
		}
		g_pendingPlayerShotExpressionAt.store(
			0,
			std::memory_order_release);
	}

	// SIMPLE GUIDE
	// What: Starts a short timer before applying the player's expression.
	// Why: Waiting a little lets the camera/body rebuild finish before facial animation is changed.
	// Trigger: Called by the subsystem described by this helper's role.
	void SchedulePlayerShotExpression()
	{
		PDC_TRACE_SCOPE();
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

		PDC_DIAGNOSTIC_INFO(
			"Player-shot expression scheduled after {}ms",
			g_expressionApplyDelayMs);
	}

	// SIMPLE GUIDE
	// What: Actually performs the delayed player expression on Starfield's task queue.
	// Why: Game objects are safer to change from the game's own queued-work path.
	// Trigger: Called by the subsystem described by this helper's role.
	void PerformPendingPlayerShotExpressionOnTaskQueue()
	{
		PDC_TRACE_SCOPE();
		struct CompletionGuard
		{
			// SIMPLE GUIDE
			// What: Clears the matching 'job already queued' flag when queued work ends.
			// Why: An early return must not block all future camera/expression jobs.
			// Trigger: Runs automatically when a queued worker finishes.
			~CompletionGuard()
			{
				PDC_TRACE_SCOPE();
				g_automaticExpressionTaskQueued.store(
					false,
					std::memory_order_release);
			}
		} completionGuard;

		if (g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
			return;
		}

		if (g_userPluginBypass.load(std::memory_order_acquire) ||
			!g_dialogueOpen ||
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

	// SIMPLE GUIDE
	// What: Checks whether the delayed expression timer is ready and queues the real work when it is.
	// Why: The main Tick can manage timing without directly changing animation at an unsafe moment.
	// Trigger: Called by the subsystem described by this helper's role.
	void UpdatePendingPlayerShotExpression()
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Moves to the next expression when the queued X-key job runs.
	// Why: Expression changes happen through the game's task queue instead of directly inside key polling.
	// Trigger: Called by the subsystem described by this helper's role.
	void PerformExpressionCycleOnTaskQueue()
	{
		PDC_TRACE_SCOPE();
		struct CompletionGuard
		{
			// SIMPLE GUIDE
			// What: Clears the matching 'job already queued' flag when queued work ends.
			// Why: An early return must not block all future camera/expression jobs.
			// Trigger: Runs automatically when a queued worker finishes.
			~CompletionGuard()
			{
				PDC_TRACE_SCOPE();
				g_expressionActionQueued.store(
					false,
					std::memory_order_release);
			}
		} completionGuard;

		if (g_userPluginBypass.load(std::memory_order_acquire) ||
			g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
			PDC_DIAGNOSTIC_INFO(
				"Expression cycle aborted: plugin/native-response bypass is active");
			return;
		}

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
				PDC_DIAGNOSTIC_INFO(
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

			PDC_DIAGNOSTIC_INFO(
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

	// SIMPLE GUIDE
	// What: Queues one expression-cycle job when the user presses the expression key.
	// Why: This prevents duplicate jobs and keeps the game-thread work in a safer place.
	// Trigger: Called by the subsystem described by this helper's role.
	void QueueExpressionCycle()
	{
		PDC_TRACE_SCOPE();

		if (g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
			return;
		}

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

	// SIMPLE GUIDE
	// What: Finds the actor 3D function Starfield uses to mark a scene object hidden or visible.
	// Why: The mod needs the real engine function before it can safely change dialogue actor visibility.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] SetAppCulled_t ResolveSetAppCulledFunction(
		RE::NiAVObject* root,
		const bool trustedLive)
	{
		PDC_TRACE_SCOPE();
		if (!root) {
			return nullptr;
		}

		if (!trustedLive) {
			g_perfVisibilityRootValidationCalls.fetch_add(
				1,
				std::memory_order_relaxed);
			if (!IsReadableAddress(root, sizeof(void*))) {
				return nullptr;
			}
		}

		auto** vtable = *reinterpret_cast<void***>(root);
		if (!vtable) {
			return nullptr;
		}

		const auto vtableAddress =
			reinterpret_cast<std::uintptr_t>(vtable);

		for (const auto& entry : g_visibilityVfuncCache) {
			if (entry.vtable.load(std::memory_order_acquire) ==
				vtableAddress) {
				const auto functionAddress =
					entry.function.load(std::memory_order_acquire);
				if (functionAddress != 0) {
					g_perfVisibilityVfuncCacheHits.fetch_add(
						1,
						std::memory_order_relaxed);
					return reinterpret_cast<SetAppCulled_t>(
						functionAddress);
				}
			}
		}

		g_perfVisibilityVfuncCacheMisses.fetch_add(
			1,
			std::memory_order_relaxed);

		void* const rawFunction =
			vtable[kSetAppCulledVtableIndex];

		// Validate each distinct vtable target once. Subsequent calls for that
		// type use the fixed-size cache and avoid VirtualQuery entirely.
		if (!IsExecutableAddress(rawFunction)) {
			return nullptr;
		}

		const auto functionAddress =
			reinterpret_cast<std::uintptr_t>(rawFunction);

		for (auto& entry : g_visibilityVfuncCache) {
			std::uintptr_t expected = 0;
			if (entry.vtable.compare_exchange_strong(
					expected,
					vtableAddress,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				entry.function.store(
					functionAddress,
					std::memory_order_release);
				break;
			}

			if (expected == vtableAddress) {
				entry.function.store(
					functionAddress,
					std::memory_order_release);
				break;
			}
		}

		return reinterpret_cast<SetAppCulled_t>(functionAddress);
	}

	// SIMPLE GUIDE
	// What: Asks Starfield to hide or show one 3D root object using the engine's own visibility function.
	// Why: Camera shots sometimes need one actor hidden or restored without deleting their 3D.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool SetRootAppCulled(
		RE::NiAVObject* root,
		const bool culled,
		const char* subject,
		const char* phase,
		const bool trustedLive = false)
	{
		PDC_TRACE_SCOPE();
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			// SIMPLE GUIDE
			// What: Records the elapsed time for the surrounding performance scope.
			// Why: RAII makes timing cleanup happen even on early returns.
			// Trigger: Runs automatically when the timed function exits.
			~PerfScope() {
				PDC_TRACE_SCOPE(); PerfRecord(g_perfSetRootAppCulled, startedAt); }
		} perfScope{ perfStartedAt };

		if (!root) {
			if (!g_visibilityCallFailed.exchange(
					true,
					std::memory_order_acq_rel)) {
				REX::WARN(
					"Native visibility {} {} skipped: 3D root is null",
					phase,
					subject);
			}
			return false;
		}

		if (!trustedLive) {
			g_perfVisibilityRootValidationCalls.fetch_add(
				1,
				std::memory_order_relaxed);
			if (!IsReadableAddress(root, sizeof(void*))) {
				if (!g_visibilityCallFailed.exchange(
						true,
						std::memory_order_acq_rel)) {
					REX::WARN(
						"Native visibility {} {} skipped: 3D root is unreadable",
						phase,
						subject);
				}
				return false;
			}
		}

		const auto beforeFlags = ReadPlayerRootFlags(root);
		const bool alreadyCulled =
			(beforeFlags & kAppCulledBit) != 0;

		if (alreadyCulled == culled) {
			g_perfVisibilityNoOp.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		const auto setAppCulled =
			ResolveSetAppCulledFunction(root, true);
		if (!setAppCulled) {
			if (!g_visibilityCallFailed.exchange(
					true,
					std::memory_order_acq_rel)) {
				REX::WARN(
					"Native visibility {} {} skipped: SetAppCulled vfunc is unavailable",
					phase,
					subject);
			}
			return false;
		}

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

		return true;
	}

	// SIMPLE GUIDE
	// What: Hides or shows the player's current 3D root.
	// Why: Dialogue camera updates need the player visible for player shots and normally hidden for NPC shots.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool SetPlayerRootAppCulled(
		const bool culled,
		const char* phase)
	{
		PDC_TRACE_SCOPE();
		return SetRootAppCulled(
			GetPlayerRoot3D(),
			culled,
			"player",
			phase,
			true);
	}



	// SIMPLE GUIDE
	// What: Finds the player's head/neck part and makes sure it is not hidden.
	// Why: First-person gameplay often hides the head, but an external dialogue camera needs to render it.
	// Trigger: Called by the subsystem described by this helper's role.
	void EnsurePlayerHeadVisible(const char* phase)
	{
		PDC_TRACE_SCOPE();
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			// SIMPLE GUIDE
			// What: Records the elapsed time for the surrounding performance scope.
			// Why: RAII makes timing cleanup happen even on early returns.
			// Trigger: Runs automatically when the timed function exits.
			~PerfScope() {
				PDC_TRACE_SCOPE(); PerfRecord(g_perfHeadVisibility, startedAt); }
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
			phase,
			true);

		const auto count = g_headVisibilityLogCount.fetch_add(
			1,
			std::memory_order_acq_rel) + 1;
		if (count <= 8) {
			PDC_DIAGNOSTIC_INFO(
				"Player head visibility {}: anchor={}, object={}, culled={}",
				phase,
				anchor.name,
				static_cast<void*>(anchor.object),
				wasCulled ? "yes" : "no");
		}
	}

	// SIMPLE GUIDE
	// What: Runs the head-visibility fix only when its small timer says it is time.
	// Why: Checking every single Tick would do needless scene-graph work.
	// Trigger: Called by the subsystem described by this helper's role.
	void MaybeEnsurePlayerHeadVisible(const char* phase)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Shows the NPC again if the obstruction system previously hid them.
	// Why: A temporary hide must be undone before the NPC is needed normally again.
	// Trigger: Called by the subsystem described by this helper's role.
	void RestoreSpeakerObstruction(const char* phase)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Forces the dialogue actors back to a visible state during cleanup.
	// Why: It is a recovery helper for cases where camera transitions left an actor hidden.
	// Trigger: Called by the subsystem described by this helper's role.
	void ForceDialogueActorsVisible(
		const char* phase,
		const bool includePlayer = true)
	{
		PDC_TRACE_SCOPE();
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			// SIMPLE GUIDE
			// What: Records the elapsed time for the surrounding performance scope.
			// Why: RAII makes timing cleanup happen even on early returns.
			// Trigger: Runs automatically when the timed function exits.
			~PerfScope() {
				PDC_TRACE_SCOPE(); PerfRecord(g_perfForceDialogueActorsVisible, startedAt); }
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

	void CancelPendingBodyPreparation(const char* phase);

	// SIMPLE GUIDE
	// What: Clears all saved steps for the old pre-dialogue body-preparation experiment.
	// Why: A new conversation must not continue an unfinished body-prep sequence.
	// Trigger: Called by the subsystem described by this helper's role.
	void ResetPreDialogueBodyPrime()
	{
		PDC_TRACE_SCOPE();
		g_preDialogueBodyPrimeStage =
			PreDialogueBodyPrimeStage::kNone;
		g_preDialogueBodyPrimeArmAt = 0;
		g_preDialogueBodyPrimeDeadlineAt = 0;
		g_preDialogueBodyPrimeNextRetryAt = 0;
		g_preDialogueBodyPrimeThirdAttempts = 0;
		g_preDialogueBodyPrimeFirstAttempts = 0;
	}

	// SIMPLE GUIDE
	// What: Starts the old first-person body-preparation sequence before dialogue camera control.
	// Why: Its goal is to make Starfield build a usable outside-the-body player model.
	// Trigger: Called by the subsystem described by this helper's role.
	void BeginPreDialogueBodyPrime()
	{
		PDC_TRACE_SCOPE();
		ResetPreDialogueBodyPrime();

		if (!g_fixFirstPersonBody ||
			!g_restoreFirstPersonAfterDialogue) {
			return;
		}

		const auto now = GetTickCount64();
		g_preDialogueBodyPrimeStage =
			PreDialogueBodyPrimeStage::kWaitArm;
		g_preDialogueBodyPrimeArmAt =
			now + kPreDialogueBodyPrimeArmDelayMs;
		g_preDialogueBodyPrimeDeadlineAt =
			now + kPreDialogueBodyPrimeTimeoutMs;

		PDC_RELEASE_INFO(
			"[FP-PRIME] armed: pre-cinematic FPP -> TPP -> FPP body prime "
			"in {}ms; speaker=0x{:08X}",
			kPreDialogueBodyPrimeArmDelayMs,
			GetSpeakerHandle());
	}

	// SIMPLE GUIDE
	// What: Advances the old pre-dialogue body-preparation sequence one small step at a time.
	// Why: Perspective changes need to be watched over several moments instead of assumed to finish instantly.
	// Trigger: Called by the subsystem described by this helper's role.
	void UpdatePreDialogueBodyPrime()
	{
		PDC_TRACE_SCOPE();

		if (!g_dialogueOpen ||
			!g_dialogueOpeningNative.load(std::memory_order_acquire) ||
			!g_restoreFirstPersonAfterDialogue) {
			return;
		}

		auto stage = g_preDialogueBodyPrimeStage;
		if (stage == PreDialogueBodyPrimeStage::kNone ||
			stage == PreDialogueBodyPrimeStage::kComplete ||
			stage == PreDialogueBodyPrimeStage::kMissedNativeDialogue ||
			stage == PreDialogueBodyPrimeStage::kFailed) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		const auto now = GetTickCount64();
		if (g_preDialogueBodyPrimeDeadlineAt != 0 &&
			now >= g_preDialogueBodyPrimeDeadlineAt) {
			REX::WARN(
				"[FP-PRIME] timed out stage={} thirdAttempts={} firstAttempts={}; "
				"falling back to post-native repair if needed",
				static_cast<std::uint32_t>(stage),
				g_preDialogueBodyPrimeThirdAttempts,
				g_preDialogueBodyPrimeFirstAttempts);
			g_preDialogueBodyPrimeStage =
				PreDialogueBodyPrimeStage::kFailed;
			return;
		}

		const bool firstPerson =
			camera->QCameraEquals(RE::CameraState::kFirstPerson) ||
			camera->QCameraEquals(RE::CameraState::kIronSights);
		const bool thirdPerson =
			camera->QCameraEquals(RE::CameraState::kThirdPerson);
		const bool dialogueCamera =
			camera->QCameraEquals(RE::CameraState::kDialogue);

		switch (stage) {
		case PreDialogueBodyPrimeStage::kWaitArm:
		{
			if (now < g_preDialogueBodyPrimeArmAt) {
				return;
			}

			// If native cinematic dialogue already exists, do not fight it here.
			// The established post-native fallback remains available.
			if (dialogueCamera ||
				g_dialogueOpeningUpdateCount.load(std::memory_order_acquire) > 0) {
				g_preDialogueBodyPrimeStage =
					PreDialogueBodyPrimeStage::kMissedNativeDialogue;
				PDC_RELEASE_INFO(
					"[FP-PRIME] native DialogueCameraState arrived before pre-prime; "
					"using post-native fallback");
				return;
			}

			if (!firstPerson) {
				// Do not manufacture a transition from an unknown camera. The prime
				// is specifically for genuine first-person gameplay entry.
				return;
			}

			g_preDialogueBodyPrimeStage =
				PreDialogueBodyPrimeStage::kAcquireThirdPerson;
			g_preDialogueBodyPrimeThirdAttempts = 1;
			g_preDialogueBodyPrimeNextRetryAt =
				now + kPreDialogueBodyPrimeRetryMs;

			(void)SetPlayerRootAppCulled(
				false,
				"pre-dialogue-body-prime");
			MaybeEnsurePlayerHeadVisible("pre-dialogue-body-prime");

			g_perfForceThirdPersonCalls.fetch_add(
				1,
				std::memory_order_relaxed);
			camera->ForceThirdPerson();

			PDC_RELEASE_INFO(
				"[FP-PRIME] genuine FirstPerson confirmed; requesting ThirdPerson "
				"before cinematic dialogue starts");
			return;
		}

		case PreDialogueBodyPrimeStage::kAcquireThirdPerson:
		{
			if (dialogueCamera) {
				g_preDialogueBodyPrimeStage =
					PreDialogueBodyPrimeStage::kMissedNativeDialogue;
				PDC_RELEASE_INFO(
					"[FP-PRIME] DialogueCameraState arrived before ThirdPerson "
					"confirmation; using post-native fallback");
				return;
			}

			if (thirdPerson) {
				// This is the important difference from the compact pulse: a real
				// ThirdPerson state has now existed across a Tick boundary. Restore
				// the user's FPP immediately; no artificial hold is needed.
				g_preDialogueBodyPrimeStage =
					PreDialogueBodyPrimeStage::kAcquireFirstPerson;
				g_preDialogueBodyPrimeFirstAttempts = 1;
				g_preDialogueBodyPrimeNextRetryAt =
					now + kPreDialogueBodyPrimeRetryMs;

				camera->ForceFirstPerson();

				PDC_RELEASE_INFO(
					"[FP-PRIME] genuine ThirdPerson confirmed; returning to "
					"FirstPerson immediately");
				return;
			}

			if (now >= g_preDialogueBodyPrimeNextRetryAt &&
				g_preDialogueBodyPrimeThirdAttempts < 4) {
				++g_preDialogueBodyPrimeThirdAttempts;
				g_perfForceThirdPersonCalls.fetch_add(
					1,
					std::memory_order_relaxed);
				camera->ForceThirdPerson();
				g_preDialogueBodyPrimeNextRetryAt =
					now + kPreDialogueBodyPrimeRetryMs;
			}
			return;
		}

		case PreDialogueBodyPrimeStage::kAcquireFirstPerson:
		{
			if (dialogueCamera) {
				// We already reached genuine TPP, so the external body should have
				// been built. Mark it prepared even if native dialogue took over
				// before our FPP restoration became current.
				g_forcedThirdPersonForDialogue = true;
				g_preDialogueBodyPrimeStage =
					PreDialogueBodyPrimeStage::kComplete;
				PDC_RELEASE_INFO(
					"[FP-PRIME] native dialogue took over after genuine TPP; "
					"body considered primed");
				return;
			}

			if (firstPerson) {
				g_forcedThirdPersonForDialogue = true;
				g_preDialogueBodyPrimeStage =
					PreDialogueBodyPrimeStage::kComplete;

				PDC_RELEASE_INFO(
					"[FP-PRIME] complete: genuine FPP -> TPP -> FPP confirmed; "
					"camera now left fully native until DialogueCameraState appears");
				return;
			}

			if (now >= g_preDialogueBodyPrimeNextRetryAt &&
				g_preDialogueBodyPrimeFirstAttempts < 4) {
				++g_preDialogueBodyPrimeFirstAttempts;
				camera->ForceFirstPerson();
				g_preDialogueBodyPrimeNextRetryAt =
					now + kPreDialogueBodyPrimeRetryMs;
			}
			return;
		}

		default:
			return;
		}
	}

	// SIMPLE GUIDE
	// What: Clears the temporary camera/body state used while dialogue is opening.
	// Why: Opening work must start clean for every new conversation.
	// Trigger: Called by the subsystem described by this helper's role.
	void ResetDialogueOpeningBodyPulseState()
	{
		PDC_TRACE_SCOPE();
		g_dialogueOpeningBodyPulseIssued.store(
			false,
			std::memory_order_release);
		g_dialogueOpeningBodyPulseRestoreRequested.store(
			false,
			std::memory_order_release);
		g_dialogueOpeningBodyPulseIssuedAt.store(
			0,
			std::memory_order_release);
		g_dialogueOpeningBodyPulseRestoreAt.store(
			0,
			std::memory_order_release);
		g_dialogueOpeningBodyPulseBaselineUpdates.store(
			0,
			std::memory_order_release);
		g_dialogueOpeningBodyPulseNextRestoreAt.store(
			0,
			std::memory_order_release);
		g_dialogueOpeningBodyPulseRestoreAttempts.store(
			0,
			std::memory_order_release);
	}

	// SIMPLE GUIDE
	// What: Stops the unfinished dialogue-opening handshake and clears its temporary state.
	// Why: The menu may close or native camera setup may fail before the mod attaches.
	// Trigger: Called by the subsystem described by this helper's role.
	void CancelDialogueOpening(const char* phase)
	{
		PDC_TRACE_SCOPE();
		const bool wasOpening =
			g_dialogueOpeningNative.exchange(false, std::memory_order_acq_rel);
		const auto updates =
			g_dialogueOpeningUpdateCount.exchange(0, std::memory_order_acq_rel);

		g_dialogueOpeningStartedAt.store(0, std::memory_order_release);
		g_dialogueOpeningLastUpdateAt.store(0, std::memory_order_release);
		g_dialogueOpeningObservedState.store(0, std::memory_order_release);
		g_dialogueOpeningFailedPassive.store(false, std::memory_order_release);
		ResetDialogueOpeningBodyPulseState();
		ResetPreDialogueBodyPrime();

		if (wasOpening) {
			PDC_RELEASE_INFO(
				"[LOGIC] native-opening cancelled phase={} updates={}",
				phase,
				updates);
		}
	}

	// SIMPLE GUIDE
	// What: Turns off this mod's active dialogue-camera ownership and clears its temporary state.
	// Why: Starfield should get its normal camera back during close, bypass, or special situations.
	// Trigger: Called by the subsystem described by this helper's role.
	void DisableDialogueLogic(const char* phase)
	{
		PDC_TRACE_SCOPE();

		const bool wasEnabled =
			g_dialogueLogicEnabled.exchange(false, std::memory_order_acq_rel);
		g_dialogueActiveForHooks.store(false, std::memory_order_release);

		g_actionQueued.store(false, std::memory_order_release);
		g_playerCameraLocked.store(false, std::memory_order_release);
		g_playerShotEstablished.store(false, std::memory_order_release);
		g_expressionActionQueued.store(false, std::memory_order_release);
		g_automaticExpressionTaskQueued.store(false, std::memory_order_release);

		CancelPendingBodyPreparation(phase);
		CancelPendingPlayerShotExpression(phase);
		ClearPlayerExpressionOverride(phase);
		ResetSpeakerHeadAnchorLock();

		// Undo only things the mod explicitly owns. Never drive the camera lifecycle.
		RestoreSpeakerObstruction(phase);
		if (wasEnabled) {
			(void)SetPlayerRootAppCulled(false, "logic-disable");
			MaybeEnsurePlayerHeadVisible("logic-disable");
		}

		g_playerViewRequested.store(false, std::memory_order_release);
		g_capturedController.store(0, std::memory_order_release);
		g_capturedControllerTarget.store(0, std::memory_order_release);
		ResetDialogueControllerDedupState();
		ResetAutomaticState();

		g_viewTransitionRetryAfter.store(0, std::memory_order_relaxed);
		g_viewTransitionRetryWarningArmed.store(false, std::memory_order_relaxed);
		g_nextObstructionCheckAt.store(0, std::memory_order_release);
		g_nextHeadVisibilityCheckAt.store(0, std::memory_order_release);

		// Old transition repair machinery is dormant in this architecture.
		g_dialogueClosing.store(false, std::memory_order_release);
		g_dialogueClosingKeepPlayerVisible.store(false, std::memory_order_release);
		g_dialogueClosingStartedAt.store(0, std::memory_order_release);
		g_dialogueClosingCameraLeftAt.store(0, std::memory_order_release);
		g_dialogueClosingUpdateCount.store(0, std::memory_order_relaxed);
		g_exitVisibilityCleanupUntil.store(0, std::memory_order_release);
		g_exitVisibilityLastLateUpdateAt.store(0, std::memory_order_release);
		g_exitVisibilityStartedAt.store(0, std::memory_order_release);
		g_exitVisibilityLastRepairAt.store(0, std::memory_order_release);
		g_exitVisibilityRepairStage.store(0, std::memory_order_release);
		g_exitVisibilityPulsePending.store(false, std::memory_order_release);
		g_exitVisibilityRepairCount.store(0, std::memory_order_relaxed);
		g_exitVisibilityPulseCount.store(0, std::memory_order_relaxed);
		g_exitPreRenderVisibilityRepaired.store(false, std::memory_order_release);

		if (wasEnabled) {
			PDC_RELEASE_INFO(
				"[LOGIC] disabled phase={}; native Starfield owns dialogue transition",
				phase);
		}
	}

	// SIMPLE GUIDE
	// What: Turns on this mod's dialogue-camera logic after native dialogue is ready.
	// Why: Waiting for Starfield first avoids fighting the engine while it is still building the conversation.
	// Trigger: Called by the subsystem described by this helper's role.
	void EnableDialogueLogic(const char* reason)
	{
		PDC_TRACE_SCOPE();
		if (g_dialogueLogicEnabled.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		const auto now = GetTickCount64();
		const auto startedAt =
			g_dialogueOpeningStartedAt.load(std::memory_order_acquire);
		const auto updates =
			g_dialogueOpeningUpdateCount.load(std::memory_order_acquire);
		auto* dialogueState =
			reinterpret_cast<void*>(
				g_dialogueOpeningObservedState.load(std::memory_order_acquire));

		g_dialogueOpeningNative.store(false, std::memory_order_release);
		g_dialogueOpeningFailedPassive.store(false, std::memory_order_release);
		g_dialogueActiveForHooks.store(true, std::memory_order_release);

		// Adopt Starfield's already-established NPC opening shot as-is.
		g_playerViewRequested.store(false, std::memory_order_release);
		g_playerCameraLocked.store(false, std::memory_order_release);
		g_playerShotEstablished.store(false, std::memory_order_release);
		g_actionQueued.store(false, std::memory_order_release);
		g_viewTransitionRetryAfter.store(0, std::memory_order_release);
		g_viewTransitionRetryWarningArmed.store(false, std::memory_order_release);
		ResetSpeakerHeadAnchorLock();
		ResetAutomaticState();

		if (g_forceSheatheOnDialogue) {
			(void)SheathePlayerWeapon("logic-enable");
		}

		(void)RememberDialogueSpeakerReference();
		(void)RememberDialogueSpeakerRoot();

		g_autoIgnoreUntil = now + g_autoStartupGraceMs;

		PDC_RELEASE_INFO(
			"[LOGIC] enabled reason={} duration={}ms updates={} state={} "
			"speaker=0x{:08X}; attached to native dialogue",
			reason,
			startedAt != 0 && now >= startedAt ? now - startedAt : 0,
			updates,
			dialogueState,
			GetSpeakerHandle());

		g_dialogueOpeningStartedAt.store(0, std::memory_order_release);
		g_dialogueOpeningLastUpdateAt.store(0, std::memory_order_release);
		g_dialogueOpeningUpdateCount.store(0, std::memory_order_relaxed);
		g_dialogueOpeningObservedState.store(0, std::memory_order_release);
		ResetDialogueOpeningBodyPulseState();
	}

	// SIMPLE GUIDE
	// What: Watches native dialogue opening and decides when it is safe for the mod to attach.
	// Why: Different conversations create their camera state at different times, so attachment must be observed rather than guessed.
	// Trigger: Called by the subsystem described by this helper's role.
	void UpdateDialogueOpening()
	{
		PDC_TRACE_SCOPE();
		if (!g_dialogueOpeningNative.load(std::memory_order_acquire) ||
			g_dialogueLogicEnabled.load(std::memory_order_acquire)) {
			return;
		}

		if (!g_dialogueOpen || !IsDialogueMenuOpen()) {
			CancelDialogueOpening("menu-closed");
			return;
		}

		const auto updates =
			g_dialogueOpeningUpdateCount.load(std::memory_order_acquire);
		const auto observedState =
			g_dialogueOpeningObservedState.load(std::memory_order_acquire);
		if (updates < kDialogueOpeningStableUpdates || observedState == 0) {
			return;
		}

		const auto now = GetTickCount64();
		const auto startedAt =
			g_dialogueOpeningStartedAt.load(std::memory_order_acquire);
		const auto elapsed =
			startedAt != 0 && now >= startedAt ? now - startedAt : 0;

		if (elapsed < kDialogueLogicNativeMinMs) {
			return;
		}

		// Do not gate camera ownership on Actor::IsTalking(). Different dialogue
		// scenes report speaker/talking state at wildly different times; the latest
		// test ranged from ~1.6s to ~7.8s. Three real native dialogue-camera updates
		// are the reliable lifecycle signal.
		const bool needsOpeningBodyRepair =
			g_fixFirstPersonBody &&
			g_restoreFirstPersonAfterDialogue &&
			!g_forcedThirdPersonForDialogue;

		if (!needsOpeningBodyRepair) {
			EnableDialogueLogic("stable-native-dialogue");
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		const bool pulseIssued =
			g_dialogueOpeningBodyPulseIssued.load(
				std::memory_order_acquire);

		if (!pulseIssued) {
			// Do not steal the camera until Starfield has a genuine live dialogue
			// state. This is still the native opening phase: all three hooks are
			// strict pass-through and the mod owns no target/culling decisions.
			if (!camera->QCameraEquals(RE::CameraState::kDialogue)) {
				return;
			}

			g_dialogueOpeningBodyPulseIssued.store(
				true,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseRestoreRequested.store(
				false,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseIssuedAt.store(
				now,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseRestoreAt.store(
				0,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseBaselineUpdates.store(
				updates,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseRestoreAttempts.store(
				0,
				std::memory_order_release);

			(void)SetPlayerRootAppCulled(
				false,
				"opening-fp-body-pulse");
			MaybeEnsurePlayerHeadVisible("opening-fp-body-pulse");

			PDC_RELEASE_INFO(
				"[FP-OPEN] background body repair begin during native opening: "
				"ForceFirstPerson -> ForceThirdPerson; nativeUpdates={}",
				updates);

			camera->ForceFirstPerson();
			g_perfForceThirdPersonCalls.fetch_add(
				1,
				std::memory_order_relaxed);
			camera->ForceThirdPerson();
			return;
		}

		const auto pulseIssuedAt =
			g_dialogueOpeningBodyPulseIssuedAt.load(
				std::memory_order_acquire);

		if (pulseIssuedAt != 0 &&
			now >= pulseIssuedAt + kOpeningBodyPulseTimeoutMs) {
			REX::CRITICAL(
				"[FP-OPEN] background body repair timed out after {}ms; "
				"mod remains passive for this dialogue",
				kOpeningBodyPulseTimeoutMs);

			// Do not attach the mod after a failed perspective repair. Native
			// dialogue remains fully functional and owns the rest of this session.
			CancelDialogueOpening("opening-fp-body-pulse-timeout");
			return;
		}

		const bool restoreRequested =
			g_dialogueOpeningBodyPulseRestoreRequested.load(
				std::memory_order_acquire);

		if (!restoreRequested) {
			if (pulseIssuedAt == 0 ||
				now < pulseIssuedAt + kOpeningBodyPulseRestoreDelayMs) {
				return;
			}

			// IMPORTANT: request DialogueCameraState unconditionally. The previous
			// build only called SetCameraState when QCameraEquals(kDialogue) was
			// false. The bad video proved currentState can already equal Dialogue
			// while the renderer is still visibly presenting gameplay ThirdPerson.
			g_perfSetDialogueCameraStateCalls.fetch_add(
				1,
				std::memory_order_relaxed);
			camera->SetCameraState(RE::CameraState::kDialogue);

			const auto baseline =
				g_dialogueOpeningUpdateCount.load(
					std::memory_order_acquire);

			g_dialogueOpeningBodyPulseRestoreRequested.store(
				true,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseRestoreAt.store(
				now,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseBaselineUpdates.store(
				baseline,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseNextRestoreAt.store(
				now + kOpeningBodyPulseRestoreRetryMs,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseRestoreAttempts.store(
				1,
				std::memory_order_release);

			PDC_RELEASE_INFO(
				"[FP-OPEN] explicit DialogueCameraState restore requested; "
				"waiting {}ms + {} fresh native dialogue update(s) before "
				"enabling mod logic",
				kOpeningBodyPulseSettleMs,
				kOpeningBodyPulseStableUpdates);
			return;
		}

		const auto restoreAt =
			g_dialogueOpeningBodyPulseRestoreAt.load(
				std::memory_order_acquire);
		const auto baselineUpdates =
			g_dialogueOpeningBodyPulseBaselineUpdates.load(
				std::memory_order_acquire);
		const auto freshUpdates =
			updates >= baselineUpdates ?
				updates - baselineUpdates :
				0;

		const bool dialogueStateCurrent =
			camera->QCameraEquals(RE::CameraState::kDialogue);
		const bool timeSettled =
			restoreAt != 0 &&
			now >= restoreAt + kOpeningBodyPulseSettleMs;
		const bool updatesSettled =
			freshUpdates >= kOpeningBodyPulseStableUpdates;

		if (dialogueStateCurrent && timeSettled && updatesSettled) {
			// The external body has been reset before the first player response,
			// and native DialogueCameraState has demonstrably run several frames
			// after our explicit restore request. Do not run the on-demand pulse.
			g_forcedThirdPersonForDialogue = true;

			PDC_RELEASE_INFO(
				"[FP-OPEN] body repair settled in native dialogue: "
				"freshUpdates={} restoreRequests={}; enabling mod logic",
				freshUpdates,
				g_dialogueOpeningBodyPulseRestoreAttempts.load(
					std::memory_order_acquire));

			EnableDialogueLogic("opening-fp-body-prepared");
			return;
		}

		// A couple of bounded explicit restore requests are useful because the
		// visible camera can lag the PlayerCamera::currentState pointer. Do not
		// spam this per-frame; at most three requests are issued.
		auto restoreAttempts =
			g_dialogueOpeningBodyPulseRestoreAttempts.load(
				std::memory_order_acquire);
		const auto nextRestoreAt =
			g_dialogueOpeningBodyPulseNextRestoreAt.load(
				std::memory_order_acquire);

		if (!dialogueStateCurrent &&
			restoreAttempts < 3 &&
			now >= nextRestoreAt) {
			g_perfSetDialogueCameraStateCalls.fetch_add(
				1,
				std::memory_order_relaxed);
			camera->SetCameraState(RE::CameraState::kDialogue);

			++restoreAttempts;
			g_dialogueOpeningBodyPulseRestoreAttempts.store(
				restoreAttempts,
				std::memory_order_release);
			g_dialogueOpeningBodyPulseNextRestoreAt.store(
				now + kOpeningBodyPulseRestoreRetryMs,
				std::memory_order_release);
		}
	}

	// Forward declaration: Closing completes before the exit-cleanup implementation below.
	void BeginExitVisibilityCleanup();

	// SIMPLE GUIDE
	// What: Stops the closing-state tracker and clears its counters.
	// Why: A new dialogue or reset must not inherit an old half-finished close.
	// Trigger: Called by the subsystem described by this helper's role.
	void CancelDialogueClosing(const char* phase)
	{
		PDC_TRACE_SCOPE();
		const bool wasClosing =
			g_dialogueClosing.exchange(false, std::memory_order_acq_rel);

		g_dialogueClosingKeepPlayerVisible.store(
			false,
			std::memory_order_release);
		g_dialogueClosingStartedAt.store(0, std::memory_order_release);
		g_dialogueClosingCameraLeftAt.store(0, std::memory_order_release);

		if (wasClosing) {
			PDC_RELEASE_INFO(
				"[CLOSING] cancelled phase={} updates={}",
				phase,
				g_dialogueClosingUpdateCount.load(std::memory_order_relaxed));
		}
		g_dialogueClosingUpdateCount.store(0, std::memory_order_relaxed);
	}

	// SIMPLE GUIDE
	// What: Starts the camera-synced closing phase after the dialogue menu closes.
	// Why: Starfield can keep DialogueCameraState alive after TAB/Escape, so menu close is too early to finish visual cleanup.
	// Trigger: Called on the normal DialogueMenu close edge.
	void BeginDialogueClosing(const bool keepPlayerVisible)
	{
		PDC_TRACE_SCOPE();
		const auto now = GetTickCount64();

		g_dialogueClosingKeepPlayerVisible.store(
			keepPlayerVisible,
			std::memory_order_release);
		g_dialogueClosingStartedAt.store(
			now,
			std::memory_order_release);
		g_dialogueClosingCameraLeftAt.store(
			0,
			std::memory_order_release);
		g_dialogueClosingUpdateCount.store(
			0,
			std::memory_order_relaxed);
		g_dialogueClosing.store(
			true,
			std::memory_order_release);

		PDC_RELEASE_INFO(
			"[CLOSING] begin keepPlayer={} cameraDialogue={} settle={}ms",
			keepPlayerVisible ? "yes" : "no",
			GetActiveDialogueState() ? "yes" : "no",
			g_dialogueExitCameraSettleMs);
	}

	// SIMPLE GUIDE
	// What: Ends camera-synced closing after the real dialogue camera boundary has been crossed.
	// Why: This is the safe point to stop the temporary closing handoff.
	// Trigger: Called by UpdateDialogueClosing after the configured settle time.
	void FinishDialogueClosing()
	{
		PDC_TRACE_SCOPE();
		const auto now = GetTickCount64();
		const auto startedAt =
			g_dialogueClosingStartedAt.load(std::memory_order_acquire);
		const auto updates =
			g_dialogueClosingUpdateCount.load(std::memory_order_relaxed);
		const bool keptPlayer =
			g_dialogueClosingKeepPlayerVisible.load(std::memory_order_acquire);

		g_dialogueClosing.store(false, std::memory_order_release);
		g_dialogueClosingKeepPlayerVisible.store(
			false,
			std::memory_order_release);
		g_dialogueClosingStartedAt.store(0, std::memory_order_release);
		g_dialogueClosingCameraLeftAt.store(0, std::memory_order_release);
		g_dialogueClosingUpdateCount.store(0, std::memory_order_relaxed);

		PDC_RELEASE_INFO(
			"[CLOSING] complete duration={}ms updates={} keptPlayer={}",
			startedAt != 0 && now >= startedAt ? now - startedAt : 0,
			updates,
			keptPlayer ? "yes" : "no");

		// Starfield has now genuinely left DialogueCameraState. Do not run the
		// older post-exit AppCulled repair pulses: those were previously tested and
		// caused regressions. The handoff ends at the actual camera boundary.
		PDC_DIAGNOSTIC_INFO(
			"[EXIT] DialogueCameraState left; visibility handoff complete");
	}

	// SIMPLE GUIDE
	// What: Watches PlayerCamera until DialogueCameraState is really gone and stays gone for the settle window.
	// Why: The player should remain protected through late native teardown updates.
	// Trigger: Called from Tick while dialogue is closed and the closing state is active.
	void UpdateDialogueClosing()
	{
		PDC_TRACE_SCOPE();
		if (!g_dialogueClosing.load(std::memory_order_acquire)) {
			return;
		}

		// A new DialogueMenu means the previous close never reached a stable
		// gameplay boundary. Do not let stale exit state bleed into it.
		if (IsDialogueMenuOpen()) {
			CancelDialogueClosing("menu-reopened");
			return;
		}

		const auto now = GetTickCount64();
		const bool dialogueCameraActive =
			GetActiveDialogueState() != nullptr;

		if (dialogueCameraActive) {
			// Teardown is still genuinely in DialogueCameraState. Any previous
			// observation of gameplay state was transient, so restart the settle.
			g_dialogueClosingCameraLeftAt.store(
				0,
				std::memory_order_release);
			return;
		}

		auto leftAt =
			g_dialogueClosingCameraLeftAt.load(
				std::memory_order_acquire);

		if (leftAt == 0) {
			g_dialogueClosingCameraLeftAt.store(
				now,
				std::memory_order_release);
			PDC_RELEASE_INFO(
				"[CLOSING] PlayerCamera left DialogueCameraState; settling {}ms",
				g_dialogueExitCameraSettleMs);

			if (g_dialogueExitCameraSettleMs == 0) {
				FinishDialogueClosing();
			}
			return;
		}

		if (now >= leftAt + g_dialogueExitCameraSettleMs) {
			FinishDialogueClosing();
		}
	}

	// SIMPLE GUIDE
	// What: Asks for one small extra actor-visibility repair during dialogue exit.
	// Why: Late native camera work can briefly change visibility after the menu closes.
	// Trigger: Called by the subsystem described by this helper's role.
	void RequestExitVisibilityPulse()
	{
		PDC_TRACE_SCOPE();
		if (g_exitVisibilityCleanupUntil.load(
				std::memory_order_acquire) == 0) {
			return;
		}

		// Late DialogueCameraState updates are useful evidence that teardown is
		// still moving, but they must NOT trigger actor visibility mutations.
		// The previous experiment converted every burst of these pulses into
		// another repair and visibly fought Starfield's gameplay transition.
		g_exitVisibilityLastLateUpdateAt.store(
			GetTickCount64(),
			std::memory_order_release);
		g_exitVisibilityPulseCount.fetch_add(
			1,
			std::memory_order_relaxed);
	}

	// SIMPLE GUIDE
	// What: Records that an exit visibility repair happened and what it touched.
	// Why: The cleanup state machine uses this bookkeeping to avoid doing the same repair blindly.
	// Trigger: Called by the subsystem described by this helper's role.
	void RecordExitVisibilityRepair(
		const char* phase,
		const std::uint64_t now,
		const bool includePlayer)
	{
		PDC_TRACE_SCOPE();
		ForceDialogueActorsVisible(phase, includePlayer);
		g_exitVisibilityLastRepairAt.store(
			now,
			std::memory_order_release);
		g_exitVisibilityRepairCount.fetch_add(
			1,
			std::memory_order_relaxed);
	}

	// SIMPLE GUIDE
	// What: Stops an unfinished exit perspective-refresh sequence.
	// Why: If conditions change, an old camera refresh must not continue into normal gameplay.
	// Trigger: Called by the subsystem described by this helper's role.
	void CancelExitPerspectiveRefresh(const char* phase)
	{
		PDC_TRACE_SCOPE();
		const auto previous =
			g_exitPerspectiveRefreshStage.exchange(
				0,
				std::memory_order_acq_rel);
		g_exitPerspectiveRefreshAt.store(0, std::memory_order_release);
		g_exitPerspectiveRefreshSpeakerReference.store(
			0,
			std::memory_order_release);

		if (previous != 0) {
			PDC_RELEASE_INFO(
				"[EXIT-REFRESH] cancelled phase={} stage={}",
				phase,
				previous);
		}
	}

	// SIMPLE GUIDE
	// What: Starts the old perspective-refresh recovery used after dialogue exit.
	// Why: Changing perspective can make Starfield rebuild player/NPC visual state when simple visibility flags are not enough.
	// Trigger: Called by the subsystem described by this helper's role.
	void BeginExitPerspectiveRefresh(RE::TESObjectREFR* speakerReference)
	{
		PDC_TRACE_SCOPE();
		if (g_exitPerspectiveRefreshStage.load(
				std::memory_order_acquire) != 0) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			REX::WARN("[EXIT-REFRESH] skipped: PlayerCamera unavailable");
			return;
		}

		const bool firstPerson =
			camera->QCameraEquals(RE::CameraState::kFirstPerson);
		const bool thirdPerson =
			camera->QCameraEquals(RE::CameraState::kThirdPerson);

		if (!firstPerson && !thirdPerson) {
			REX::WARN(
				"[EXIT-REFRESH] skipped: gameplay camera is neither first nor third person");
			return;
		}

		g_exitPerspectiveRefreshOriginalFirstPerson.store(
			firstPerson,
			std::memory_order_release);
		g_exitPerspectiveRefreshSpeakerReference.store(
			reinterpret_cast<std::uintptr_t>(speakerReference),
			std::memory_order_release);

		auto* playerRoot = GetPlayerRoot3D();
		auto* speakerRoot =
			speakerReference &&
			IsReadableAddress(speakerReference, sizeof(void*)) ?
				GetReferenceRoot3D(speakerReference) :
				nullptr;

		PDC_RELEASE_INFO(
			"[EXIT-REFRESH] begin original={} playerRoot={} speakerRef={} speakerRoot={}",
			firstPerson ? "first" : "third",
			static_cast<void*>(playerRoot),
			static_cast<void*>(speakerReference),
			static_cast<void*>(speakerRoot));

		// Do exactly what the user's manual recovery does: cross the gameplay
		// perspective boundary, allow the engine at least one rendered frame to
		// rebuild/reattach its perspective-dependent 3D, then return.
		if (firstPerson) {
			g_perfForceThirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
			camera->ForceThirdPerson();
		} else {
			camera->ForceFirstPerson();
		}

		g_exitPerspectiveRefreshStage.store(1, std::memory_order_release);
		g_exitPerspectiveRefreshAt.store(
			GetTickCount64() + kExitPerspectiveRefreshStepMs,
			std::memory_order_release);
	}

	// SIMPLE GUIDE
	// What: Advances the old exit perspective-refresh sequence through its timed camera steps.
	// Why: Perspective changes need to happen in order rather than all in one instant.
	// Trigger: Called by the subsystem described by this helper's role.
	void UpdateExitPerspectiveRefresh()
	{
		PDC_TRACE_SCOPE();
		const auto stage =
			g_exitPerspectiveRefreshStage.load(
				std::memory_order_acquire);
		if (stage == 0) {
			return;
		}

		// Never let a recovery transition spill into a newly opened dialogue.
		if (g_dialogueOpen || IsDialogueMenuOpen()) {
			CancelExitPerspectiveRefresh("new-dialogue");
			return;
		}

		const auto now = GetTickCount64();
		if (now < g_exitPerspectiveRefreshAt.load(
				std::memory_order_acquire)) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			CancelExitPerspectiveRefresh("camera-unavailable");
			return;
		}

		const bool originalFirstPerson =
			g_exitPerspectiveRefreshOriginalFirstPerson.load(
				std::memory_order_acquire);

		if (stage == 1) {
			// Return to the perspective that was active when teardown settled.
			if (originalFirstPerson) {
				camera->ForceFirstPerson();
			} else {
				g_perfForceThirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
				camera->ForceThirdPerson();
			}

			g_exitPerspectiveRefreshStage.store(2, std::memory_order_release);
			g_exitPerspectiveRefreshAt.store(
				now + kExitPerspectiveRefreshVerifyMs,
				std::memory_order_release);

			PDC_RELEASE_INFO(
				"[EXIT-REFRESH] return requested target={}",
				originalFirstPerson ? "first" : "third");
			return;
		}

		auto* speakerReference =
			reinterpret_cast<RE::TESObjectREFR*>(
				g_exitPerspectiveRefreshSpeakerReference.load(
					std::memory_order_acquire));
		const bool speakerReadable =
			speakerReference &&
			IsReadableAddress(speakerReference, sizeof(void*));

		auto* playerRoot = GetPlayerRoot3D();
		auto* speakerRoot =
			speakerReadable ?
				GetReferenceRoot3D(speakerReference) :
				nullptr;

		const auto playerFlags =
			playerRoot ? ReadPlayerRootFlags(playerRoot) : 0;
		const auto speakerFlags =
			speakerRoot ? ReadPlayerRootFlags(speakerRoot) : 0;

		PDC_RELEASE_INFO(
			"[EXIT-REFRESH] verify camera={} playerRoot={} appCulled={} "
			"speakerRef={} speakerRoot={} appCulled={}",
			camera->QCameraEquals(RE::CameraState::kFirstPerson) ?
				"first" :
				(camera->QCameraEquals(RE::CameraState::kThirdPerson) ?
					"third" : "other"),
			static_cast<void*>(playerRoot),
			(playerFlags & kAppCulledBit) != 0 ? 1 : 0,
			static_cast<void*>(speakerReference),
			static_cast<void*>(speakerRoot),
			(speakerFlags & kAppCulledBit) != 0 ? 1 : 0);

		g_exitPerspectiveRefreshStage.store(0, std::memory_order_release);
		g_exitPerspectiveRefreshAt.store(0, std::memory_order_release);
		g_exitPerspectiveRefreshSpeakerReference.store(
			0,
			std::memory_order_release);
	}

	// SIMPLE GUIDE
	// What: Finishes the old exit visibility cleanup and clears its remembered state.
	// Why: Temporary cleanup flags must not survive after gameplay camera control is stable again.
	// Trigger: Called by the subsystem described by this helper's role.
	void FinishExitVisibilityCleanup(
		const char* phase,
		const bool alreadyRepairedSpeaker)
	{
		PDC_TRACE_SCOPE();

		// The first late DialogueCameraState update is not necessarily the last.
		// Real logs show dozens of native teardown updates continuing for roughly
		// another second after DialogueMenu closes. Any one of those updates can
		// re-cull the player after our immediate/one-shot repair.
		//
		// Repair BOTH actors exactly once after teardown has gone quiet. This is
		// fundamentally different from the failed pulse-driven experiment: no
		// repeated player writes occur during teardown, so there is nothing here
		// to flicker.
		auto* playerRootBefore = GetPlayerRoot3D();
		const auto playerFlagsBefore =
			playerRootBefore ? ReadPlayerRootFlags(playerRootBefore) : 0;

		auto* rememberedReference =
			reinterpret_cast<RE::TESObjectREFR*>(
				g_lastDialogueSpeakerReference.load(
					std::memory_order_acquire));
		const bool rememberedReadable =
			rememberedReference &&
			IsReadableAddress(rememberedReference, sizeof(void*));
		auto* speakerRootBefore =
			rememberedReadable ?
				GetReferenceRoot3D(rememberedReference) :
				nullptr;
		const auto speakerFlagsBefore =
			speakerRootBefore ? ReadPlayerRootFlags(speakerRootBefore) : 0;

		RecordExitVisibilityRepair(
			phase,
			GetTickCount64(),
			true);
		EnsurePlayerHeadVisible("post-dialogue-final");

		auto* playerRootAfter = GetPlayerRoot3D();
		const auto playerFlagsAfter =
			playerRootAfter ? ReadPlayerRootFlags(playerRootAfter) : 0;
		auto* speakerRootAfter =
			rememberedReadable ?
				GetReferenceRoot3D(rememberedReference) :
				nullptr;
		const auto speakerFlagsAfter =
			speakerRootAfter ? ReadPlayerRootFlags(speakerRootAfter) : 0;

		PDC_RELEASE_INFO(
			"[EXIT-FINAL] phase={} playerRoot={} appCulled {}->{} "
			"speakerRef={} speakerRoot={} appCulled {}->{} "
			"repairs={} pulses={} scheduledSpeakerSameTick={}",
			phase,
			static_cast<void*>(playerRootAfter),
			(playerFlagsBefore & kAppCulledBit) != 0 ? 1 : 0,
			(playerFlagsAfter & kAppCulledBit) != 0 ? 1 : 0,
			static_cast<void*>(rememberedReference),
			static_cast<void*>(speakerRootAfter),
			(speakerFlagsBefore & kAppCulledBit) != 0 ? 1 : 0,
			(speakerFlagsAfter & kAppCulledBit) != 0 ? 1 : 0,
			g_exitVisibilityRepairCount.load(std::memory_order_relaxed),
			g_exitVisibilityPulseCount.load(std::memory_order_relaxed),
			alreadyRepairedSpeaker ? "yes" : "no");

		// Perspective-refresh diagnostic disabled. Camera-synced Closing now
		// starts speaker cleanup only after the real DialogueCameraState exit.
		// If a companion regression returns, refresh actor 3D directly rather
		// than visibly bouncing first/third person.

		g_exitVisibilityCleanupUntil.store(0, std::memory_order_release);
		g_exitVisibilityLastLateUpdateAt.store(0, std::memory_order_release);
		g_exitVisibilityStartedAt.store(0, std::memory_order_release);
		g_exitVisibilityLastRepairAt.store(0, std::memory_order_release);
		g_exitVisibilityPulsePending.store(false, std::memory_order_release);
		g_culledSpeakerRoot.store(0, std::memory_order_release);
		g_lastDialogueSpeakerRoot.store(0, std::memory_order_release);
		g_lastDialogueSpeakerReference.store(0, std::memory_order_release);
	}

	// SIMPLE GUIDE
	// What: Starts the old post-dialogue actor-visibility cleanup window.
	// Why: It gives Starfield a short time to finish teardown while the mod watches for hidden actors.
	// Trigger: Called by the subsystem described by this helper's role.
	void BeginExitVisibilityCleanup()
	{
		PDC_TRACE_SCOPE();
		const auto now = GetTickCount64();
		const auto cleanupWindow = std::max<std::uint64_t>(
			g_exitVisibilityCleanupMs,
			kExitVisibilityMinRepairMs);

		g_exitVisibilityCleanupUntil.store(
			now + cleanupWindow,
			std::memory_order_release);
		g_exitVisibilityLastLateUpdateAt.store(
			now,
			std::memory_order_release);
		g_exitVisibilityStartedAt.store(
			now,
			std::memory_order_release);
		g_exitVisibilityLastRepairAt.store(
			now,
			std::memory_order_release);
		g_exitVisibilityRepairStage.store(
			0,
			std::memory_order_release);
		g_exitVisibilityPulsePending.store(
			false,
			std::memory_order_release);
		g_exitVisibilityRepairCount.store(
			0,
			std::memory_order_relaxed);
		g_exitVisibilityPulseCount.store(
			0,
			std::memory_order_relaxed);

		// During active teardown this is the only scheduled cleanup repair that
		// touches the player. DialogueUpdateVisibilityHook also retains its existing
		// one-shot post-native repair. One final player+speaker repair is performed
		// only after teardown has gone quiet.
		RecordExitVisibilityRepair(
			"dialogue-close-immediate",
			now,
			true);
	}

	// SIMPLE GUIDE
	// What: Checks the old exit cleanup state and performs the next repair or finish step when needed.
	// Why: Exit behavior happens over time, so cleanup is handled like a tiny state machine.
	// Trigger: Called by the subsystem described by this helper's role.
	void UpdateExitVisibilityCleanup()
	{
		PDC_TRACE_SCOPE();
		auto cleanupUntil =
			g_exitVisibilityCleanupUntil.load(
				std::memory_order_acquire);
		if (cleanupUntil == 0) {
			return;
		}

		// Cheap polling only. Scene-graph work happens exclusively at the fixed
		// speaker-only schedule below.
		g_perfExitCleanupUpdates.fetch_add(1, std::memory_order_relaxed);

		const auto now = GetTickCount64();
		const auto startedAt =
			g_exitVisibilityStartedAt.load(
				std::memory_order_acquire);
		const auto lastLateUpdate =
			g_exitVisibilityLastLateUpdateAt.load(
				std::memory_order_acquire);

		bool scheduledRepairDue = false;
		auto stage =
			g_exitVisibilityRepairStage.load(
				std::memory_order_acquire);

		while (startedAt != 0 &&
			stage < kExitVisibilityRepairScheduleMs.size() &&
			now >= startedAt + kExitVisibilityRepairScheduleMs[stage]) {
			scheduledRepairDue = true;
			++stage;
		}
		g_exitVisibilityRepairStage.store(
			stage,
			std::memory_order_release);

		bool repairedSpeakerThisUpdate = false;
		if (scheduledRepairDue) {
			RecordExitVisibilityRepair(
				"post-dialogue-speaker-scheduled",
				now,
				false);
			repairedSpeakerThisUpdate = true;

			PDC_DIAGNOSTIC_INFO(
				"Exit speaker repair: stage={}/{}, repairs={}, pulses={}",
				stage,
				kExitVisibilityRepairScheduleMs.size(),
				g_exitVisibilityRepairCount.load(std::memory_order_relaxed),
				g_exitVisibilityPulseCount.load(std::memory_order_relaxed));
		}

		const bool minimumElapsed =
			startedAt != 0 &&
			now >= startedAt + kExitVisibilityMinRepairMs;
		const bool teardownQuiet =
			lastLateUpdate == 0 ||
			now >= lastLateUpdate + g_exitVisibilitySettleDelayMs;

		if (minimumElapsed && teardownQuiet) {
			FinishExitVisibilityCleanup(
				"post-dialogue-speaker-settled",
				repairedSpeakerThisUpdate);
			return;
		}

		if (now >= cleanupUntil) {
			// If Starfield is *still* running DialogueCameraState teardown at the
			// configured timeout, do not throw away the remembered speaker actor yet.
			// Extend only the cheap speaker safety tail, up to a hard 3-second cap.
			const auto hardDeadline =
				startedAt != 0 ?
					startedAt + kExitVisibilityHardMaxMs :
					now;
			const bool teardownStillActive =
				lastLateUpdate != 0 &&
				now < lastLateUpdate + g_exitVisibilitySettleDelayMs;

			if (teardownStillActive && now < hardDeadline) {
				const auto extendedUntil =
					std::min<std::uint64_t>(
						hardDeadline,
						now + kExitVisibilityLateTailMs);
				g_exitVisibilityCleanupUntil.store(
					extendedUntil,
					std::memory_order_release);
				return;
			}

			FinishExitVisibilityCleanup(
				"post-dialogue-speaker-timeout",
				repairedSpeakerThisUpdate);
		}
	}

	// SIMPLE GUIDE
	// What: Checks whether a test sphere sits too close to the line between the camera and player.
	// Why: This is the basic geometry question used to detect an NPC blocking the player shot.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool PointIntersectsPlayerShot(
		const RE::NiPoint3& cameraPoint,
		const RE::NiPoint3& playerPoint,
		float sightLineLengthSquared,
		const RE::NiPoint3& testPoint,
		float testRadius)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Checks whether important parts of the NPC overlap the line of sight to the player.
	// Why: If the NPC is physically between camera and player, the shot can be unusable.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool SpeakerIntersectsPlayerShot(
		RE::NiAVObject* speakerRoot)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Periodically checks whether the NPC blocks the player shot and hides/restores them when required.
	// Why: It tries to prevent the NPC's body or head from filling the player-facing camera.
	// Trigger: Called by the subsystem described by this helper's role.
	void UpdateSpeakerObstruction(const bool force = false)
	{
		PDC_TRACE_SCOPE();
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			// SIMPLE GUIDE
			// What: Records the elapsed time for the surrounding performance scope.
			// Why: RAII makes timing cleanup happen even on early returns.
			// Trigger: Runs automatically when the timed function exits.
			~PerfScope() {
				PDC_TRACE_SCOPE(); PerfRecord(g_perfObstructionUpdate, startedAt); }
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
					"obstruction-detected",
					true)) {
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

	// SIMPLE GUIDE
	// What: Checks whether the current gameplay camera is a first-person-style camera.
	// Why: The mod must remember how the player was viewing the game before dialogue.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsFirstPersonGameplayCamera(
		const RE::PlayerCamera* camera)
	{
		PDC_TRACE_SCOPE();
		return camera &&
			(camera->QCameraEquals(RE::CameraState::kFirstPerson) ||
			 camera->QCameraEquals(RE::CameraState::kIronSights));
	}

	// SIMPLE GUIDE
	// What: Checks whether the current gameplay camera is ordinary third person.
	// Why: Knowing the original perspective helps the mod restore it correctly later.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsThirdPersonGameplayCamera(
		const RE::PlayerCamera* camera)
	{
		PDC_TRACE_SCOPE();
		return camera &&
			camera->QCameraEquals(RE::CameraState::kThirdPerson);
	}

	// SIMPLE GUIDE
	// What: Watches gameplay perspective long enough to remember a stable first- or third-person choice.
	// Why: Dialogue itself briefly changes camera states, so one instant reading is not trustworthy.
	// Trigger: Called by the subsystem described by this helper's role.
	void UpdateGameplayViewHistory()
	{
		PDC_TRACE_SCOPE();
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera ||
			g_firstPersonRestorePending ||
			camera->QCameraEquals(RE::CameraState::kDialogue)) {
			return;
		}

		bool observedKnown = false;
		bool observedFirstPerson = false;

		if (camera->QCameraEquals(RE::CameraState::kFirstPerson) ||
			camera->QCameraEquals(RE::CameraState::kIronSights)) {
			observedKnown = true;
			observedFirstPerson = true;
		} else if (IsThirdPersonGameplayCamera(camera)) {
			observedKnown = true;
			observedFirstPerson = false;
		}

		if (!observedKnown) {
			return;
		}

		const auto now = GetTickCount64();

		// Establish the initial known perspective immediately. Debouncing is only
		// needed when changing an already-established gameplay perspective.
		if (!g_gameplayViewKnown) {
			g_lastGameplayFirstPerson = observedFirstPerson;
			g_gameplayViewKnown = true;
			g_gameplayViewCandidateKnown = false;
			g_gameplayViewCandidateSince = 0;
			return;
		}

		// Same as the committed state: any pending opposite-state candidate was
		// merely transient, so discard it.
		if (observedFirstPerson == g_lastGameplayFirstPerson) {
			g_gameplayViewCandidateKnown = false;
			g_gameplayViewCandidateSince = 0;
			return;
		}

		// Start/restart a candidate when the observed perspective changes.
		if (!g_gameplayViewCandidateKnown ||
			g_gameplayViewCandidateFirstPerson != observedFirstPerson) {
			g_gameplayViewCandidateKnown = true;
			g_gameplayViewCandidateFirstPerson = observedFirstPerson;
			g_gameplayViewCandidateSince = now;
			return;
		}

		// Only commit an actual gameplay perspective change after it survives the
		// short transition window. This preserves real first-person entry when
		// Starfield internally flashes ThirdPerson while constructing dialogue.
		if (now >=
			g_gameplayViewCandidateSince +
				kGameplayViewPerspectiveCommitMs) {
			g_lastGameplayFirstPerson =
				g_gameplayViewCandidateFirstPerson;
			g_gameplayViewCandidateKnown = false;
			g_gameplayViewCandidateSince = 0;
		}
	}

	// SIMPLE GUIDE
	// What: Starts the special repair used before showing a first-person player's body from an outside camera.
	// Why: Starfield's first-person body can be twisted when seen externally unless perspective/body state is rebuilt first.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool BeginThirdPersonPlayerBodyPreparation(
		const ViewReason reason)
	{
		PDC_TRACE_SCOPE();

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

		const auto now = GetTickCount64();

		g_bodyPreparationPending = true;
		g_bodyPreparationResumeAt = now;
		g_bodyPreparationDeadlineAt = now + kFirstPersonBodyRepairTimeoutMs;
		g_deferredPlayerViewReason = reason;
		g_bodyPrepTerminal = BodyPrepTerminal::kShowPlayer;

		g_firstPersonBodyRepairStage =
			FirstPersonBodyRepairStage::kAcquireFirstPerson;
		g_firstPersonBodyRepairStageStartedAt = 0;
		g_firstPersonBodyRepairNextRetryAt = now + kFirstPersonBodyRepairRetryMs;
		g_firstPersonBodyRepairDialogueRestoreAt = 0;
		g_firstPersonBodyRepairFirstAttempts = 1;
		g_firstPersonBodyRepairThirdAttempts = 0;
		g_firstPersonBodyRepairDialogueAttempts = 0;
		g_firstPersonBodyRepairDialogueUpdates.store(0, std::memory_order_release);

		(void)SetPlayerRootAppCulled(false, "fp-body-repair-start");

		PDC_RELEASE_INFO(
			"[FP-REPAIR] begin reason={}: FPP hold={}ms -> TPP hold={}ms -> "
			"Dialogue settle={}ms/{} updates",
			static_cast<std::uint32_t>(reason),
			kFirstPersonBodyRepairHoldFirstMs,
			kFirstPersonBodyRepairHoldThirdMs,
			kFirstPersonBodyRepairDialogueSettleMs,
			kFirstPersonBodyRepairDialogueStableUpdates);

		camera->ForceFirstPerson();
		return true;
	}

	// Dialogue-open path: sheathe, force third person to build a posed external
	// body during the opening beat, then settle back into the engine's natural
	// NPC-facing dialogue view. This is why the conversation can *start* face to
	// face even though we briefly showed third person to build the body.
	// SIMPLE GUIDE
	// What: Clears body-preparation state when a new dialogue begins.
	// Why: Every conversation needs a fresh body/camera preparation state.
	// Trigger: Called by the subsystem described by this helper's role.
	void BeginDialogueOpenBodyPreparation()
	{
		PDC_TRACE_SCOPE();

		ResetSpeakerHeadAnchorLock();
		const auto now = GetTickCount64();

		// Fully native opening: no sheathe, visibility writes, camera state writes,
		// rebuilds, retargeting or perspective transitions.
		g_dialogueLogicEnabled.store(false, std::memory_order_release);
		g_dialogueActiveForHooks.store(false, std::memory_order_release);
		g_dialogueOpeningStartedAt.store(now, std::memory_order_release);
		g_dialogueOpeningLastUpdateAt.store(0, std::memory_order_release);
		g_dialogueOpeningUpdateCount.store(0, std::memory_order_relaxed);
		g_dialogueOpeningObservedState.store(0, std::memory_order_release);
		g_dialogueOpeningFailedPassive.store(false, std::memory_order_release);
		g_dialogueOpeningNative.store(true, std::memory_order_release);

		g_bodyPreparationPending = false;
		g_bodyPreparationResumeAt = 0;
		g_bodyPreparationDeadlineAt = 0;
		g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kNone;
		g_firstPersonBodyRepairStageStartedAt = 0;
		g_firstPersonBodyRepairNextRetryAt = 0;
		g_firstPersonBodyRepairDialogueRestoreAt = 0;
		g_firstPersonBodyRepairFirstAttempts = 0;
		g_firstPersonBodyRepairThirdAttempts = 0;
		g_firstPersonBodyRepairDialogueAttempts = 0;
		g_firstPersonBodyRepairDialogueUpdates.store(0, std::memory_order_release);
		g_bodyPrepHasHeadSample = false;
		g_bodyPrepStableFrames = 0;
		g_bodyPrepTerminal = BodyPrepTerminal::kShowNpc;

		PDC_RELEASE_INFO(
			"[LOGIC] native opening started; mod logic OFF until native camera "
			"and opening speaker line are established");
	}

	// SIMPLE GUIDE
	// What: Stops an unfinished player-body preparation and clears all of its timers and stages.
	// Why: A camera request can disappear before preparation finishes, so stale repair steps must be abandoned safely.
	// Trigger: Called by the subsystem described by this helper's role.
	void CancelPendingBodyPreparation(const char* phase)
	{
		PDC_TRACE_SCOPE();
		if (!g_bodyPreparationPending) {
			return;
		}

		g_bodyPreparationPending = false;
		g_bodyPreparationResumeAt = 0;
		g_bodyPreparationDeadlineAt = 0;
		g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kNone;
		g_firstPersonBodyRepairStageStartedAt = 0;
		g_firstPersonBodyRepairNextRetryAt = 0;
		g_firstPersonBodyRepairDialogueRestoreAt = 0;
		g_firstPersonBodyRepairFirstAttempts = 0;
		g_firstPersonBodyRepairThirdAttempts = 0;
		g_firstPersonBodyRepairDialogueAttempts = 0;
		g_firstPersonBodyRepairDialogueUpdates.store(0, std::memory_order_release);
		g_bodyPrepHasHeadSample = false;
		g_bodyPrepStableFrames = 0;

		PDC_DIAGNOSTIC_INFO(
			"First-person body preparation cancelled: {}",
			phase);
	}

	// SIMPLE GUIDE
	// What: Runs the player-body repair state machine one step at a time until the outside body is safe to show.
	// Why: The repair needs real first-person, third-person, and dialogue-camera phases instead of one giant instant jump.
	// Trigger: Called by the subsystem described by this helper's role.
	void UpdatePendingBodyPreparation()
	{
		PDC_TRACE_SCOPE();
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			// SIMPLE GUIDE
			// What: Records the elapsed time for the surrounding performance scope.
			// Why: RAII makes timing cleanup happen even on early returns.
			// Trigger: Runs automatically when the timed function exits.
			~PerfScope() {
				PDC_TRACE_SCOPE(); PerfRecord(g_perfBodyPreparation, startedAt); }
		} perfScope{ perfStartedAt };

		if (!g_bodyPreparationPending) {
			return;
		}

		if (!g_dialogueOpen || !IsDialogueMenuOpen()) {
			CancelPendingBodyPreparation("dialogue closed");
			return;
		}

		if (g_bodyPrepTerminal == BodyPrepTerminal::kShowNpc) {
			CancelPendingBodyPreparation("legacy NPC body prep unused");
			return;
		}

		const auto now = GetTickCount64();
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		const auto reason = g_deferredPlayerViewReason;

		auto failRepair = [&](const char* phase) {
			REX::CRITICAL(
				"[FP-REPAIR] failed phase={} stage={} firstAttempts={} "
				"thirdAttempts={} dialogueAttempts={}; abandoning player shot",
				phase,
				static_cast<std::uint32_t>(g_firstPersonBodyRepairStage),
				g_firstPersonBodyRepairFirstAttempts,
				g_firstPersonBodyRepairThirdAttempts,
				g_firstPersonBodyRepairDialogueAttempts);

			g_perfSetDialogueCameraStateCalls.fetch_add(1, std::memory_order_relaxed);
			camera->SetCameraState(RE::CameraState::kDialogue);
			g_playerViewRequested.store(false, std::memory_order_release);
			CancelPendingBodyPreparation(phase);
			g_autoIgnoreUntil = now + 750;
		};

		if (now >= g_bodyPreparationDeadlineAt) {
			failRepair("hard-timeout");
			return;
		}

		const bool firstPerson =
			camera->QCameraEquals(RE::CameraState::kFirstPerson) ||
			camera->QCameraEquals(RE::CameraState::kIronSights);
		const bool thirdPerson =
			camera->QCameraEquals(RE::CameraState::kThirdPerson);
		const bool dialogueCamera =
			camera->QCameraEquals(RE::CameraState::kDialogue);

		switch (g_firstPersonBodyRepairStage) {
		case FirstPersonBodyRepairStage::kAcquireFirstPerson:
			if (firstPerson) {
				g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kHoldFirstPerson;
				g_firstPersonBodyRepairStageStartedAt = now;
				PDC_RELEASE_INFO(
					"[FP-REPAIR] FirstPerson acquired after {} request(s); holding {}ms",
					g_firstPersonBodyRepairFirstAttempts,
					kFirstPersonBodyRepairHoldFirstMs);
				return;
			}
			if (now >= g_firstPersonBodyRepairNextRetryAt) {
				++g_firstPersonBodyRepairFirstAttempts;
				camera->ForceFirstPerson();
				g_firstPersonBodyRepairNextRetryAt = now + kFirstPersonBodyRepairRetryMs;
			}
			return;

		case FirstPersonBodyRepairStage::kHoldFirstPerson:
			if (!firstPerson && now >= g_firstPersonBodyRepairNextRetryAt) {
				++g_firstPersonBodyRepairFirstAttempts;
				camera->ForceFirstPerson();
				g_firstPersonBodyRepairNextRetryAt = now + kFirstPersonBodyRepairRetryMs;
			}
			if (now < g_firstPersonBodyRepairStageStartedAt +
				kFirstPersonBodyRepairHoldFirstMs) {
				return;
			}
			g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kAcquireThirdPerson;
			g_firstPersonBodyRepairStageStartedAt = 0;
			g_firstPersonBodyRepairThirdAttempts = 1;
			g_firstPersonBodyRepairNextRetryAt = now + kFirstPersonBodyRepairRetryMs;
			g_perfForceThirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
			camera->ForceThirdPerson();
			PDC_RELEASE_INFO(
				"[FP-REPAIR] rendered FirstPerson hold complete; requesting ThirdPerson");
			return;

		case FirstPersonBodyRepairStage::kAcquireThirdPerson:
			if (thirdPerson) {
				g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kHoldThirdPerson;
				g_firstPersonBodyRepairStageStartedAt = now;
				PDC_RELEASE_INFO(
					"[FP-REPAIR] ThirdPerson acquired after {} request(s); holding {}ms",
					g_firstPersonBodyRepairThirdAttempts,
					kFirstPersonBodyRepairHoldThirdMs);
				return;
			}
			if (now >= g_firstPersonBodyRepairNextRetryAt) {
				++g_firstPersonBodyRepairThirdAttempts;
				g_perfForceThirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
				camera->ForceThirdPerson();
				g_firstPersonBodyRepairNextRetryAt = now + kFirstPersonBodyRepairRetryMs;
			}
			return;

		case FirstPersonBodyRepairStage::kHoldThirdPerson:
			if (!thirdPerson && now >= g_firstPersonBodyRepairNextRetryAt) {
				++g_firstPersonBodyRepairThirdAttempts;
				g_perfForceThirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
				camera->ForceThirdPerson();
				g_firstPersonBodyRepairNextRetryAt = now + kFirstPersonBodyRepairRetryMs;
			}
			if (now < g_firstPersonBodyRepairStageStartedAt +
				kFirstPersonBodyRepairHoldThirdMs) {
				return;
			}

			g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kRestoreDialogue;
			g_firstPersonBodyRepairDialogueAttempts = 1;
			g_firstPersonBodyRepairNextRetryAt = now + 32;
			g_firstPersonBodyRepairDialogueUpdates.store(0, std::memory_order_release);
			g_perfSetDialogueCameraStateCalls.fetch_add(1, std::memory_order_relaxed);
			camera->SetCameraState(RE::CameraState::kDialogue);
			PDC_RELEASE_INFO(
				"[FP-REPAIR] rendered ThirdPerson hold complete; restoring "
				"DialogueCameraState WITHOUT returning player body to FPP");
			return;

		case FirstPersonBodyRepairStage::kRestoreDialogue:
			if (!dialogueCamera) {
				if (now >= g_firstPersonBodyRepairNextRetryAt) {
					++g_firstPersonBodyRepairDialogueAttempts;
					g_perfSetDialogueCameraStateCalls.fetch_add(1, std::memory_order_relaxed);
					camera->SetCameraState(RE::CameraState::kDialogue);
					g_firstPersonBodyRepairNextRetryAt = now + 32;
				}
				return;
			}
			g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kSettleDialogue;
			g_firstPersonBodyRepairDialogueRestoreAt = now;
			g_firstPersonBodyRepairDialogueUpdates.store(0, std::memory_order_release);
			PDC_RELEASE_INFO(
				"[FP-REPAIR] DialogueCameraState acquired after {} request(s); "
				"waiting {}ms + {} native update(s)",
				g_firstPersonBodyRepairDialogueAttempts,
				kFirstPersonBodyRepairDialogueSettleMs,
				kFirstPersonBodyRepairDialogueStableUpdates);
			return;

		case FirstPersonBodyRepairStage::kSettleDialogue:
		{
			if (!dialogueCamera) {
				g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kRestoreDialogue;
				g_firstPersonBodyRepairNextRetryAt = now;
				return;
			}

			const auto nativeUpdates =
				g_firstPersonBodyRepairDialogueUpdates.load(std::memory_order_acquire);

			if (now < g_firstPersonBodyRepairDialogueRestoreAt +
					kFirstPersonBodyRepairDialogueSettleMs ||
				nativeUpdates < kFirstPersonBodyRepairDialogueStableUpdates) {
				return;
			}

			g_bodyPreparationPending = false;
			g_bodyPreparationResumeAt = 0;
			g_bodyPreparationDeadlineAt = 0;
			g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kNone;
			g_firstPersonBodyRepairStageStartedAt = 0;
			g_firstPersonBodyRepairNextRetryAt = 0;
			g_firstPersonBodyRepairDialogueRestoreAt = 0;
			g_firstPersonBodyRepairDialogueUpdates.store(0, std::memory_order_release);

			g_forcedThirdPersonForDialogue = true;
			ResetPlayerHeadAnchorCache();
			ReapplyPlayerExpressionOverride("rendered first-third body repair complete");

			PDC_RELEASE_INFO(
				"[FP-REPAIR] SUCCESS: FPP={}ms TPP={}ms dialogueUpdates={}; "
				"queuing deferred player shot",
				kFirstPersonBodyRepairHoldFirstMs,
				kFirstPersonBodyRepairHoldThirdMs,
				nativeUpdates);

			QueueView(true, reason);
			return;
		}

		case FirstPersonBodyRepairStage::kNone:
		default:
			failRepair("invalid-stage");
			return;
		}
	}

	// SIMPLE GUIDE
	// What: Restores the original gameplay perspective only after real dialogue teardown, and cancels the restore if the ship HUD owns the cockpit.
	// Why: Forcing FPP too early can fight DialogueCameraState or put head/armour geometry in the cockpit camera.
	// Trigger: Called from Tick while a post-dialogue perspective restore is pending.
	void UpdatePendingFirstPersonRestore()
	{
		PDC_TRACE_SCOPE();
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

		// Never force a gameplay perspective while native dialogue teardown still
		// owns DialogueCameraState. The camera-synced exit handoff above now gives
		// us a reliable boundary, so the old deadlock workaround is no longer needed.
		if (g_dialogueClosing.load(std::memory_order_acquire) ||
			camera->QCameraEquals(RE::CameraState::kDialogue)) {
			g_firstPersonRestoreAt = now + 32;
			return;
		}

		// Cockpit cameras own player body/perspective presentation. Firing the
		// dialogue FPP restore here is a direct route to armour geometry/eyeballs
		// appearing in front of the camera.
		if (IsSpaceshipHudOpen()) {
			PDC_DIAGNOSTIC_INFO(
				"[SHIP] cancelled pending post-dialogue perspective restore while piloting");
			g_firstPersonRestorePending = false;
			g_gameplayCameraRestoreDeadlineAt = 0;
			g_restoreFirstPersonAfterDialogue = false;
			g_forcedThirdPersonForDialogue = false;
			return;
		}

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

		PDC_DIAGNOSTIC_INFO(
			"Gameplay camera restored after dialogue: {}",
			restoreFirstPerson ? "first person" : "third person");
	}



	// SIMPLE GUIDE
	// What: Checks that a raw dialogue-controller field address can be safely read or written.
	// Why: The controller is reverse-engineered memory, so bad pointers must be rejected before touching them.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool IsAccessibleControllerField(
		void* controller,
		const std::ptrdiff_t offset,
		const bool requireWritable)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Reads one known integer field from a dialogue controller.
	// Why: The mod uses these fields to understand which actor the native controller is targeting.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] std::uint32_t ReadControllerField(
		void* controller,
		const std::ptrdiff_t offset)
	{
		PDC_TRACE_SCOPE();
		if (!controller) {
			return 0xFFFFFFFF;
		}

		const auto* field =
			reinterpret_cast<const std::uint32_t*>(
				static_cast<const std::byte*>(controller) + offset);

		return std::atomic_ref<const std::uint32_t>(*field).load(
			std::memory_order_acquire);
	}

	// SIMPLE GUIDE
	// What: Safely writes one known integer field in a dialogue controller.
	// Why: Target changes sometimes need to be mirrored into the native controller's own state.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool WriteControllerField(
		void* controller,
		const std::ptrdiff_t offset,
		const std::uint32_t value)
	{
		PDC_TRACE_SCOPE();
		if (!controller) {
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

	// SIMPLE GUIDE
	// What: Returns the primary dialogue-controller pointer the hook most recently captured.
	// Why: Queued camera work needs the same controller after the hook call itself has finished.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] void* GetCapturedController()
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Checks whether the remembered controller still looks like the same live dialogue controller.
	// Why: Raw engine pointers can go stale, so the mod must not blindly reuse them.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool ValidateCapturedController(
		void* controller,
		const std::uint32_t speakerHandle,
		const std::uint32_t playerHandle)
	{
		PDC_TRACE_SCOPE();
		const auto primary =
			g_primaryDialogueController.load(
				std::memory_order_acquire);
		if (!controller ||
			primary == 0 ||
			reinterpret_cast<std::uintptr_t>(controller) != primary ||
			!g_primaryDialogueControllerFieldsValidated.load(
				std::memory_order_acquire)) {
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

	// SIMPLE GUIDE
	// What: Writes the chosen player/NPC target into the captured controller when it is safe.
	// Why: Keeping the controller's target in sync helps stop Starfield from immediately recentering on the wrong actor.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool ApplyCapturedControllerTarget(
		const std::uint32_t targetHandle,
		const bool showPlayer)
	{
		PDC_TRACE_SCOPE();
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

		// Target only. Historical working-camera builds established player
		// shots with +0x9C preserved. This field also participates in native
		// dialogue/input progression, so do not rewind it here.
		const bool statePreserved = true;

		const auto afterTarget =
			ReadControllerField(
				controller,
				kControllerTargetHandle);

		const auto afterState =
			ReadControllerField(
				controller,
				kControllerState);

		PDC_DIAGNOSTIC_INFO(
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
			"preserved");

		return targetWritten && statePreserved;
	}


	// SIMPLE GUIDE
	// What: Finds Starfield's internal dialogue-camera rebuild function.
	// Why: The mod uses the game's own rebuild routine instead of trying to recreate all camera math itself.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] RebuildDialogueCamera_t GetRebuildFunction()
	{
		PDC_TRACE_SCOPE();
		const auto moduleBase =
			reinterpret_cast<std::uintptr_t>(
				GetModuleHandleW(nullptr));

		if (moduleBase == 0) {
			return nullptr;
		}

		return reinterpret_cast<RebuildDialogueCamera_t>(
			moduleBase + kRebuildDialogueCameraRva);
	}

	// SIMPLE GUIDE
	// What: Writes the player's handle into the dialogue-state target fields.
	// Why: A player-facing shot needs both native target slots to agree that the player is the target.
	// Trigger: Called by the subsystem described by this helper's role.
	void ForcePlayerHandles(void* dialogueState)
	{
		PDC_TRACE_SCOPE();
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

	// SIMPLE GUIDE
	// What: Forgets the NPC head position that was temporarily being held still.
	// Why: A new actor or camera shot must capture a fresh head position.
	// Trigger: Called by the subsystem described by this helper's role.
	void ResetSpeakerHeadAnchorLock()
	{
		PDC_TRACE_SCOPE();
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
		// SIMPLE GUIDE
		// What: Temporarily holds the NPC head's world position steady while Starfield performs a native camera update.
		// Why: This reduces unwanted recentering without skipping the engine's important controller work.
		// Trigger: Called by the subsystem described by this helper's role.
		explicit ScopedSpeakerHeadAnchorOverride(
			const bool playerMode,
			const char* phase)
		{
			PDC_TRACE_SCOPE();
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

				PDC_DIAGNOSTIC_INFO(
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
				PDC_DIAGNOSTIC_INFO(
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

		// SIMPLE GUIDE
		// What: Restores the NPC head's real animated transform after the temporary camera calculation.
		// Why: The NPC should keep animating normally once the camera math is finished.
		// Trigger: Called by the subsystem described by this helper's role.
		~ScopedSpeakerHeadAnchorOverride()
		{
			PDC_TRACE_SCOPE();
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


	// SIMPLE GUIDE
	// What: Ends the temporary "Starfield owns the selected NPC response" window.
	// Why: The response bypass must be cleared cleanly without pretending the whole dialogue opened or closed again.
	// Trigger: Called after the selected NPC response has really finished, or when another lifecycle boundary supersedes it.
	void ResetDialogueChoiceNativeWindow(
		const char* reason,
		const bool seedAutomaticPlayerCandidate)
	{
		PDC_TRACE_SCOPE();

		const bool wasActive =
			g_dialogueChoiceNativeWindow.load(std::memory_order_acquire);
		if (!wasActive) {
			return;
		}

		const auto silenceSince =
			g_dialogueChoiceSilenceSince.load(std::memory_order_acquire);

		if (seedAutomaticPlayerCandidate) {
			// Starfield owned the response and therefore NPC view is the only
			// sensible logical baseline when our own switching resumes.
			g_playerViewRequested.store(false, std::memory_order_release);
			g_playerCameraLocked.store(false, std::memory_order_release);
			g_playerShotEstablished.store(false, std::memory_order_release);

			ResetAutomaticState();

			// We already waited through the complete fragment-safe silence window.
			// Seed that timestamp so UpdateAutomaticCamera can use its normal
			// player-shot path immediately instead of waiting another 1500 ms.
			if (silenceSince != 0) {
				g_autoCandidateInitialized = true;
				g_autoCandidatePlayer = true;
				g_autoCandidateSince = silenceSince;
				g_lastSpeakerTalking = false;
				g_autoIgnoreUntil = 0;
			}
		}

		g_dialogueChoiceSawNpcSpeech.store(false, std::memory_order_release);
		g_dialogueChoiceCleanupPending.store(false, std::memory_order_release);
		g_dialogueChoiceStartedAt.store(0, std::memory_order_release);
		g_dialogueChoiceSilenceSince.store(0, std::memory_order_release);

		// Publish false last. Worker hooks that observe the native window ending
		// also observe the logical state written above.
		g_dialogueChoiceNativeWindow.store(false, std::memory_order_release);

		PDC_DIAGNOSTIC_INFO(
			"[CHOICE] native response window OFF: reason={} autoPlayerCandidate={}",
			reason ? reason : "unknown",
			seedAutomaticPlayerCandidate ? "yes" : "no");
	}

	// SIMPLE GUIDE
	// What: Restores the actual native dialogue target to the speaker before Starfield processes a selected response.
	// Why: 1.18.18 only made our hooks passive; the live DialogueCameraState could still be targeted at the player when selection processing began.
	// Trigger: Called after the response-window flag is published and before original DialogueMenu_OnDialogueSelect.
	void ReleasePlayerTargetBeforeDialogueSelection()
	{
		PDC_TRACE_SCOPE();

		const auto speakerHandle =
			GetSpeakerHandle();

		void* const dialogueState =
			GetActiveDialogueState();

		void* const controller =
			GetCapturedController();

		const auto beforeRequested =
			dialogueState ?
				ReadHandle(dialogueState,kRequestedTargetHandle) :
				0u;

		const auto beforeCurrent =
			dialogueState ?
				ReadHandle(dialogueState,kCurrentTargetHandle) :
				0u;

		const auto beforeControllerTarget =
			controller ?
				ReadControllerField(controller,kControllerTargetHandle) :
				0xFFFFFFFFu;

		const auto beforeControllerState =
			controller ?
				ReadControllerField(controller,kControllerState) :
				0xFFFFFFFFu;

		bool dialogueReleased = false;
		bool controllerReleased = false;

		if (dialogueState && speakerHandle != 0) {
			WriteHandle(
				dialogueState,
				kRequestedTargetHandle,
				speakerHandle);

			WriteHandle(
				dialogueState,
				kCurrentTargetHandle,
				speakerHandle);

			dialogueReleased =
				ReadHandle(dialogueState,kRequestedTargetHandle) ==
					speakerHandle &&
				ReadHandle(dialogueState,kCurrentTargetHandle) ==
					speakerHandle;
		}

		// Restore target identity only. Do not reset +0x9C and do not rebuild
		// at this boundary. The original callback should begin from native NPC
		// ownership without us rewinding controller progression.
		if (controller &&
			speakerHandle != 0 &&
			ValidateCapturedController(
				controller,
				speakerHandle,
				GetPlayerHandle())) {
			controllerReleased =
				WriteControllerField(
					controller,
					kControllerTargetHandle,
					speakerHandle);

			g_capturedControllerTarget.store(
				speakerHandle,
				std::memory_order_release);
		}

		RestoreSpeakerObstruction(
			"preselect-native-release");

		if (auto* speakerRoot =
				RememberDialogueSpeakerRoot()) {
			(void)SetRootAppCulled(
				speakerRoot,
				false,
				"speaker",
				"preselect-native-release",
				true);
		}

		PDC_DIAGNOSTIC_INFO(
			"[CHOICE-RELEASE] BEFORE original OnDialogueSelect: "
			"speaker=0x{:08X}; dialogue [0x{:08X},0x{:08X}]"
			"->[0x{:08X},0x{:08X}] released={}; "
			"controller +0x60 0x{:08X}->0x{:08X} released={}; "
			"+0x9C={} PRESERVED; rebuild=NO",
			speakerHandle,
			beforeRequested,
			beforeCurrent,
			dialogueState ?
				ReadHandle(dialogueState,kRequestedTargetHandle) :
				0u,
			dialogueState ?
				ReadHandle(dialogueState,kCurrentTargetHandle) :
				0u,
			dialogueReleased ? "yes" : "no",
			beforeControllerTarget,
			controller ?
				ReadControllerField(controller,kControllerTargetHandle) :
				0xFFFFFFFFu,
			controllerReleased ? "yes" : "no",
			beforeControllerState);
	}


	// SIMPLE GUIDE
	// What: Arms native pass-through at the exact moment DialogueMenu reports that a dialogue entry was selected.
	// Why: Starfield can then start the ordered NPC response without our camera/controller/resolver logic changing its progression.
	// Trigger: Called by DialogueSelectCallbackHook BEFORE the original DialogueMenu_OnDialogueSelect callback runs.
	void BeginDialogueChoiceNativeWindow()
	{
		PDC_TRACE_SCOPE();

		if (!g_dialogueOpen ||
			!IsDialogueMenuOpen() ||
			!g_dialogueLogicEnabled.load(std::memory_order_acquire) ||
			g_dialogueClosing.load(std::memory_order_acquire) ||
			g_bodyPreparationPending ||
			g_userPluginBypass.load(std::memory_order_acquire) ||
			g_shipDialogueBypass.load(std::memory_order_acquire) ||
			g_scriptedSceneBypass.load(std::memory_order_acquire)) {
			return;
		}

		// Publish pass-through FIRST. The original OnDialogueSelect callback can
		// synchronously trigger dialogue/controller/camera work, and those hooks
		// must already know to stay native.
		const bool alreadyActive =
			g_dialogueChoiceNativeWindow.exchange(
				true,
				std::memory_order_acq_rel);

		if (alreadyActive) {
			// A second callback inside the same response should not restart the
			// observed speech/silence lifetime.
			return;
		}

		g_dialogueChoiceSawNpcSpeech.store(false, std::memory_order_release);
		g_dialogueChoiceCleanupPending.store(true, std::memory_order_release);
		g_dialogueChoiceStartedAt.store(
			GetTickCount64(),
			std::memory_order_release);
		g_dialogueChoiceSilenceSince.store(0, std::memory_order_release);

		// We do not cancel an SFSE task already in the queue from this Scaleform
		// callback. PerformSetViewOnTaskQueue now checks the response-window flag
		// and will safely abort if that old task executes later.
		g_playerViewRequested.store(false, std::memory_order_release);
		g_playerCameraLocked.store(false, std::memory_order_release);
		g_playerShotEstablished.store(false, std::memory_order_release);

		// Critical difference from 1.18.18: make the ENGINE state native too,
		// not merely our hook behaviour.
		ReleasePlayerTargetBeforeDialogueSelection();

		PDC_DIAGNOSTIC_INFO(
			"[CHOICE] DialogueMenu_OnDialogueSelect fired; "
			"native response window ON and real target released to NPC "
			"before Starfield selection processing");
	}

	// SIMPLE GUIDE
	// What: Watches the selected NPC response start talking, survive fragment gaps, and finally remain silent.
	// Why: Re-enabling on the first tiny pause would recreate the ordered-fragment camera churn that caused dialogue skipping.
	// Trigger: Called from Tick while g_dialogueChoiceNativeWindow is active.
	[[nodiscard]] bool UpdateDialogueChoiceNativeWindow()
	{
		PDC_TRACE_SCOPE();

		if (!g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
			return false;
		}

		if (!g_dialogueOpen || !IsDialogueMenuOpen()) {
			ResetDialogueChoiceNativeWindow(
				"dialogue closed",
				false);
			return false;
		}

		// Stronger/longer-lived native ownership modes supersede this one-response
		// handoff. Only clear our response state; the other bypass stays active.
		if (g_userPluginBypass.load(std::memory_order_acquire) ||
			g_shipDialogueBypass.load(std::memory_order_acquire) ||
			g_scriptedSceneBypass.load(std::memory_order_acquire)) {
			ResetDialogueChoiceNativeWindow(
				"superseded by another bypass",
				false);
			return false;
		}

		if (g_dialogueChoiceCleanupPending.exchange(
				false,
				std::memory_order_acq_rel)) {
			// Undo only remaining presentation state. Target identity was already
			// restored synchronously BEFORE the original selection callback.
			// Do not rebuild here and do not reset controller progression state.
			RestoreSpeakerObstruction("dialogue-choice-native-window");
			if (auto* speakerRoot = RememberDialogueSpeakerRoot()) {
				(void)SetRootAppCulled(
					speakerRoot,
					false,
					"speaker",
					"dialogue-choice-native-window",
					true);
			}

			CancelPendingPlayerShotExpression(
				"dialogue-choice-native-window");
			ResetAutomaticState();
			g_vWasDown = false;
			g_expressionWasDown = false;

			PDC_DIAGNOSTIC_INFO(
				"[CHOICE] plugin camera/controller/resolver logic is pass-through; "
				"waiting for NPC response speech");
		}

		bool validSpeaker = false;
		const bool speakerTalking = IsSpeakerTalking(validSpeaker);
		if (!validSpeaker) {
			return true;
		}

		const auto now = GetTickCount64();

		if (speakerTalking) {
			const bool firstSpeech =
				!g_dialogueChoiceSawNpcSpeech.exchange(
					true,
					std::memory_order_acq_rel);

			const auto priorSilence =
				g_dialogueChoiceSilenceSince.exchange(
					0,
					std::memory_order_acq_rel);

			if (firstSpeech) {
				PDC_DIAGNOSTIC_INFO(
					"[CHOICE] NPC response speech started; "
					"plugin remains native-pass-through");
			} else if (priorSilence != 0) {
				PDC_DIAGNOSTIC_INFO(
					"[CHOICE] NPC voice fragment resumed after {}ms silence; "
					"response-end timer reset",
					now >= priorSilence ? now - priorSilence : 0);
			}

			return true;
		}

		// The quiet delay between selecting a response and the first NPC line is
		// not the end of the response. We require proof that NPC speech started.
		if (!g_dialogueChoiceSawNpcSpeech.load(std::memory_order_acquire)) {
			return true;
		}

		auto silenceSince =
			g_dialogueChoiceSilenceSince.load(std::memory_order_acquire);

		if (silenceSince == 0) {
			g_dialogueChoiceSilenceSince.store(
				now,
				std::memory_order_release);

			PDC_DIAGNOSTIC_INFO(
				"[CHOICE] NPC became silent after response; requiring {}ms "
				"continuous silence before plugin resumes",
				g_autoFragmentSilenceHoldMs);
			return true;
		}

		if (now < silenceSince + g_autoFragmentSilenceHoldMs) {
			return true;
		}

		PDC_DIAGNOSTIC_INFO(
			"[CHOICE] NPC response complete: {}ms continuous silence after speech",
			now - silenceSince);

		ResetDialogueChoiceNativeWindow(
			"NPC response complete",
			true);
		return false;
	}

	class DialogueSelectCallbackHook
	{
	public:
		// SIMPLE GUIDE
		// What: Hooks the exact Scaleform callback named DialogueMenu_OnDialogueSelect.
		// Why: This is the semantic "a dialogue entry was selected" event, so there is no keyboard/mouse guessing and no live menu-vtable surgery.
		// Trigger: Starfield invokes the callback when DialogueMenu dispatches a selected response.
		static void Thunk(
			RE::Scaleform::GFx::FunctionHandler* handler,
			const RE::Scaleform::GFx::FunctionHandler::Params& params)
		{
			PDC_TRACE_SCOPE();

			BeginDialogueChoiceNativeWindow();

			// Always call Starfield's original callback. The hook observes the
			// selection boundary; it never consumes or replaces the selection.
			Hook(handler, params);

			if (g_dialogueChoiceNativeWindow.load(
					std::memory_order_acquire)) {
				void* const dialogueState =
					GetActiveDialogueState();

				void* const controller =
					GetCapturedController();

				PDC_DIAGNOSTIC_INFO(
					"[CHOICE-RELEASE] AFTER original OnDialogueSelect: "
					"dialogue=[0x{:08X},0x{:08X}], "
					"controller=[target=0x{:08X},state={}], "
					"speaker=0x{:08X}, player=0x{:08X}",
					dialogueState ?
						ReadHandle(dialogueState,kRequestedTargetHandle) :
						0u,
					dialogueState ?
						ReadHandle(dialogueState,kCurrentTargetHandle) :
						0u,
					controller ?
						ReadControllerField(controller,kControllerTargetHandle) :
						0xFFFFFFFFu,
					controller ?
						ReadControllerField(controller,kControllerState) :
						0xFFFFFFFFu,
					GetSpeakerHandle(),
					GetPlayerHandle());
			}
		}

		// CommonLibSF exposes DialogueMenu_OnDialogueSelect as its own vtable.
		// Scaleform::GFx::FunctionHandler::Call is virtual slot 1.
		static inline REL::THookVFT<
			void(
				RE::Scaleform::GFx::FunctionHandler*,
				const RE::Scaleform::GFx::FunctionHandler::Params&)> Hook{
			"PDC_DialogueMenu_OnDialogueSelect",
			RE::VTABLE::DialogueMenu_OnDialogueSelect[0],
			1,
			Thunk
		};
	};


	class DialogueUpdateVisibilityHook
	{
	public:
		// SIMPLE GUIDE
		// What: Wraps DialogueCameraState::Update and coordinates opening counters, closing visibility, body-repair updates, and active actor presentation.
		// Why: The mod needs to cooperate with real native camera updates instead of replacing them.
		// Trigger: Called by the DialogueCameraState vtable hook.
		static void Thunk(void* dialogueState)
		{
			PDC_TRACE_SCOPE();
			const auto perfStartedAt = PerfNowTicks();
			struct PerfScope
			{
				std::uint64_t startedAt;
				// SIMPLE GUIDE
				// What: Records the elapsed time for the surrounding performance scope.
				// Why: RAII makes timing cleanup happen even on early returns.
				// Trigger: Runs automatically when the timed function exits.
				~PerfScope() {
					PDC_TRACE_SCOPE(); PerfRecord(g_perfDialogueUpdateHook, startedAt); }
			} perfScope{{ perfStartedAt }};

			if (g_userPluginBypass.load(std::memory_order_acquire) ||
				g_shipDialogueBypass.load(std::memory_order_acquire) ||
				g_scriptedSceneBypass.load(std::memory_order_acquire) ||
				g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
				Hook(dialogueState);
				return;
			}

			const bool dialogueMenuOpen = IsDialogueMenuOpen();

			if (!dialogueMenuOpen &&
				g_dialogueClosing.load(std::memory_order_acquire)) {
				// Menu is logically closed but native DialogueCameraState may still
				// run teardown updates. Let native update first, then keep the player
				// visible until PlayerCamera actually leaves DialogueCameraState.
				Hook(dialogueState);
				g_dialogueClosingUpdateCount.fetch_add(
					1,
					std::memory_order_relaxed);
				(void)SetPlayerRootAppCulled(false, "exit-camera-handoff");
				MaybeEnsurePlayerHeadVisible("exit-camera-handoff");
				return;
			}

			if (g_bodyPreparationPending) {
				Hook(dialogueState);
				if (g_firstPersonBodyRepairStage ==
						FirstPersonBodyRepairStage::kRestoreDialogue ||
					g_firstPersonBodyRepairStage ==
						FirstPersonBodyRepairStage::kSettleDialogue) {
					g_firstPersonBodyRepairDialogueUpdates.fetch_add(
						1,
						std::memory_order_relaxed);
				}
				return;
			}

			if (!g_dialogueLogicEnabled.load(std::memory_order_acquire) ||
				!dialogueMenuOpen) {
				Hook(dialogueState);

				if (dialogueMenuOpen &&
					g_dialogueOpeningNative.load(std::memory_order_acquire)) {
					g_dialogueOpeningObservedState.store(
						reinterpret_cast<std::uintptr_t>(dialogueState),
						std::memory_order_release);
					g_dialogueOpeningLastUpdateAt.store(
						GetTickCount64(),
						std::memory_order_release);
					g_dialogueOpeningUpdateCount.fetch_add(
						1,
						std::memory_order_relaxed);
				}
				return;
			}

			const bool dialogueActive =
				g_dialogueActiveForHooks.load(std::memory_order_acquire);
			const bool playerMode =
				dialogueActive &&
				g_playerViewRequested.load(std::memory_order_acquire) &&
				dialogueState;

			if (!dialogueActive) {
				Hook(dialogueState);
				return;
			}

			if (!playerMode) {
				(void)SetPlayerRootAppCulled(
					true,
					"NPC-update-entry");
			}

			if (playerMode && !g_stablePlayerVisibility) {
				(void)SetPlayerRootAppCulled(
					true,
					"update-entry-legacy");
			}

			Hook(dialogueState);

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
		// SIMPLE GUIDE
		// What: Wraps Starfield's dialogue-controller update, captures the primary controller, and substitutes the player target only when needed.
		// Why: Native controller work must always run because it also handles dialogue/input/lighting bookkeeping.
		// Trigger: Called by the controller-update hook.
		static void Thunk(
			void* controller,
			std::uint32_t targetHandle)
		{
			PDC_TRACE_SCOPE();
			const auto perfStartedAt = PerfNowTicks();
			struct PerfScope
			{
				std::uint64_t startedAt;
				// SIMPLE GUIDE
				// What: Records the elapsed time for the surrounding performance scope.
				// Why: RAII makes timing cleanup happen even on early returns.
				// Trigger: Runs automatically when the timed function exits.
				~PerfScope() {
					PDC_TRACE_SCOPE(); PerfRecord(g_perfControllerHook, startedAt); }
			} perfScope{{ perfStartedAt }};

			if (g_userPluginBypass.load(std::memory_order_acquire) ||
				g_shipDialogueBypass.load(std::memory_order_acquire) ||
				g_scriptedSceneBypass.load(std::memory_order_acquire) ||
				g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
				Hook(controller, targetHandle);
				return;
			}

			if (g_bodyPreparationPending ||
				!g_dialogueLogicEnabled.load(std::memory_order_acquire) ||
				!IsDialogueMenuOpen()) {
				Hook(controller, targetHandle);
				return;
			}

			if (IsSecondaryDialogueController(controller)) {
				// Do not capture or modify secondary controller instances, but NEVER
				// starve them of their native update. Previous builds returned here
				// without calling Hook(), even though the native routine also performs
				// dialogue/camera/input/lighting bookkeeping. That correlated with the
				// next conversation failing to create DialogueCameraState after travel.
				Hook(controller, targetHandle);
				return;
			}

			g_capturedController.store(
				reinterpret_cast<std::uintptr_t>(controller),
				std::memory_order_release);

			g_capturedControllerTarget.store(
				targetHandle,
				std::memory_order_release);

			const auto controllerAddress =
				reinterpret_cast<std::uintptr_t>(controller);
			const auto primaryController =
				g_primaryDialogueController.load(
					std::memory_order_acquire);
			if (primaryController == controllerAddress &&
				!g_primaryDialogueControllerFieldsValidated.load(
					std::memory_order_acquire)) {
				g_perfControllerFieldValidationAttempts.fetch_add(
					1,
					std::memory_order_relaxed);
				const bool fieldsAccessible =
					IsAccessibleControllerField(
						controller,
						kControllerTargetHandle,
						true) &&
					IsAccessibleControllerField(
						controller,
						kControllerState,
						true);
				g_primaryDialogueControllerFieldsValidated.store(
					fieldsAccessible,
					std::memory_order_release);
				if (!fieldsAccessible) {
					REX::WARN(
						"Primary dialogue controller fields failed one-time validation; "
						"player controller writes will remain disabled");
				}
			}

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

			// Never mutate scene-graph transforms or bounds from this hook. The
			// controller may run on several engine worker threads.
			Hook(controller, effectiveTarget);

			if (playerMode) {
				(void)SetPlayerRootAppCulled(
					false,
					"controller-exit");
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
		// SIMPLE GUIDE
		// What: Wraps the dialogue-target resolver and suppresses only the recurring NPC retarget after a player shot is established.
		// Why: This stops snap-back without disabling native teardown/input logic.
		// Trigger: Called by the target-resolver hook.
		static void Thunk(void* dialogueState, void* dialogueContext)
		{
			PDC_TRACE_SCOPE();
			const auto perfStartedAt = PerfNowTicks();
			struct PerfScope
			{
				std::uint64_t startedAt;
				// SIMPLE GUIDE
				// What: Records the elapsed time for the surrounding performance scope.
				// Why: RAII makes timing cleanup happen even on early returns.
				// Trigger: Runs automatically when the timed function exits.
				~PerfScope() {
					PDC_TRACE_SCOPE(); PerfRecord(g_perfResolverHook, startedAt); }
			} perfScope{{ perfStartedAt }};

			if (g_userPluginBypass.load(std::memory_order_acquire) ||
				g_shipDialogueBypass.load(std::memory_order_acquire) ||
				g_scriptedSceneBypass.load(std::memory_order_acquire) ||
				g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
				Hook(dialogueState, dialogueContext);
				return;
			}

			if (g_bodyPreparationPending ||
				!g_dialogueLogicEnabled.load(std::memory_order_acquire) ||
				!IsDialogueMenuOpen()) {
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
					PDC_DIAGNOSTIC_INFO(
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

	// SIMPLE GUIDE
	// What: Actually performs a queued switch between NPC view and player view.
	// Why: It gathers visibility, handles, controller target, camera rebuild, expression, and safety work in one ordered place.
	// Trigger: Called by the subsystem described by this helper's role.
	void PerformSetViewOnTaskQueue(
		const bool showPlayer,
		const ViewReason reason)
	{
		PDC_TRACE_SCOPE();
		struct CompletionGuard
		{
			// SIMPLE GUIDE
			// What: Clears the matching 'job already queued' flag when queued work ends.
			// Why: An early return must not block all future camera/expression jobs.
			// Trigger: Runs automatically when a queued worker finishes.
			~CompletionGuard()
			{
				PDC_TRACE_SCOPE();
				g_actionQueued.store(
					false,
					std::memory_order_release);
			}
		} completionGuard;

		if (g_userPluginBypass.load(std::memory_order_acquire) ||
			g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
			PDC_DIAGNOSTIC_INFO(
				"View toggle aborted: plugin/native-response bypass is active");
			return;
		}

		// Hard guard: if the DialogueMenu has already closed by the time this
		// queued task runs, do nothing. Rebuilding the dialogue camera after the
		// menu is gone shoves the player back into a menu-less dialogue camera
		// with no way out — the soft-lock. Let the engine's normal teardown run.
		if (!g_dialogueOpen || !IsDialogueMenuOpen()) {
			PDC_DIAGNOSTIC_INFO(
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
					"pre-NPC-rebuild",
					true);
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

		PDC_DIAGNOSTIC_INFO(
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

		PDC_DIAGNOSTIC_INFO(
			"Rebuild returned; requested view={}, reason={}, duration={}ms",
			showPlayer ? "player" : "NPC",
			DescribeViewReason(reason),
			rebuildDurationMs);
	}

	// SIMPLE GUIDE
	// What: Requests a camera view change and queues the real work if another change is not already running.
	// Why: Camera rebuilds must not pile on top of each other when speech state changes quickly.
	// Trigger: Called by the subsystem described by this helper's role.
	void QueueView(
		const bool showPlayer,
		const ViewReason reason)
	{
		PDC_TRACE_SCOPE();
		if (g_userPluginBypass.load(std::memory_order_acquire) ||
			g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
			return;
		}

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

		PDC_DIAGNOSTIC_INFO(
			"Queuing dialogue view={}, reason={}",
			showPlayer ? "player" : "NPC",
			DescribeViewReason(reason));

		g_taskInterface->AddTask([showPlayer, reason]() {
			PerformSetViewOnTaskQueue(
				showPlayer,
				reason);
		});
	}

	// SIMPLE GUIDE
	// What: Queues the V-key request to swap between player and NPC camera.
	// Why: Manual switching uses the same safe camera-change path as automatic switching.
	// Trigger: Called by the subsystem described by this helper's role.
	void QueueManualToggle()
	{
		PDC_TRACE_SCOPE();

		if (g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
			return;
		}

		const bool showPlayer =
			!g_playerViewRequested.load(
				std::memory_order_acquire);

		const auto now = GetTickCount64();
		g_autoIgnoreUntil = std::max(
			g_autoIgnoreUntil,
			now + kManualCameraOverrideGraceMs);
		g_autoCandidateInitialized = false;
		g_autoCandidateSince = 0;

		PDC_DIAGNOSTIC_INFO(
			"Manual V camera override: requestedView={}, mode={}, autoResumeIn={}ms",
			showPlayer ? "player" : "NPC",
			g_switchMode == SwitchMode::kAutomatic ? "automatic" : "manual",
			kManualCameraOverrideGraceMs);

		QueueView(showPlayer, ViewReason::kManual);
	}


	// SIMPLE GUIDE
	// What: Turns normal mod dialogue ownership off and gives a detected special scene back to native Starfield.
	// Why: Quest cutscenes can use camera rules that ordinary dialogue retargeting should not fight.
	// Trigger: Triggered when scripted-scene detection decides a special camera has taken over.
	void EnterScriptedSceneBypass(
		const RE::CameraState state,
		const char* reason)
	{
		PDC_TRACE_SCOPE();
		if (g_scriptedSceneBypass.exchange(
				true,
				std::memory_order_acq_rel)) {
			return;
		}

		ResetDialogueChoiceNativeWindow(
			"scripted scene bypass",
			false);

		// Publish bypass before clearing plugin state so hooks immediately become
		// native-only. Undo only plugin-owned actor presentation.
		const bool wasLogicEnabled =
			g_dialogueLogicEnabled.exchange(
				false,
				std::memory_order_acq_rel);
		g_dialogueActiveForHooks.store(false, std::memory_order_release);
		g_actionQueued.store(false, std::memory_order_release);
		g_playerCameraLocked.store(false, std::memory_order_release);
		g_playerShotEstablished.store(false, std::memory_order_release);

		CancelPendingBodyPreparation("scripted-scene-bypass");
		CancelPendingPlayerShotExpression("scripted-scene-bypass");
		ClearPlayerExpressionOverride("scripted-scene-bypass");
		RestoreSpeakerObstruction("scripted-scene-bypass");

		if (wasLogicEnabled) {
			(void)SetPlayerRootAppCulled(false, "scripted-scene-bypass");
			MaybeEnsurePlayerHeadVisible("scripted-scene-bypass");
		}

		g_playerViewRequested.store(false, std::memory_order_release);
		g_capturedController.store(0, std::memory_order_release);
		g_capturedControllerTarget.store(0, std::memory_order_release);
		ResetDialogueControllerDedupState();
		ResetAutomaticState();

		PDC_DIAGNOSTIC_INFO(
			"[CINEMATIC] native bypass engaged: reason={} camera={}",
			reason,
			DescribeCameraState(state));
	}

	// SIMPLE GUIDE
	// What: Watches dialogue camera state for a hard scripted camera or a persistent non-dialogue handoff.
	// Why: The mod should stay active for ordinary dialogue but disappear politely for custom cinematics.
	// Trigger: Called from Tick before normal dialogue work.
	void UpdateScriptedSceneDetection()
	{
		PDC_TRACE_SCOPE();
		if (!g_dialogueOpen ||
			g_shipDialogueBypass.load(std::memory_order_acquire) ||
			g_scriptedSceneBypass.load(std::memory_order_acquire)) {
			return;
		}

		const auto state = GetCurrentPlayerCameraState();

		if (state != g_lastObservedDialogueCameraState) {
			g_lastObservedDialogueCameraState = state;
			PDC_DIAGNOSTIC_INFO(
				"[CINEMATIC] dialogue camera state={}",
				DescribeCameraState(state));
		}

		if (IsHardScriptedCameraState(state)) {
			EnterScriptedSceneBypass(state, "hard-camera-state");
			return;
		}

		// After normal dialogue logic is established, a persistent non-Dialogue
		// camera state is strong evidence of a quest/scripted handoff. Do not run
		// this heuristic during our own FPP/TPP body repair.
		if (g_dialogueLogicEnabled.load(std::memory_order_acquire) &&
			!g_bodyPreparationPending &&
			!g_actionQueued.load(std::memory_order_acquire) &&
			state != RE::CameraState::kDialogue) {
			const auto now = GetTickCount64();

			if (state != g_scriptedCameraCandidateState) {
				g_scriptedCameraCandidateState = state;
				g_scriptedCameraCandidateSince = now;
				return;
			}

			if (g_scriptedCameraCandidateSince != 0 &&
				now >= g_scriptedCameraCandidateSince +
					kScriptedNonDialogueConfirmMs) {
				EnterScriptedSceneBypass(
					state,
					"persistent-non-dialogue-camera");
			}
			return;
		}

		g_scriptedCameraCandidateState = RE::CameraState::kTotal;
		g_scriptedCameraCandidateSince = 0;
	}


	// SIMPLE GUIDE
	// What: Keeps the NPC camera while the NPC talks and waits for continuous silence before requesting the player camera.
	// Why: The 1500 ms silence hold bridges gaps between ordered voice fragments and prevents the repeatable dialogue-skip bug.
	// Trigger: Called every active-dialogue Tick in automatic mode.
	void UpdateAutomaticCamera()
	{
		PDC_TRACE_SCOPE();
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			// SIMPLE GUIDE
			// What: Records the elapsed time for the surrounding performance scope.
			// Why: RAII makes timing cleanup happen even on early returns.
			// Trigger: Runs automatically when the timed function exits.
			~PerfScope() {
				PDC_TRACE_SCOPE(); PerfRecord(g_perfAutomaticCamera, startedAt); }
		} perfScope{ perfStartedAt };
		if (!g_dialogueLogicEnabled.load(std::memory_order_acquire) ||
			g_dialogueChoiceNativeWindow.load(std::memory_order_acquire) ||
			g_switchMode != SwitchMode::kAutomatic ||
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

			PDC_DIAGNOSTIC_INFO(
				"[FLOW] speaker={} handle=0x{:08X} desired={} current={}",
				speakerTalking ? "talking" : "silent",
				GetSpeakerHandle(),
				desiredPlayerView ? "player" : "NPC",
				g_playerViewRequested.load(std::memory_order_acquire) ?
					"player" : "NPC");

			if (!desiredPlayerView) {
				// If obstruction handling had hidden the NPC during the player shot,
				// restore them as soon as speech starts. The normal NPC camera delay
				// then doubles as a short render-history prewarm.
				RestoreSpeakerObstruction("speaker-start-prewarm");
				if (auto* speakerRoot = RememberDialogueSpeakerRoot()) {
					(void)SetRootAppCulled(
						speakerRoot,
						false,
						"speaker",
						"speaker-start-prewarm",
						true);
				}
			}

			g_lastSpeakerTalking = speakerTalking;
			return;
		}

		const auto requiredDelay =
			desiredPlayerView ?
				std::max(
					g_autoPlayerDelayMs,
					g_autoFragmentSilenceHoldMs) :
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

		PDC_DIAGNOSTIC_INFO(
			"[FLOW] auto switch request={} stableFor={}ms speaker=0x{:08X}",
			desiredPlayerView ? "player" : "NPC",
			now - g_autoCandidateSince,
			GetSpeakerHandle());

		QueueView(
			desiredPlayerView,
			desiredPlayerView ?
				ViewReason::kSpeakerStopped :
				ViewReason::kSpeakerStarted);
	}


	// SIMPLE GUIDE
	// What: Immediately turns the mod's camera behavior off and hands control back to native Starfield.
	// Why: F10 is an emergency escape hatch when a conversation or camera state behaves badly.
	// Trigger: Called by the subsystem described by this helper's role.
	void EnterUserPluginBypass(const bool dialogueOpen)
	{
		PDC_TRACE_SCOPE();
		// Publish bypass first. Any engine worker thread entering one of our hooks
		// from this point forward will immediately fall through to Starfield.
		g_userPluginBypass.store(true, std::memory_order_release);
		ResetDialogueChoiceNativeWindow(
			"user plugin bypass",
			false);

		if (dialogueOpen) {
			(void)RememberDialogueSpeakerReference();
			(void)RememberDialogueSpeakerRoot();
		}

		g_dialogueLogicEnabled.store(false, std::memory_order_release);
		g_dialogueActiveForHooks.store(false, std::memory_order_release);
		g_actionQueued.store(false, std::memory_order_release);
		g_playerViewRequested.store(false, std::memory_order_release);
		g_playerCameraLocked.store(false, std::memory_order_release);
		g_playerShotEstablished.store(false, std::memory_order_release);
		g_expressionActionQueued.store(false, std::memory_order_release);
		g_automaticExpressionTaskQueued.store(false, std::memory_order_release);

		CancelPendingBodyPreparation("user plugin bypass");
		g_firstPersonBodyRepairStage = FirstPersonBodyRepairStage::kNone;
		g_firstPersonBodyRepairDialogueUpdates.store(0, std::memory_order_release);
		ResetPreDialogueBodyPrime();
		CancelPendingPlayerShotExpression("user plugin bypass");
		ClearPlayerExpressionOverride("user plugin bypass");
		ResetSpeakerHeadAnchorLock();

		// Leave no actor hidden by us when control is handed back. We deliberately
		// do not rebuild or force a camera here; native DialogueCameraState gets the
		// next update and decides what the shot should be.
		RestoreSpeakerObstruction("user-plugin-bypass");
		ForceDialogueActorsVisible("user-plugin-bypass");

			g_exitVisibilityCleanupUntil.store(0, std::memory_order_release);
		g_exitVisibilityLastLateUpdateAt.store(0, std::memory_order_release);
		g_exitVisibilityStartedAt.store(0, std::memory_order_release);
		g_exitVisibilityLastRepairAt.store(0, std::memory_order_release);
		g_exitVisibilityRepairStage.store(0, std::memory_order_release);
		g_exitVisibilityPulsePending.store(false, std::memory_order_release);
		g_exitVisibilityRepairCount.store(0, std::memory_order_relaxed);
		g_exitVisibilityPulseCount.store(0, std::memory_order_relaxed);
		g_exitPreRenderVisibilityRepaired.store(false, std::memory_order_release);
		g_nextObstructionCheckAt.store(0, std::memory_order_release);
		g_nextHeadVisibilityCheckAt.store(0, std::memory_order_release);
		g_lastDialogueSpeakerReference.store(0, std::memory_order_release);
		g_lastDialogueSpeakerRoot.store(0, std::memory_order_release);
		g_culledSpeakerRoot.store(0, std::memory_order_release);
		ResetPlayerHeadAnchorCache();

		g_firstPersonRestorePending = false;
		g_firstPersonRestoreAt = 0;
		g_gameplayCameraRestoreDeadlineAt = 0;
		g_restoreFirstPersonAfterDialogue = false;
		g_forcedThirdPersonForDialogue = false;

		g_capturedController.store(0, std::memory_order_release);
		g_capturedControllerTarget.store(0, std::memory_order_release);
		ResetDialogueControllerDedupState();
		ResetAutomaticState();
		g_viewTransitionRetryAfter.store(0, std::memory_order_relaxed);
		g_viewTransitionRetryWarningArmed.store(false, std::memory_order_relaxed);

		PDC_DIAGNOSTIC_INFO(
			"User plugin bypass enabled; native Starfield camera/dialogue logic now owns the scene");
	}

	// Returns true when a rising-edge press changed the bypass state.
	// SIMPLE GUIDE
	// What: Checks for a fresh F10 key press and toggles native bypass mode once per press.
	// Why: Holding the key should not switch the mod on and off hundreds of times.
	// Trigger: Called by the subsystem described by this helper's role.
	[[nodiscard]] bool PollUserPluginBypassToggle(const bool dialogueOpen)
	{
		PDC_TRACE_SCOPE();
		const bool keyDown =
			(GetAsyncKeyState(g_pluginBypassToggleKey) & 0x8000) != 0;
		const bool pressed = keyDown && !g_pluginBypassToggleWasDown;
		g_pluginBypassToggleWasDown = keyDown;

		if (!pressed) {
			return false;
		}

		const bool wasBypassed =
			g_userPluginBypass.load(std::memory_order_acquire);

		if (!wasBypassed) {
			EnterUserPluginBypass(dialogueOpen);
			// Synchronise the menu edge tracker while the plugin is dormant. This
			// prevents a fake close/open edge when the user later re-enables it.
			g_dialogueOpen = dialogueOpen;
			ShowPluginBypassNotification(true);
			return true;
		}

		g_userPluginBypass.store(false, std::memory_order_release);
		ShowPluginBypassNotification(false);
		PDC_DIAGNOSTIC_INFO("User plugin bypass disabled; waiting for native attach point");

		if (dialogueOpen) {
			// If this is already a known ship-hailing session, keep the existing
			// native-only ship bypass. Otherwise force the ordinary dialogue-open
			// lifecycle to run once below so re-enabling mid-conversation is clean.
			if (g_shipDialogueBypass.load(std::memory_order_acquire)) {
				g_dialogueOpen = true;
			} else {
				g_dialogueOpen = false;
			}
		} else {
			g_dialogueOpen = false;
		}

		return true;
	}

	// SIMPLE GUIDE
	// What: Runs the mod's heartbeat: menus, ship bypass, F10, dialogue edges, scripted scenes, opening/closing, body repair, automatic switching, expressions, V/X, and performance snapshots.
	// Why: Hooks do narrow engine-facing work; Tick coordinates the larger lifecycle over time.
	// Trigger: Registered permanently through SFSE TaskInterface.
	void Tick()
	{
		PDC_TRACE_SCOPE();
		const auto perfStartedAt = PerfNowTicks();
		struct PerfScope
		{
			std::uint64_t startedAt;
			// SIMPLE GUIDE
			// What: Records the elapsed time for the surrounding performance scope.
			// Why: RAII makes timing cleanup happen even on early returns.
			// Trigger: Runs automatically when the timed function exits.
			~PerfScope() {
				PDC_TRACE_SCOPE(); PerfRecord(g_perfTick, startedAt); }
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
		LogResourceTelemetry("tick");

		const bool shipHudOpen = IsSpaceshipHudOpen();
		if (!dialogueOpen && shipHudOpen) {
			if (!g_shipGameplayBypass.exchange(
					true,
					std::memory_order_acq_rel)) {
				PDC_DIAGNOSTIC_INFO(
					"[SHIP] SpaceshipHudMenu active; plugin gameplay/perspective work bypassed");
			}

			if (g_firstPersonRestorePending) {
				g_firstPersonRestorePending = false;
				g_gameplayCameraRestoreDeadlineAt = 0;
				g_restoreFirstPersonAfterDialogue = false;
				g_forcedThirdPersonForDialogue = false;
				PDC_DIAGNOSTIC_INFO(
					"[SHIP] discarded pending dialogue perspective restore on cockpit entry");
			}

			g_vWasDown = false;
			g_expressionWasDown = false;
			return;
		}

		if (g_shipGameplayBypass.exchange(
				false,
				std::memory_order_acq_rel)) {
			PDC_DIAGNOSTIC_INFO("[SHIP] SpaceshipHudMenu closed; normal plugin observation resumed");
		}

		(void)PollUserPluginBypassToggle(dialogueOpen);

		if (g_userPluginBypass.load(std::memory_order_acquire)) {
			// Preserve only the passive ship-hailing session marker while globally
			// bypassed. Everything else belongs to Starfield until the user toggles
			// the plugin back on.
			if (dialogueOpen && !g_dialogueOpen && WasRecentlyPilotingShip()) {
				g_shipDialogueBypass.store(true, std::memory_order_release);
				PDC_DIAGNOSTIC_INFO(
					"Ship-piloting DialogueMenu observed while user bypass is active");
			}
			if (!dialogueOpen && g_dialogueOpen) {
				g_shipDialogueBypass.store(false, std::memory_order_release);
			}
			g_dialogueOpen = dialogueOpen;
			return;
		}

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
					ResetDialogueChoiceNativeWindow(
						"ship dialogue opened",
						false);

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

					PDC_DIAGNOSTIC_INFO(
						"Ship-piloting DialogueMenu detected; "
						"PointCameraAtPlayer fully bypassed for this session");
				} else {
				g_shipDialogueBypass.store(
					false,
					std::memory_order_release);
				ResetDialogueChoiceNativeWindow(
					"new dialogue",
					false);
				g_scriptedSceneBypass.store(
					false,
					std::memory_order_release);
				g_lastObservedDialogueCameraState = RE::CameraState::kTotal;
				g_scriptedCameraCandidateState = RE::CameraState::kTotal;
				g_scriptedCameraCandidateSince = 0;
				g_exitPreRenderVisibilityRepaired.store(
					false,
					std::memory_order_release);
				g_exitVisibilityCleanupUntil.store(
					0,
					std::memory_order_release);
				g_exitVisibilityLastLateUpdateAt.store(
					0,
					std::memory_order_release);
				g_exitVisibilityStartedAt.store(
					0,
					std::memory_order_release);
				g_exitVisibilityLastRepairAt.store(
					0,
					std::memory_order_release);
				g_exitVisibilityRepairStage.store(
					0,
					std::memory_order_release);
				g_exitVisibilityPulsePending.store(
					false,
					std::memory_order_release);
				g_exitVisibilityRepairCount.store(
					0,
					std::memory_order_relaxed);
				g_exitVisibilityPulseCount.store(
					0,
					std::memory_order_relaxed);
				CancelDialogueOpening("dialogue-open-reset");
				CancelDialogueClosing("dialogue-open");
				CancelExitPerspectiveRefresh("dialogue-open");
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
				// Native opening handshake owns the first few DialogueCameraState
				// updates. The hooks must stay transparent until it completes.
				g_dialogueLogicEnabled.store(
					false,
					std::memory_order_release);
				g_dialogueActiveForHooks.store(
					false,
					std::memory_order_release);
				// Remember whether this conversation was entered from first person.
				// If so, initialise the external body in the BACKGROUND during the
				// native NPC opening line, before mod camera logic is enabled. This
				// keeps the FP -> TP repair out of the first dialogue selection.
				g_restoreFirstPersonAfterDialogue =
					g_gameplayViewKnown && g_lastGameplayFirstPerson;

				PDC_RELEASE_INFO(
					"[FP-ENTRY] gameplayViewKnown={} stablePerspective={} "
					"pendingCandidate={} candidateAge={}ms -> bodyPrepNeeded={}",
					g_gameplayViewKnown ? "yes" : "no",
					g_lastGameplayFirstPerson ? "first" : "third",
					g_gameplayViewCandidateKnown ?
						(g_gameplayViewCandidateFirstPerson ? "first" : "third") :
						"none",
					g_gameplayViewCandidateKnown &&
						GetTickCount64() >= g_gameplayViewCandidateSince ?
						GetTickCount64() - g_gameplayViewCandidateSince :
						0,
					g_restoreFirstPersonAfterDialogue ? "yes" : "no");

				g_forcedThirdPersonForDialogue = false;
						g_firstPersonRestorePending = false;
				g_gameplayCameraRestoreDeadlineAt = 0;
				g_bodyPreparationPending = false;
				g_bodyPreparationResumeAt = 0;
				g_bodyPreparationDeadlineAt = 0;
				g_lastFunExpressionIndex = kExpressionCount;
				CancelPendingPlayerShotExpression("dialogue-open reset");

				// Begin fully native. For FPP entry, first prime the third-person
				// player body while we are still in the pre-cinematic gameplay camera.
				BeginDialogueOpenBodyPreparation();
				// Disabled: returning to FPP before cinematic dialogue exposed the
				// first-person body/neck pose from an external camera.
				ResetPreDialogueBodyPrime();

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

				PDC_DIAGNOSTIC_INFO(
					"DialogueMenu opened; speaker=0x{:08X}, player=0x{:08X}, "
					"restoreFirstPerson={}",
					GetSpeakerHandle(),
					GetPlayerHandle(),
					g_restoreFirstPersonAfterDialogue ? "yes" : "no");
				}
			} else {
				ResetDialogueChoiceNativeWindow(
					"dialogue close edge",
					false);

				const bool wasShipDialogue =
					g_shipDialogueBypass.exchange(
						false,
						std::memory_order_acq_rel);
				g_scriptedSceneBypass.store(
					false,
					std::memory_order_release);
				g_scriptedCameraCandidateState = RE::CameraState::kTotal;
				g_scriptedCameraCandidateSince = 0;
				g_lastObservedDialogueCameraState = RE::CameraState::kTotal;
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

					PDC_DIAGNOSTIC_INFO(
						"Ship dialogue closed; bypass cleared, "
						"native teardown left untouched");
				} else {
				const bool restoreFirstPersonAfterClose =
					g_restoreFirstPersonAfterDialogue &&
					g_forcedThirdPersonForDialogue;

				DisableDialogueLogic("dialogue-close");
				CancelDialogueOpening("dialogue-close");
				BeginDialogueClosing(true);

				PDC_DIAGNOSTIC_INFO(
					"[EXIT] DialogueMenu closed; waiting for real DialogueCameraState exit");

				if (restoreFirstPersonAfterClose) {
					const auto now = GetTickCount64();
					g_firstPersonRestorePending = true;
					g_firstPersonRestoreAt = now + g_firstPersonRestoreDelayMs;
					g_gameplayCameraRestoreDeadlineAt = now + 1500;
					PDC_RELEASE_INFO(
						"[FP-RESTORE] dialogue closed after body repair; waiting {}ms "
						"before restoring original FPP if native Starfield has not "
						"already done so",
						g_firstPersonRestoreDelayMs);
				} else {
					g_firstPersonRestorePending = false;
					g_firstPersonRestoreAt = 0;
					g_gameplayCameraRestoreDeadlineAt = 0;
					g_restoreFirstPersonAfterDialogue = false;
					g_forcedThirdPersonForDialogue = false;
				}

				ResetPreDialogueBodyPrime();
				g_gameplayViewCandidateKnown = false;
				g_gameplayViewCandidateSince = 0;

				g_lastDialogueSpeakerReference.store(0, std::memory_order_release);
				g_lastDialogueSpeakerRoot.store(0, std::memory_order_release);
				g_culledSpeakerRoot.store(0, std::memory_order_release);

				LogPerformanceSnapshot("dialogue-close FINAL");
				LogResourceTelemetry("dialogue-close FINAL");

				PDC_RELEASE_INFO(
					"[LOGIC] DialogueMenu closed; mod OFF, native Starfield teardown untouched");

				}
			}
		}

		if (g_shipDialogueBypass.load(std::memory_order_acquire)) {
			return;
		}

		if (g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
			const bool stillNative =
				UpdateDialogueChoiceNativeWindow();

			UpdatePerformanceSnapshot();

			if (stillNative ||
				g_dialogueChoiceNativeWindow.load(std::memory_order_acquire)) {
				g_vWasDown = false;
				g_expressionWasDown = false;
				return;
			}

			// Response ended on this Tick. Continue through the normal cinematic
			// detector and automatic/manual logic. Automatic mode was seeded with
			// the silence interval already observed, so it does not wait another
			// artificial 1500 ms before requesting the player shot.
		}

		if (dialogueOpen) {
			UpdateScriptedSceneDetection();
			if (g_scriptedSceneBypass.load(std::memory_order_acquire)) {
				return;
			}
			UpdateDialogueOpening();
		}

		if (!dialogueOpen) {
			UpdateDialogueClosing();

			if (g_firstPersonRestorePending) {
				UpdatePendingFirstPersonRestore();
			}
		}

		if (!g_dialogueLogicEnabled.load(std::memory_order_acquire)) {
			g_vWasDown = false;
			g_expressionWasDown = false;
			UpdatePerformanceSnapshot();
			return;
		}

		const bool bodyPrepWasPending = g_bodyPreparationPending;
		UpdatePendingBodyPreparation();
		if (bodyPrepWasPending || g_bodyPreparationPending) {
			UpdatePerformanceSnapshot();
			return;
		}

		UpdateAutomaticCamera();
		UpdatePendingPlayerShotExpression();
		UpdatePerformanceSnapshot();

		// V/X only matter while dialogue is open. The global bypass key was
		// already polled above so it remains available anywhere in gameplay.
		if (!dialogueOpen) {
			g_vWasDown = false;
			g_expressionWasDown = false;
			return;
		}

		const bool vIsDown =
			(GetAsyncKeyState(g_manualSwitchKey) & 0x8000) != 0;

		if (vIsDown &&
			!g_vWasDown) {
			PDC_DIAGNOSTIC_INFO(
				"Camera switch key pressed; mode={}",
				g_switchMode == SwitchMode::kAutomatic ? "automatic" : "manual");
			QueueManualToggle();
		}

		g_vWasDown = vIsDown;

		const bool expressionIsDown =
			(GetAsyncKeyState(g_expressionCycleKey) & 0x8000) != 0;

		if (expressionIsDown &&
			!g_expressionWasDown) {
			PDC_DIAGNOSTIC_INFO("Expression cycle key pressed");
			QueueExpressionCycle();
		}

		g_expressionWasDown = expressionIsDown;

	}
}

// SIMPLE GUIDE
// What: Initialises SFSE early and reserves trampoline space for hooks.
// Why: The branch stubs need memory before normal plugin load finishes.
// Trigger: Called by SFSE during preload.
SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	PDC_TRACE_SCOPE();
	// Two CALL5 hooks need 28 bytes of branch-stub storage.
	SFSE::Init(a_sfse, {
		.trampoline = true,
		.trampolineSize = 96
	});
	return true;
}

// SIMPLE GUIDE
// What: Loads settings, starts the notification helper, prints startup/logging state, gets TaskInterface, and registers Tick.
// Why: This is the main 'turn the plugin on' entry point.
// Trigger: Called by SFSE during plugin load.
SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	PDC_TRACE_SCOPE();
	SFSE::Init(a_sfse);
	LoadSettings();
	StartExpressionNotificationThread();

	REX::INFO("PointCameraAtPlayer 1.18.39 pre-selection native-release test loaded");
	PDC_RELEASE_INFO(
		"[CHOICE-RELEASE] native player-camera path restored; selection "
		"synchronously restores dialogue/controller target to the speaker "
		"before original OnDialogueSelect; +0x9C preserved; no selection-time rebuild");
	REX::INFO(
		"PointCameraAtPlayer logging: debug={} performance={}",
		g_debugLogging ? "on" : "off",
		g_performanceLogging ? "on" : "off");

	g_taskInterface = SFSE::GetTaskInterface();
	if (!g_taskInterface) {
		REX::CRITICAL("SFSE TaskInterface is unavailable");
		return false;
	}

	g_taskInterface->AddPermanentTask(Tick);


	return true;
}
