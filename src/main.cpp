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
#include "RE/N/NiAVObject.h"
#include "RE/N/NiMatrix3.h"
#include "RE/N/NiPoint.h"
#include "RE/N/NiNode.h"
#include "RE/P/PlayerCamera.h"
#include "RE/T/TESObjectREFR.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	constexpr char kDialogueMenuName[] = "DialogueMenu";
	constexpr char kShipHudMenuName[] = "ShipHudMenu";
	constexpr char kShipHudMenuAltName[] = "ShipHUDMenu";
	// Hailed ship speakers are physically far from the player even when cockpit
	// view reports itself as generic first person and the ship HUD is already hidden.
	// Use this only on the DialogueMenu opening edge as a fallback classifier.
	constexpr float kRemoteShipDialogueDistance = 500.0F;
	constexpr char kIniPath[] =
		".\\Data\\SFSE\\Plugins\\PointCameraAtPlayer.ini";

	enum class SwitchMode : std::uint32_t
	{
		kManual = 0,
		kAutomatic = 1,
		// Automatic switching with a render-only player camera. The native
		// DialogueCameraState target remains the real NPC at all times.
		kVisualIsolated = 2
	};

	enum class ViewReason : std::uint32_t
	{
		kManual,
		kSpeakerStarted,
		kSpeakerStopped,
		kDialogueChoiceConfirmed
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

	// DialogueCameraState::Update. The lean visibility guard hooks only the
	// post-update edge so Starfield can perform all native camera/dialogue work
	// untouched, then clears the player root app-cull bit with one atomic write.
	constexpr std::uintptr_t kDialogueStateVtableRva = 0x4C27500;
	constexpr std::size_t kDialogueUpdateVtableIndex = 12;

	constexpr std::ptrdiff_t kControllerTargetHandle = 0x60;
	constexpr std::ptrdiff_t kControllerState = 0x9C;

	constexpr std::uint64_t kAppCulledBit = 0x1;
	constexpr std::size_t kSetAppCulledVtableIndex = 61;

	using SetAppCulled_t = void (*)(RE::NiAVObject*, bool);
	using RebuildDialogueCamera_t = void (*)(void*, bool);
	using ChangeAnimFaceArchetype_t =
		void (*)(RE::Actor*, RE::BGSKeyword*);

	// Forward declarations used by the staged body-preparation and camera hooks.
	[[nodiscard]] RebuildDialogueCamera_t GetRebuildFunction();

	const SFSE::TaskInterface* g_taskInterface = nullptr;

	SwitchMode g_switchMode = SwitchMode::kAutomatic;
	int g_manualSwitchKey = 'V';
	int g_expressionCycleKey = 'X';
	std::uint32_t g_autoPlayerDelayMs = 250;
	std::uint32_t g_autoNpcDelayMs = 60;
	// Delay automatic player-to-NPC release until Starfield has safely started
	// the ordered voice response.
	std::uint32_t g_autoNpcSpeechLeadMs = 2000;
	// Stable silence before returning to the player.
	std::uint32_t g_autoNpcHoldMs = 900;
	std::uint32_t g_autoStartupGraceMs = 350;
	std::uint32_t g_dialogueOpenCameraDelayMs = 350;
	bool g_expressionNotifications = true;
	std::uint32_t g_expressionNotificationDurationMs = 1400;
	std::uint32_t g_expressionApplyDelayMs = 120;
	bool g_funMode = false;
	// Force a known-good third-person body state while dialogue opens.
	bool g_forceSheatheOnDialogue = true;
	bool g_forceThirdPersonOnDialogue = true;
	// Keep an established player shot from being overwritten by Starfield's
	// recurring dialogue-target resolver. This is bounded by DialogueMenu state
	// so native teardown/input cleanup always resumes when dialogue closes.
	bool g_suppressCameraRecentering = true;

	// Experimental render-only player camera used by SwitchMode::kVisualIsolated.
	// These values affect only the camera root transform after Starfield has
	// completed its native NPC-targeted dialogue update.
	std::uint32_t g_visualCameraDistancePercent = 38;
	std::uint32_t g_visualCameraMinDistance = 34;
	std::uint32_t g_visualCameraMaxDistance = 82;
	std::uint32_t g_visualCameraSideOffset = 30;
	std::int32_t g_visualCameraVerticalOffset = 2;
	std::uint32_t g_visualCameraAimHeightPercent = 55;

	std::size_t g_defaultExpressionIndex = kExpressionCount;

	std::atomic_flag g_tickLock = ATOMIC_FLAG_INIT;
	std::atomic_bool g_actionQueued = false;
	std::atomic_bool g_playerViewRequested = false;
	std::atomic_uintptr_t g_capturedController = 0;
	std::atomic_uint32_t g_capturedControllerTarget = 0;
	// Cached only while DialogueMenu is open. Resolved once per incoming player
	// shot, never searched from the per-frame camera hook.
	std::atomic_uintptr_t g_playerRootForShot = 0;
	std::atomic_uintptr_t g_playerSetAppCulledForShot = 0;
	std::atomic_bool g_visibilityGuardRefreshPending = false;
	std::atomic_ullong g_nextVisibilityGuardRefreshAt = 0;
	std::atomic_uint32_t g_visibilityGuardRefreshCount = 0;
	std::atomic_bool g_visibilityGuardHookObserved = false;
	std::atomic_uint32_t g_visibilityGuardNativeCallLogCount = 0;
	std::atomic_bool g_expressionActionQueued = false;
	std::atomic_bool g_expressionOverrideActive = false;
	std::atomic_uintptr_t g_activeExpressionKeyword = 0;
	std::atomic_bool g_dialogueActiveForHooks = false;
	// Ship hails use DialogueMenu too, but they are not ordinary face-to-face
	// dialogue. While this flag is set the plugin becomes a complete spectator:
	// no camera transforms, dialogue targets, visibility, body prep, expressions,
	// or controller overrides. Starfield owns the whole interaction.
	std::atomic_bool g_shipDialogueBypass = false;
	// Main-thread speaker snapshot used by hooks running on engine workers.
	std::atomic_uint32_t g_dialogueSpeakerHandleForHooks = 0;
	// Main-thread actor-root snapshots for the render-only visual camera.
	std::atomic_uintptr_t g_visualPlayerRoot = 0;
	std::atomic_uintptr_t g_visualSpeakerRoot = 0;
	std::atomic_uint32_t g_visualCameraApplyLogCount = 0;
	std::atomic_uint32_t g_visualCameraFailureLogCount = 0;
	// The previous frame's native camera transform is restored before the next
	// DialogueCameraState update so Starfield never starts its calculation from
	// our render-only player composition.
	RE::NiTransform g_visualSavedNativeWorld{};
	RE::NiTransform g_visualSavedNativePreviousWorld{};
	std::atomic_bool g_visualNativeTransformSaved = false;
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
	// Last non-dialogue gameplay camera context. A hail switches into
	// DialogueCameraState before our next lifecycle tick, so remember whether the
	// immediately preceding gameplay frame was one of the ship-piloting cameras.
	bool g_lastGameplayWasShipCamera = false;
	// Cockpit/first-person flight can report a generic first-person camera state.
	// Track the ship HUD independently so hails are still recognized while the
	// player is actively seated at the controls.
	bool g_lastGameplayHadShipHud = false;
	bool g_vWasDown = false;
	bool g_expressionWasDown = false;
	std::size_t g_expressionIndex = 0;
	std::size_t g_lastFunExpressionIndex = kExpressionCount;
	std::uint32_t g_funRandomState = 0;
	ULONGLONG g_nextVisualRootRefreshAt = 0;
	std::uint32_t g_visualRootSpeakerHandle = 0;
	bool g_bodyPreparationPending = false;
	ULONGLONG g_bodyPreparationResumeAt = 0;
	ULONGLONG g_bodyPreparationDeadlineAt = 0;

	bool g_autoCandidateInitialized = false;
	bool g_autoCandidatePlayer = false;
	bool g_lastSpeakerTalking = false;
	ULONGLONG g_autoCandidateSince = 0;
	ULONGLONG g_autoIgnoreUntil = 0;

	std::atomic_bool g_pendingPlayerShotExpression = false;
	std::atomic_uint64_t g_pendingPlayerShotExpressionAt = 0;
	std::atomic_bool g_automaticExpressionTaskQueued = false;


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
		case ViewReason::kDialogueChoiceConfirmed:
			return "dialogue-choice-confirmed";
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
		const int mode =
			GetPrivateProfileIntA(
				"Settings",
				"iSwitchMode",
				1,
				kIniPath);

		g_switchMode =
			mode == 0 ?
				SwitchMode::kManual :
				(mode == 2 ?
					SwitchMode::kVisualIsolated :
					SwitchMode::kAutomatic);

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

		g_autoNpcSpeechLeadMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iAutoNpcSpeechLeadMs",
					2000,
					kIniPath));

		g_autoNpcHoldMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iAutoNpcHoldMs",
					900,
					kIniPath));

		g_autoStartupGraceMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iAutoStartupGraceMs",
					350,
					kIniPath));

		g_dialogueOpenCameraDelayMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iDialogueOpenCameraDelayMs",
					350,
					kIniPath));

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

		g_visualCameraDistancePercent =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iVisualCameraDistancePercent",
					38,
					kIniPath));
		g_visualCameraMinDistance =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iVisualCameraMinDistance",
					34,
					kIniPath));
		g_visualCameraMaxDistance =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iVisualCameraMaxDistance",
					82,
					kIniPath));
		g_visualCameraSideOffset =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iVisualCameraSideOffset",
					30,
					kIniPath));
		g_visualCameraVerticalOffset =
			GetPrivateProfileIntA(
				"Settings",
				"iVisualCameraVerticalOffset",
				2,
				kIniPath);
		g_visualCameraAimHeightPercent =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iVisualCameraAimHeightPercent",
					55,
					kIniPath));

		g_funMode =
			GetPrivateProfileIntA(
				"Settings",
				"iFunMode",
				0,
				kIniPath) != 0;

		g_expressionApplyDelayMs =
			static_cast<std::uint32_t>(
				GetPrivateProfileIntA(
					"Settings",
					"iExpressionApplyDelayMs",
					120,
					kIniPath));

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

		LoadDefaultExpressionSetting();

		g_autoPlayerDelayMs =
			std::min(g_autoPlayerDelayMs, 5000u);
		g_autoNpcDelayMs =
			std::min(g_autoNpcDelayMs, 5000u);
		g_autoNpcSpeechLeadMs =
			std::clamp(g_autoNpcSpeechLeadMs, 250u, 10000u);
		g_autoNpcHoldMs =
			std::clamp(g_autoNpcHoldMs, 250u, 5000u);
		g_autoStartupGraceMs =
			std::min(g_autoStartupGraceMs, 5000u);
		g_dialogueOpenCameraDelayMs =
			std::clamp(g_dialogueOpenCameraDelayMs, 100u, 1500u);
		g_visualCameraDistancePercent =
			std::clamp(g_visualCameraDistancePercent, 15u, 80u);
		g_visualCameraMinDistance =
			std::clamp(g_visualCameraMinDistance, 12u, 120u);
		g_visualCameraMaxDistance =
			std::clamp(g_visualCameraMaxDistance, g_visualCameraMinDistance, 180u);
		g_visualCameraSideOffset =
			std::clamp(g_visualCameraSideOffset, 0u, 100u);
		g_visualCameraVerticalOffset =
			std::clamp(g_visualCameraVerticalOffset, -40, 40);
		g_visualCameraAimHeightPercent =
			std::clamp(g_visualCameraAimHeightPercent, 0u, 100u);
		g_expressionApplyDelayMs =
			std::clamp(g_expressionApplyDelayMs, 32u, 1000u);
		g_expressionNotificationDurationMs =
			std::clamp(g_expressionNotificationDurationMs, 250u, 5000u);

		REX::INFO(
			"Settings: mode={}, manualKey={}, expressionKey={}, "
			"playerDelay={}ms, npcDelay={}ms, npcSpeechLead={}ms, "
			"responseEndSilence={}ms, startupGrace={}ms, "
			"dialogueOpenDelay={}ms, forceSheathe={}, forceThirdPerson={}, "
			"suppressRecentering={}, visualDistance={}%, visualRange={}-{}, "
			"visualSide={}, visualVertical={}, visualAimHeight={}%, "
			"expressionNotifications={}, notificationDuration={}ms, "
			"funMode={}, defaultExpression={}",
			g_switchMode == SwitchMode::kVisualIsolated ?
				"visual-isolated" :
				(g_switchMode == SwitchMode::kAutomatic ?
					"automatic" :
					"manual"),
			g_manualSwitchKey,
			g_expressionCycleKey,
			g_autoPlayerDelayMs,
			g_autoNpcDelayMs,
			g_autoNpcSpeechLeadMs,
			g_autoNpcHoldMs,
			g_autoStartupGraceMs,
			g_dialogueOpenCameraDelayMs,
			g_forceSheatheOnDialogue ? "on" : "off",
			g_forceThirdPersonOnDialogue ? "on" : "off",
			g_suppressCameraRecentering ? "on" : "off",
			g_visualCameraDistancePercent,
			g_visualCameraMinDistance,
			g_visualCameraMaxDistance,
			g_visualCameraSideOffset,
			g_visualCameraVerticalOffset,
			g_visualCameraAimHeightPercent,
			g_expressionNotifications ? "on" : "off",
			g_expressionNotificationDurationMs,
			g_funMode ? "on" : "off",
			g_defaultExpressionIndex < kExpressionCount ?
				kExpressionNames[g_defaultExpressionIndex] :
				"disabled");
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


	// Starfield's SetAppCulled virtual does more than flip the raw flag: it also
	// updates the render-side state associated with the object. The previous lean
	// build cleared the bit directly, which left the external player body absent
	// even though the flag read as visible.
	//
	// Cache both the current external player root and the native virtual function
	// after the player-shot rebuild. The hot hook performs no lookup, traversal,
	// validation or logging; it checks one bit and calls the cached native virtual
	// only when Starfield has culled the root again.
	[[nodiscard]] bool RefreshPlayerVisibilityGuardCache(
		const bool logResult)
	{
		auto* root = GetPlayerRoot3D();
		if (!root || !IsReadableAddress(root, sizeof(void*))) {
			g_playerRootForShot.store(0, std::memory_order_release);
			g_playerSetAppCulledForShot.store(0, std::memory_order_release);
			if (logResult) {
				REX::WARN(
					"Player visibility guard cache refresh failed: player 3D root unavailable");
			}
			return false;
		}

		auto** vtable = *reinterpret_cast<void***>(root);
		if (!vtable) {
			g_playerRootForShot.store(0, std::memory_order_release);
			g_playerSetAppCulledForShot.store(0, std::memory_order_release);
			if (logResult) {
				REX::WARN(
					"Player visibility guard cache refresh failed: player root vtable unavailable");
			}
			return false;
		}

		void* const rawFunction = vtable[kSetAppCulledVtableIndex];
		if (!IsExecutableAddress(rawFunction)) {
			g_playerRootForShot.store(0, std::memory_order_release);
			g_playerSetAppCulledForShot.store(0, std::memory_order_release);
			if (logResult) {
				REX::WARN(
					"Player visibility guard cache refresh failed: SetAppCulled vfunc is unavailable");
			}
			return false;
		}

		const auto previousRoot = g_playerRootForShot.exchange(
			reinterpret_cast<std::uintptr_t>(root),
			std::memory_order_acq_rel);
		g_playerSetAppCulledForShot.store(
			reinterpret_cast<std::uintptr_t>(rawFunction),
			std::memory_order_release);

		auto setAppCulled = reinterpret_cast<SetAppCulled_t>(rawFunction);
		setAppCulled(root, false);

		if (logResult ||
			previousRoot != reinterpret_cast<std::uintptr_t>(root)) {
			REX::INFO(
				"Player visibility guard cached native SetAppCulled: root={}, replaced={}",
				static_cast<void*>(root),
				previousRoot != 0 &&
				previousRoot != reinterpret_cast<std::uintptr_t>(root) ?
					"yes" : "no");
		}

		return true;
	}

	void DisarmPlayerVisibilityGuard()
	{
		g_visibilityGuardRefreshPending.store(false, std::memory_order_release);
		g_nextVisibilityGuardRefreshAt.store(0, std::memory_order_release);
		g_visibilityGuardRefreshCount.store(0, std::memory_order_release);
		g_playerRootForShot.store(0, std::memory_order_release);
		g_playerSetAppCulledForShot.store(0, std::memory_order_release);
	}

	void ArmPlayerVisibilityGuardForShot()
	{
		DisarmPlayerVisibilityGuard();
		g_visibilityGuardNativeCallLogCount.store(0, std::memory_order_release);
		(void)RefreshPlayerVisibilityGuardCache(true);

		// Re-resolve a handful of times after the rebuild. Starfield can replace
		// the external root during the first few camera updates. Ten inexpensive
		// main-thread refreshes are bounded and avoid any scene lookup in the hook.
		g_visibilityGuardRefreshPending.store(true, std::memory_order_release);
		g_nextVisibilityGuardRefreshAt.store(
			GetTickCount64() + 50,
			std::memory_order_release);

		REX::INFO(
			"Player visibility guard armed with native render-state update");
	}

	void UpdatePlayerVisibilityGuardCache()
	{
		if (!g_visibilityGuardRefreshPending.load(std::memory_order_acquire)) {
			return;
		}

		if (!g_dialogueActiveForHooks.load(std::memory_order_acquire) ||
			!g_playerViewRequested.load(std::memory_order_acquire)) {
			DisarmPlayerVisibilityGuard();
			return;
		}

		const auto now = GetTickCount64();
		if (now < g_nextVisibilityGuardRefreshAt.load(std::memory_order_acquire)) {
			return;
		}

		(void)RefreshPlayerVisibilityGuardCache(false);
		const auto count = g_visibilityGuardRefreshCount.fetch_add(
			1,
			std::memory_order_acq_rel) + 1;

		if (count >= 10) {
			g_visibilityGuardRefreshPending.store(false, std::memory_order_release);
			return;
		}

		g_nextVisibilityGuardRefreshAt.store(
			now + 50,
			std::memory_order_release);
	}

	inline void MaintainPlayerVisibilityFast()
	{
		auto* root = reinterpret_cast<RE::NiAVObject*>(
			g_playerRootForShot.load(std::memory_order_acquire));
		auto setAppCulled = reinterpret_cast<SetAppCulled_t>(
			g_playerSetAppCulledForShot.load(std::memory_order_acquire));
		if (!root || !setAppCulled) {
			return;
		}

		const auto current = std::atomic_ref<std::uint64_t>(root->flags).load(
			std::memory_order_relaxed);
		if ((current & kAppCulledBit) == 0) {
			return;
		}

		setAppCulled(root, false);

		// Diagnostic only for the first two actual native reveals per shot. This
		// proves the hook is active without restoring the former per-frame log spam.
		const auto count = g_visibilityGuardNativeCallLogCount.fetch_add(
			1,
			std::memory_order_acq_rel) + 1;
		if (count <= 2) {
			REX::INFO(
				"Player visibility guard restored native render state ({})",
				count);
		}
	}


	void QueueView(bool showPlayer, ViewReason reason);

	void BeginDialogueOpenBodyPreparation()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		if (g_forceSheatheOnDialogue) {
			(void)SheathePlayerWeapon("dialogue-open");
		}

		if (g_forceThirdPersonOnDialogue &&
			!camera->IsInThirdPerson()) {
			camera->ForceThirdPerson();
		}

		const auto now = GetTickCount64();
		g_bodyPreparationPending = true;
		g_bodyPreparationResumeAt =
			now + g_dialogueOpenCameraDelayMs;
		g_bodyPreparationDeadlineAt =
			now + std::max<std::uint32_t>(
				g_dialogueOpenCameraDelayMs + 1500u,
				2000u);

		REX::INFO(
			"Dialogue-open camera warm-up scheduled for {}ms",
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

		REX::INFO(
			"Dialogue-open camera warm-up cancelled: {}",
			phase);
	}

	void UpdatePendingBodyPreparation()
	{
		if (!g_bodyPreparationPending) {
			return;
		}

		if (!g_dialogueOpen || !IsDialogueMenuOpen()) {
			CancelPendingBodyPreparation("dialogue closed");
			return;
		}

		const auto now = GetTickCount64();
		if (now < g_bodyPreparationResumeAt) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		const bool dialogueStateAlreadyActive =
			camera->QCameraEquals(RE::CameraState::kDialogue);
		if (!dialogueStateAlreadyActive) {
			camera->SetCameraState(RE::CameraState::kDialogue);
		}
		if (!camera->QCameraEquals(RE::CameraState::kDialogue)) {
			if (now < g_bodyPreparationDeadlineAt) {
				g_bodyPreparationResumeAt = now + 32;
				return;
			}

			REX::WARN(
				"Dialogue-open camera restore timed out; leaving Starfield's current camera untouched");
			CancelPendingBodyPreparation("camera restore timeout");
			return;
		}

		void* const dialogueState = GetActiveDialogueState();
		const auto speakerHandle = GetSpeakerHandle();
		if ((!dialogueState || speakerHandle == 0) &&
			now < g_bodyPreparationDeadlineAt) {
			g_bodyPreparationResumeAt = now + 32;
			return;
		}

		g_bodyPreparationPending = false;
		g_bodyPreparationResumeAt = 0;
		g_bodyPreparationDeadlineAt = 0;
		g_playerViewRequested.store(
			false,
			std::memory_order_release);

		if (dialogueState && speakerHandle != 0) {
			const bool targetAlreadyActive =
				ReadHandle(dialogueState, kRequestedTargetHandle) == speakerHandle &&
				ReadHandle(dialogueState, kCurrentTargetHandle) == speakerHandle;

			if (!targetAlreadyActive) {
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
					rebuild(dialogueState, true);
				}
			}
		}


		REX::INFO(
			"Opening NPC shot established after {}ms",
			g_dialogueOpenCameraDelayMs);
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

		// The player shot needs only the mapped visual target. Camera rebuilding
		// is requested explicitly below; resetting unknown controller state here
		// is unnecessary and unsafe for dialogue progression.
		// Do not reset +0x9C. It was originally treated as a camera-only state,
		// but the controller also owns dialogue/input progression. Rewinding this
		// field while a response is being accepted can reactivate old branches.
		const bool statePreserved = true;

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
			"+0x9C {}->{}, writes=[target:{},state:preserved]",
			showPlayer ? "player" : "NPC",
			controller,
			beforeTarget,
			afterTarget,
			beforeState,
			afterState,
			targetWritten ? "ok" : "failed");

		return targetWritten && statePreserved;
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


	[[nodiscard]] bool IsVisualIsolatedMode()
	{
		return g_switchMode == SwitchMode::kVisualIsolated;
	}

	[[nodiscard]] bool IsShipPilotingCamera(const RE::PlayerCamera* camera)
	{
		if (!camera) {
			return false;
		}

		// These are the dedicated spacecraft camera states exposed by
		// PlayerCamera. kVehicle is deliberately excluded so ground-vehicle
		// conversations are not silently classified as ship hails.
		return camera->QCameraEquals(RE::CameraState::kFlight) ||
			camera->QCameraEquals(RE::CameraState::kShipFarTravel) ||
			camera->QCameraEquals(RE::CameraState::kShipAction) ||
			camera->QCameraEquals(RE::CameraState::kShipTargeting) ||
			camera->QCameraEquals(RE::CameraState::kShipCombatOrbit);
	}

	[[nodiscard]] bool IsShipHudOpen(RE::UI* ui)
	{
		if (!ui) {
			return false;
		}

		// In cockpit/first-person flight Starfield may expose the camera as the
		// generic first-person state rather than one of the dedicated ship camera
		// states above. The ship HUD is a much better indication that the player is
		// still seated at the controls. Sample both the current frame and the last
		// non-dialogue frame because the HUD can be hidden as DialogueMenu opens.
		static const RE::BSFixedString shipHudMenu{ kShipHudMenuName };
		static const RE::BSFixedString shipHudMenuAlt{ kShipHudMenuAltName };

		return ui->IsMenuOpen(shipHudMenu) ||
			ui->IsMenuOpen(shipHudMenuAlt);
	}

	[[nodiscard]] float GetOpeningDialogueSpeakerDistance()
	{
		auto* const playerRoot = GetPlayerRoot3D();
		auto* const speakerRoot =
			GetReferenceRoot3D(GetSpeakerReference());

		if (!playerRoot || !speakerRoot || playerRoot == speakerRoot) {
			return -1.0F;
		}

		return std::sqrt(
			playerRoot->world.translate.GetSquaredDistance(
				speakerRoot->world.translate));
	}

	[[nodiscard]] bool IsGenericFirstPersonCamera(
		const RE::PlayerCamera* camera)
	{
		return camera &&
			(camera->QCameraEquals(RE::CameraState::kFirstPerson) ||
			 camera->QCameraEquals(RE::CameraState::kIronSights));
	}

	void RestoreSavedNativeCameraTransform()
	{
		if (!g_visualNativeTransformSaved.exchange(
				false,
				std::memory_order_acq_rel)) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->cameraRoot) {
			return;
		}

		camera->cameraRoot->world = g_visualSavedNativeWorld;
		camera->cameraRoot->previousWorld =
			g_visualSavedNativePreviousWorld;
	}

	[[nodiscard]] float DotPoint(
		const RE::NiPoint3& left,
		const RE::NiPoint3& right)
	{
		return left.x * right.x +
			left.y * right.y +
			left.z * right.z;
	}

	[[nodiscard]] RE::NiPoint3 CrossPoint(
		const RE::NiPoint3& left,
		const RE::NiPoint3& right)
	{
		return {
			left.y * right.z - left.z * right.y,
			left.z * right.x - left.x * right.z,
			left.x * right.y - left.y * right.x
		};
	}

	[[nodiscard]] bool NormalizePoint(RE::NiPoint3& point)
	{
		const float lengthSquared = DotPoint(point, point);
		if (lengthSquared <= 0.000001F) {
			return false;
		}

		const float inverseLength =
			1.0F / static_cast<float>(std::sqrt(lengthSquared));
		point.x *= inverseLength;
		point.y *= inverseLength;
		point.z *= inverseLength;
		return true;
	}

	[[nodiscard]] RE::NiPoint3 ScalePoint(
		const RE::NiPoint3& point,
		const float scale)
	{
		return {
			point.x * scale,
			point.y * scale,
			point.z * scale
		};
	}

	[[nodiscard]] RE::NiPoint3 AddPoint(
		const RE::NiPoint3& left,
		const RE::NiPoint3& right)
	{
		return {
			left.x + right.x,
			left.y + right.y,
			left.z + right.z
		};
	}

	[[nodiscard]] RE::NiPoint3 SubtractPoint(
		const RE::NiPoint3& left,
		const RE::NiPoint3& right)
	{
		return {
			left.x - right.x,
			left.y - right.y,
			left.z - right.z
		};
	}

	[[nodiscard]] RE::NiPoint3 GetVisualAimPoint(
		RE::NiAVObject* root)
	{
		if (!root) {
			return {};
		}

		auto point = root->worldBound.center;
		point.z +=
			root->worldBound.radius *
			(static_cast<float>(g_visualCameraAimHeightPercent) / 100.0F);
		return point;
	}

	[[nodiscard]] RE::NiPoint3 GetStoredCameraAxis(
		const RE::NiMatrix3& matrix,
		const bool columns,
		const int index)
	{
		if (columns) {
			return {
				matrix.entry[0][index],
				matrix.entry[1][index],
				matrix.entry[2][index]
			};
		}

		return {
			matrix.entry[index][0],
			matrix.entry[index][1],
			matrix.entry[index][2]
		};
	}

	void SetStoredCameraAxis(
		RE::NiMatrix3& matrix,
		const bool columns,
		const int index,
		const RE::NiPoint3& axis)
	{
		if (columns) {
			matrix.entry[0][index] = axis.x;
			matrix.entry[1][index] = axis.y;
			matrix.entry[2][index] = axis.z;
			return;
		}

		matrix.entry[index][0] = axis.x;
		matrix.entry[index][1] = axis.y;
		matrix.entry[index][2] = axis.z;
	}

	struct CameraAxisMapping
	{
		bool columns{ false };
		int forwardIndex{ 1 };
		int forwardSign{ 1 };
		int upIndex{ 2 };
		int upSign{ 1 };
		int rightIndex{ 0 };
		int rightSign{ 1 };
		float forwardScore{ -1.0F };
		float upScore{ -1.0F };
	};

	[[nodiscard]] CameraAxisMapping InferCameraAxisMapping(
		const RE::NiMatrix3& nativeRotation,
		const RE::NiPoint3& nativeForward)
	{
		CameraAxisMapping result{};
		const RE::NiPoint3 worldUp{ 0.0F, 0.0F, 1.0F };

		for (int storage = 0; storage < 2; ++storage) {
			const bool columns = storage != 0;
			for (int index = 0; index < 3; ++index) {
				auto axis = GetStoredCameraAxis(
					nativeRotation,
					columns,
					index);
				if (!NormalizePoint(axis)) {
					continue;
				}

				for (const int sign : { -1, 1 }) {
					const float score =
						DotPoint(
							ScalePoint(axis, static_cast<float>(sign)),
							nativeForward);
					if (score > result.forwardScore) {
						result.columns = columns;
						result.forwardIndex = index;
						result.forwardSign = sign;
						result.forwardScore = score;
					}
				}
			}
		}

		result.upScore = -1.0F;
		for (int index = 0; index < 3; ++index) {
			if (index == result.forwardIndex) {
				continue;
			}

			auto axis = GetStoredCameraAxis(
				nativeRotation,
				result.columns,
				index);
			if (!NormalizePoint(axis)) {
				continue;
			}

			for (const int sign : { -1, 1 }) {
				const float score =
					DotPoint(
						ScalePoint(axis, static_cast<float>(sign)),
						worldUp);
				if (score > result.upScore) {
					result.upIndex = index;
					result.upSign = sign;
					result.upScore = score;
				}
			}
		}

		result.rightIndex =
			3 - result.forwardIndex - result.upIndex;

		auto nativeForwardAxis = GetStoredCameraAxis(
			nativeRotation,
			result.columns,
			result.forwardIndex);
		auto nativeUpAxis = GetStoredCameraAxis(
			nativeRotation,
			result.columns,
			result.upIndex);
		auto nativeRightRaw = GetStoredCameraAxis(
			nativeRotation,
			result.columns,
			result.rightIndex);

		nativeForwardAxis =
			ScalePoint(
				nativeForwardAxis,
				static_cast<float>(result.forwardSign));
		nativeUpAxis =
			ScalePoint(
				nativeUpAxis,
				static_cast<float>(result.upSign));
		(void)NormalizePoint(nativeForwardAxis);
		(void)NormalizePoint(nativeUpAxis);
		(void)NormalizePoint(nativeRightRaw);

		auto expectedRight =
			CrossPoint(nativeForwardAxis, nativeUpAxis);
		if (!NormalizePoint(expectedRight)) {
			result.rightSign = 1;
		} else {
			result.rightSign =
				DotPoint(nativeRightRaw, expectedRight) >= 0.0F ?
					1 :
					-1;
		}

		return result;
	}

	[[nodiscard]] float SegmentSphereClearanceVisual(
		const RE::NiPoint3& cameraPoint,
		const RE::NiPoint3& targetPoint,
		const RE::NiPoint3& sphereCenter,
		const float sphereRadius)
	{
		const auto segment =
			SubtractPoint(targetPoint, cameraPoint);
		const float segmentLengthSquared =
			DotPoint(segment, segment);
		if (segmentLengthSquared <= 0.0001F) {
			return -sphereRadius;
		}

		const auto cameraToSphere =
			SubtractPoint(sphereCenter, cameraPoint);
		const float projection = std::clamp(
			DotPoint(cameraToSphere, segment) /
				segmentLengthSquared,
			0.0F,
			0.98F);
		const auto nearest =
			AddPoint(
				cameraPoint,
				ScalePoint(segment, projection));
		const auto delta =
			SubtractPoint(sphereCenter, nearest);
		return static_cast<float>(
			std::sqrt(std::max(DotPoint(delta, delta), 0.0F))) -
			sphereRadius;
	}

	[[nodiscard]] bool BuildLookAtRotationFromNative(
		const RE::NiMatrix3& nativeRotation,
		const RE::NiPoint3& nativeCameraPoint,
		const RE::NiPoint3& nativeTargetPoint,
		const RE::NiPoint3& desiredCameraPoint,
		const RE::NiPoint3& desiredTargetPoint,
		RE::NiMatrix3& result)
	{
		auto nativeForward =
			SubtractPoint(nativeTargetPoint, nativeCameraPoint);
		if (!NormalizePoint(nativeForward)) {
			return false;
		}

		const auto mapping =
			InferCameraAxisMapping(
				nativeRotation,
				nativeForward);
		if (mapping.forwardScore < 0.72F ||
			mapping.upScore < 0.35F) {
			return false;
		}

		auto desiredForward =
			SubtractPoint(desiredTargetPoint, desiredCameraPoint);
		if (!NormalizePoint(desiredForward)) {
			return false;
		}

		const RE::NiPoint3 worldUp{ 0.0F, 0.0F, 1.0F };
		auto desiredRight =
			CrossPoint(desiredForward, worldUp);
		if (!NormalizePoint(desiredRight)) {
			desiredRight =
				ScalePoint(
					GetStoredCameraAxis(
						nativeRotation,
						mapping.columns,
						mapping.rightIndex),
					static_cast<float>(mapping.rightSign));
			if (!NormalizePoint(desiredRight)) {
				return false;
			}
		}

		auto desiredUp =
			CrossPoint(desiredRight, desiredForward);
		if (!NormalizePoint(desiredUp)) {
			return false;
		}

		result = nativeRotation;
		SetStoredCameraAxis(
			result,
			mapping.columns,
			mapping.forwardIndex,
			ScalePoint(
				desiredForward,
				static_cast<float>(mapping.forwardSign)));
		SetStoredCameraAxis(
			result,
			mapping.columns,
			mapping.upIndex,
			ScalePoint(
				desiredUp,
				static_cast<float>(mapping.upSign)));
		SetStoredCameraAxis(
			result,
			mapping.columns,
			mapping.rightIndex,
			ScalePoint(
				desiredRight,
				static_cast<float>(mapping.rightSign)));
		return true;
	}

	void ApplyIsolatedVisualPlayerCamera()
	{
		if (!IsVisualIsolatedMode() ||
			!g_dialogueActiveForHooks.load(std::memory_order_acquire) ||
			!g_playerViewRequested.load(std::memory_order_acquire)) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->cameraRoot) {
			return;
		}

		auto* playerRoot =
			reinterpret_cast<RE::NiAVObject*>(
				g_visualPlayerRoot.load(std::memory_order_acquire));
		auto* speakerRoot =
			reinterpret_cast<RE::NiAVObject*>(
				g_visualSpeakerRoot.load(std::memory_order_acquire));
		if (!playerRoot || !speakerRoot ||
			playerRoot == speakerRoot) {
			return;
		}

		const auto playerAim = GetVisualAimPoint(playerRoot);
		const auto speakerAim = GetVisualAimPoint(speakerRoot);
		auto playerToSpeaker =
			SubtractPoint(speakerAim, playerAim);
		const float actorDistanceSquared =
			DotPoint(playerToSpeaker, playerToSpeaker);
		if (actorDistanceSquared <= 4.0F ||
			!NormalizePoint(playerToSpeaker)) {
			return;
		}

		const float actorDistance =
			static_cast<float>(std::sqrt(actorDistanceSquared));
		const RE::NiPoint3 worldUp{ 0.0F, 0.0F, 1.0F };
		auto sideAxis =
			CrossPoint(playerToSpeaker, worldUp);
		if (!NormalizePoint(sideAxis)) {
			return;
		}

		float forwardDistance =
			actorDistance *
			(static_cast<float>(g_visualCameraDistancePercent) / 100.0F);
		forwardDistance = std::clamp(
			forwardDistance,
			static_cast<float>(g_visualCameraMinDistance),
			static_cast<float>(g_visualCameraMaxDistance));
		// Never place the visual camera past the NPC. In cramped conversations,
		// shrink the distance rather than allowing the camera to cross the native
		// target and create a reversed shot.
		forwardDistance = std::min(
			forwardDistance,
			std::max(12.0F, actorDistance * 0.68F));

		const auto nativeCameraPoint =
			camera->cameraRoot->world.translate;
		const auto nativeRelative =
			SubtractPoint(nativeCameraPoint, playerAim);
		int preferredSide =
			DotPoint(nativeRelative, sideAxis) < 0.0F ?
				-1 :
				1;

		auto buildCandidate = [&](const int side) {
			auto result =
				AddPoint(
					playerAim,
					ScalePoint(playerToSpeaker, forwardDistance));
			result = AddPoint(
				result,
				ScalePoint(
					sideAxis,
					static_cast<float>(side) *
						static_cast<float>(g_visualCameraSideOffset)));
			result.z += static_cast<float>(g_visualCameraVerticalOffset);
			return result;
		};

		const auto positiveCandidate = buildCandidate(1);
		const auto negativeCandidate = buildCandidate(-1);
		const float speakerRadius = std::clamp(
			speakerRoot->worldBound.radius * 0.58F,
			14.0F,
			42.0F);
		const float positiveClearance =
			SegmentSphereClearanceVisual(
				positiveCandidate,
				playerAim,
				speakerAim,
				speakerRadius);
		const float negativeClearance =
			SegmentSphereClearanceVisual(
				negativeCandidate,
				playerAim,
				speakerAim,
				speakerRadius);

		if (std::abs(positiveClearance - negativeClearance) > 1.0F) {
			preferredSide =
				positiveClearance > negativeClearance ?
					1 :
					-1;
		}

		const auto desiredCameraPoint =
			preferredSide > 0 ?
				positiveCandidate :
				negativeCandidate;

		RE::NiMatrix3 desiredRotation{};
		if (!BuildLookAtRotationFromNative(
				camera->cameraRoot->world.rotate,
				nativeCameraPoint,
				speakerAim,
				desiredCameraPoint,
				playerAim,
				desiredRotation)) {
			const auto failureCount =
				g_visualCameraFailureLogCount.fetch_add(
					1,
					std::memory_order_acq_rel) + 1;
			if (failureCount <= 3) {
				REX::WARN(
					"Visual-isolated camera skipped: native camera-axis mapping was inconclusive");
			}
			return;
		}

		// Preserve the fully native NPC-targeted result. It is restored at the
		// beginning of the next dialogue update before Starfield runs again.
		g_visualSavedNativeWorld = camera->cameraRoot->world;
		g_visualSavedNativePreviousWorld =
			camera->cameraRoot->previousWorld;
		g_visualNativeTransformSaved.store(
			true,
			std::memory_order_release);

		camera->cameraRoot->world.translate = desiredCameraPoint;
		camera->cameraRoot->world.rotate = desiredRotation;
		// Match previousWorld to the same render-only composition. This prevents
		// the override from generating a fabricated full-frame motion vector.
		camera->cameraRoot->previousWorld.translate = desiredCameraPoint;
		camera->cameraRoot->previousWorld.rotate = desiredRotation;

		const auto applyCount =
			g_visualCameraApplyLogCount.fetch_add(
				1,
				std::memory_order_acq_rel) + 1;
		if (applyCount == 1) {
			REX::INFO(
				"Visual-isolated player camera active: native target remains NPC, "
				"side={}, actorDistance={:.1f}, cameraDistance={:.1f}, "
				"clearance={:.1f}",
				preferredSide < 0 ? "left" : "right",
				actorDistance,
				forwardDistance,
				preferredSide > 0 ?
					positiveClearance :
					negativeClearance);
		}
	}


	class PlayerShotVisibilityGuardHook
	{
	public:
		static void Thunk(void* dialogueState)
		{
			if (g_shipDialogueBypass.load(std::memory_order_acquire)) {
				// Hailed-ship communications are entirely native. Do not even touch
				// the saved visual-camera transform or player visibility here.
				Hook(dialogueState);
				return;
			}

			// Undo the previous frame's render-only transform before Starfield
			// calculates its next native NPC-targeted composition.
			RestoreSavedNativeCameraTransform();

			// Let Starfield finish its complete dialogue/camera update first.
			Hook(dialogueState);

			if (g_dialogueActiveForHooks.load(std::memory_order_acquire) &&
				g_playerViewRequested.load(std::memory_order_acquire)) {
				if (!g_visibilityGuardHookObserved.exchange(
						true,
						std::memory_order_acq_rel)) {
					REX::INFO(
						"Player visibility guard observed its first post-camera update");
				}
				MaintainPlayerVisibilityFast();
				// In visual-isolated mode, Starfield has just completed an entirely
				// native NPC-targeted dialogue update. Override only the render camera
				// transform after that bookkeeping is finished.
				ApplyIsolatedVisualPlayerCamera();
			}
		}

		static inline REL::THookVFT<void(void*)> Hook{
			"PDC_PlayerShotVisibilityGuard",
			static_cast<REL::EHookStep>(2),
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
			g_capturedController.store(
				reinterpret_cast<std::uintptr_t>(controller),
				std::memory_order_release);
			g_capturedControllerTarget.store(
				targetHandle,
				std::memory_order_release);

			const bool playerMode =
				!IsVisualIsolatedMode() &&
				g_dialogueActiveForHooks.load(
					std::memory_order_acquire) &&
				g_playerViewRequested.load(
					std::memory_order_acquire);

			const auto speakerHandle =
				g_dialogueSpeakerHandleForHooks.load(
					std::memory_order_acquire);
			const auto playerHandle = GetPlayerHandle();

			const std::uint32_t nativeTarget =
				playerMode && speakerHandle != 0 ?
					speakerHandle :
					targetHandle;

			Hook(controller, nativeTarget);

			// The hook argument has just survived Starfield's native controller
			// call, so the +0x60 field is live here. Avoid VirtualQuery and
			// diagnostic field reads on every controller update.
			if (playerMode && playerHandle != 0) {
				auto* field =
					reinterpret_cast<std::uint32_t*>(
						static_cast<std::byte*>(controller) +
						kControllerTargetHandle);
				std::atomic_ref<std::uint32_t>(*field).store(
					playerHandle,
					std::memory_order_release);
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
			const bool playerModeLocked =
				!IsVisualIsolatedMode() &&
				g_suppressCameraRecentering &&
				g_dialogueActiveForHooks.load(
					std::memory_order_acquire) &&
				g_playerViewRequested.load(
					std::memory_order_acquire) &&
				dialogueState &&
				IsDialogueMenuOpen();

			if (!playerModeLocked) {
				Hook(dialogueState, dialogueContext);
				return;
			}

			const auto speakerHandle =
				g_dialogueSpeakerHandleForHooks.load(
					std::memory_order_acquire);
			if (speakerHandle == 0) {
				Hook(dialogueState, dialogueContext);
				return;
			}

			WriteHandle(
				dialogueState,
				kRequestedTargetHandle,
				speakerHandle);
			WriteHandle(
				dialogueState,
				kCurrentTargetHandle,
				speakerHandle);

			Hook(dialogueState, dialogueContext);

			if (g_dialogueActiveForHooks.load(
					std::memory_order_acquire) &&
				g_playerViewRequested.load(
					std::memory_order_acquire)) {
				ForcePlayerHandles(dialogueState);
			}

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

		if (!g_dialogueOpen || !IsDialogueMenuOpen() ||
			g_shipDialogueBypass.load(std::memory_order_acquire)) {
			return;
		}

		void* const dialogueState = GetActiveDialogueState();
		if (!dialogueState) {
			REX::WARN(
				"Toggle aborted: no active DialogueCameraState");
			return;
		}

		const bool currentPlayerView =
			g_playerViewRequested.load(
				std::memory_order_acquire);
		if (currentPlayerView == showPlayer) {
			return;
		}

		const bool nativeAutomaticNpcRelease =
			!showPlayer &&
			g_switchMode == SwitchMode::kAutomatic &&
			(reason == ViewReason::kSpeakerStarted ||
				reason == ViewReason::kDialogueChoiceConfirmed);

		if (nativeAutomaticNpcRelease) {
			g_playerViewRequested.store(
				false,
				std::memory_order_release);
			DisarmPlayerVisibilityGuard();
			CancelPendingPlayerShotExpression(
				"automatic native NPC release");

			REX::INFO(
				"Automatic NPC transition released after continuous speech lead; "
				"no controller writes, model writes, dialogue-handle writes, or camera rebuild");
			return;
		}

		const auto targetHandle =
			showPlayer ?
				GetPlayerHandle() :
				GetSpeakerHandle();
		if (targetHandle == 0) {
			REX::WARN(
				"Toggle aborted: selected target handle is zero");
			return;
		}

		g_playerViewRequested.store(
			showPlayer,
			std::memory_order_release);

		if (showPlayer) {
			DisarmPlayerVisibilityGuard();
		}

		WriteHandle(
			dialogueState,
			kRequestedTargetHandle,
			targetHandle);
		WriteHandle(
			dialogueState,
			kCurrentTargetHandle,
			targetHandle);

		if (showPlayer) {
			(void)ApplyCapturedControllerTarget(
				targetHandle,
				true);
		}

		const auto rebuild = GetRebuildFunction();
		if (!rebuild) {
			g_playerViewRequested.store(
				false,
				std::memory_order_release);
			REX::CRITICAL(
				"Could not resolve dialogue-camera rebuild function");
			return;
		}

		const auto transitionStartedAt = GetTickCount64();
		rebuild(dialogueState, true);
		const auto rebuildDurationMs =
			GetTickCount64() - transitionStartedAt;

		if (showPlayer) {
			ArmPlayerVisibilityGuardForShot();
			SchedulePlayerShotExpression();
		} else {
			CancelPendingPlayerShotExpression(
				"NPC camera selected");
		}

		REX::INFO(
			"Rebuild returned; requested view={}, reason={}, duration={}ms",
			showPlayer ? "player" : "NPC",
			DescribeViewReason(reason),
			rebuildDurationMs);
	}


	void SetVisualIsolatedView(
		const bool showPlayer,
		const ViewReason reason)
	{
		if (!IsVisualIsolatedMode() ||
			!g_dialogueOpen ||
			!IsDialogueMenuOpen() ||
			g_shipDialogueBypass.load(std::memory_order_acquire)) {
			return;
		}

		const bool currentPlayerView =
			g_playerViewRequested.load(std::memory_order_acquire);
		if (currentPlayerView == showPlayer) {
			return;
		}

		g_playerViewRequested.store(
			showPlayer,
			std::memory_order_release);
		g_visualCameraApplyLogCount.store(0, std::memory_order_release);
		g_visualCameraFailureLogCount.store(0, std::memory_order_release);

		if (showPlayer) {
			ArmPlayerVisibilityGuardForShot();
			SchedulePlayerShotExpression();
		} else {
			DisarmPlayerVisibilityGuard();
			CancelPendingPlayerShotExpression(
				"visual-isolated NPC view");
		}

		REX::INFO(
			"Visual-isolated view={}, reason={}; native dialogue target remains NPC",
			showPlayer ? "player" : "NPC",
			DescribeViewReason(reason));
	}


	void QueueView(
		const bool showPlayer,
		const ViewReason reason)
	{
		if (!g_taskInterface) {
			REX::CRITICAL("SFSE TaskInterface is unavailable");
			return;
		}

		bool expected = false;
		if (!g_actionQueued.compare_exchange_strong(
				expected,
				true,
				std::memory_order_acq_rel)) {
			REX::WARN(
				"A dialogue-camera toggle is already queued");
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

		QueueView(
			showPlayer,
			ViewReason::kManual);
	}

	// Choice-confirm pre-switching is intentionally disabled. Polling input and
	// retargeting during response acceptance proved less reliable than leaving the
	// established player shot untouched until NPC speech has been continuous for
	// the configured safety window.

	void UpdateAutomaticCamera()
	{
		const bool visualIsolated = IsVisualIsolatedMode();
		if ((g_switchMode != SwitchMode::kAutomatic && !visualIsolated) ||
			!g_dialogueOpen ||
			g_shipDialogueBypass.load(std::memory_order_acquire) ||
			g_bodyPreparationPending ||
			(!visualIsolated &&
				g_actionQueued.load(std::memory_order_acquire))) {
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

		// Returning to the player still requires stable silence so pauses between
		// ordered fragments do not produce rapid visual cuts. Legacy automatic mode
		// retains the conservative 2-second native-release delay. Visual-isolated
		// mode never mutates native dialogue state, so it can return to the already
		// active native NPC shot using the ordinary short NPC delay.
		const std::uint32_t requiredDelay =
			desiredPlayerView ?
				std::max(g_autoPlayerDelayMs, g_autoNpcHoldMs) :
				(visualIsolated ?
					g_autoNpcDelayMs :
					std::max(g_autoNpcDelayMs, g_autoNpcSpeechLeadMs));

		if (now - g_autoCandidateSince < requiredDelay) {
			return;
		}

		const bool currentPlayerView =
			g_playerViewRequested.load(
				std::memory_order_acquire);

		if (currentPlayerView == desiredPlayerView) {
			return;
		}

		if (!desiredPlayerView) {
			if (visualIsolated) {
				REX::INFO(
					"Visual-isolated NPC speech remained continuous for {}ms; "
					"removing only the render-camera override",
					requiredDelay);
			} else {
				REX::INFO(
					"Automatic NPC speech remained continuous for {}ms; "
					"releasing player shot through Starfield native update",
					requiredDelay);
			}
		}

		const auto reason =
			desiredPlayerView ?
				ViewReason::kSpeakerStopped :
				ViewReason::kSpeakerStarted;
		if (visualIsolated) {
			SetVisualIsolatedView(
				desiredPlayerView,
				reason);
		} else {
			QueueView(
				desiredPlayerView,
				reason);
		}
	}

	void Tick()
	{
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

		auto* const playerCamera = RE::PlayerCamera::GetSingleton();
		const bool currentShipCamera = IsShipPilotingCamera(playerCamera);
		const bool currentShipHud = IsShipHudOpen(ui);

		if (!dialogueOpen && playerCamera &&
			!playerCamera->QCameraEquals(RE::CameraState::kDialogue)) {
			g_lastGameplayWasShipCamera = currentShipCamera;
			g_lastGameplayHadShipHud = currentShipHud;
		}

		const bool dialogueOpening = dialogueOpen && !g_dialogueOpen;
		const bool genericFirstPerson =
			IsGenericFirstPersonCamera(playerCamera);
		const float openingSpeakerDistance =
			dialogueOpening ? GetOpeningDialogueSpeakerDistance() : -1.0F;
		const bool remoteFirstPersonDialogue =
			dialogueOpening &&
			genericFirstPerson &&
			openingSpeakerDistance >= kRemoteShipDialogueDistance;

		const bool openingShipDialogue =
			dialogueOpening &&
			(currentShipCamera ||
				currentShipHud ||
				g_lastGameplayWasShipCamera ||
				g_lastGameplayHadShipHud ||
				remoteFirstPersonDialogue);

		if (dialogueOpen &&
			!g_shipDialogueBypass.load(std::memory_order_acquire) &&
			!openingShipDialogue) {
			const auto speakerHandle = GetSpeakerHandle();
			g_dialogueSpeakerHandleForHooks.store(
				speakerHandle,
				std::memory_order_release);

			if (IsVisualIsolatedMode()) {
				const auto now = GetTickCount64();
				const bool speakerChanged =
					speakerHandle != g_visualRootSpeakerHandle;
				const bool rootsMissing =
					g_visualPlayerRoot.load(std::memory_order_acquire) == 0 ||
					g_visualSpeakerRoot.load(std::memory_order_acquire) == 0;
				if (speakerChanged ||
					rootsMissing ||
					now >= g_nextVisualRootRefreshAt) {
					g_visualPlayerRoot.store(
						reinterpret_cast<std::uintptr_t>(
							GetPlayerRoot3D()),
						std::memory_order_release);
					g_visualSpeakerRoot.store(
						reinterpret_cast<std::uintptr_t>(
							GetReferenceRoot3D(
								GetSpeakerReference())),
						std::memory_order_release);
					g_visualRootSpeakerHandle = speakerHandle;
					g_nextVisualRootRefreshAt = now + 250;
				}
			}
		}

		if (dialogueOpen != g_dialogueOpen) {
			g_dialogueOpen = dialogueOpen;

			if (dialogueOpen) {
				if (openingShipDialogue) {
					// Cockpit hails can arrive as generic first person with the ship HUD
					// already hidden. Normalize that presentation immediately, before any
					// PointCameraAtPlayer dialogue/body/camera logic is allowed to run.
					// We intentionally do not restore cockpit view on hang-up; this mirrors
					// the normal NPC-dialogue policy of leaving gameplay in third person.
					if (genericFirstPerson && playerCamera) {
						playerCamera->ForceThirdPerson();
						REX::INFO(
							"Cockpit hail normalized to third person before ship-dialogue bypass; "
							"speakerDistance={:.1f}",
							openingSpeakerDistance);
					}

					g_shipDialogueBypass.store(
						true,
						std::memory_order_release);
					g_dialogueActiveForHooks.store(
						false,
						std::memory_order_release);
					g_dialogueSpeakerHandleForHooks.store(
						0,
						std::memory_order_release);
					g_playerViewRequested.store(
						false,
						std::memory_order_release);
					g_bodyPreparationPending = false;
					g_bodyPreparationResumeAt = 0;
					g_bodyPreparationDeadlineAt = 0;
					g_actionQueued.store(false, std::memory_order_release);
					ResetAutomaticState();
					DisarmPlayerVisibilityGuard();
					g_visualNativeTransformSaved.store(
						false,
						std::memory_order_release);
					g_visualPlayerRoot.store(0, std::memory_order_release);
					g_visualSpeakerRoot.store(0, std::memory_order_release);

					REX::INFO(
						"Ship-piloting DialogueMenu detected; full camera/model bypass active "
						"(shipCamera={}, shipHud={}, previousShipCamera={}, previousShipHud={}, "
						"genericFirstPerson={}, remoteSpeaker={}, speakerDistance={:.1f})",
						currentShipCamera ? "yes" : "no",
						currentShipHud ? "yes" : "no",
						g_lastGameplayWasShipCamera ? "yes" : "no",
						g_lastGameplayHadShipHud ? "yes" : "no",
						genericFirstPerson ? "yes" : "no",
						remoteFirstPersonDialogue ? "yes" : "no",
						openingSpeakerDistance);
				} else {
					g_shipDialogueBypass.store(
						false,
						std::memory_order_release);
					g_dialogueActiveForHooks.store(
					true,
					std::memory_order_release);
					g_bodyPreparationPending = false;
				g_bodyPreparationResumeAt = 0;
				g_bodyPreparationDeadlineAt = 0;
				g_lastFunExpressionIndex = kExpressionCount;
				CancelPendingPlayerShotExpression(
					"dialogue-open reset");

				if (g_forceThirdPersonOnDialogue) {
					BeginDialogueOpenBodyPreparation();
				} else if (g_forceSheatheOnDialogue) {
					(void)SheathePlayerWeapon(
						"dialogue-open");
				}

				g_playerViewRequested.store(
					false,
					std::memory_order_release);
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
				DisarmPlayerVisibilityGuard();
				g_visualNativeTransformSaved.store(
					false,
					std::memory_order_release);
				g_visualCameraApplyLogCount.store(
					0,
					std::memory_order_release);
				g_visualCameraFailureLogCount.store(
					0,
					std::memory_order_release);
				g_nextVisualRootRefreshAt = 0;
				g_visualRootSpeakerHandle = 0;

					REX::INFO(
						"DialogueMenu opened; speaker=0x{:08X}, player=0x{:08X}",
						GetSpeakerHandle(),
						GetPlayerHandle());
				}
			} else {
				const bool wasShipDialogue =
					g_shipDialogueBypass.exchange(
						false,
						std::memory_order_acq_rel);

				if (wasShipDialogue) {
					g_dialogueActiveForHooks.store(false, std::memory_order_release);
					g_dialogueSpeakerHandleForHooks.store(0, std::memory_order_release);
					g_playerViewRequested.store(false, std::memory_order_release);
					g_bodyPreparationPending = false;
					g_bodyPreparationResumeAt = 0;
					g_bodyPreparationDeadlineAt = 0;
					g_actionQueued.store(false, std::memory_order_release);
					ResetAutomaticState();
					DisarmPlayerVisibilityGuard();
					g_visualNativeTransformSaved.store(false, std::memory_order_release);
					g_visualPlayerRoot.store(0, std::memory_order_release);
					g_visualSpeakerRoot.store(0, std::memory_order_release);
					g_expressionActionQueued.store(false, std::memory_order_release);
					g_capturedController.store(0, std::memory_order_release);
					g_capturedControllerTarget.store(0, std::memory_order_release);

					REX::INFO(
						"Ship dialogue closed; bypass ended with native camera/model teardown untouched");
				} else {
				// If DialogueCameraState does not receive another teardown update,
				// restore the last native camera transform here before gameplay resumes.
				RestoreSavedNativeCameraTransform();
				g_dialogueActiveForHooks.store(
					false,
					std::memory_order_release);
				g_dialogueSpeakerHandleForHooks.store(
					0,
					std::memory_order_release);
				CancelPendingBodyPreparation(
					"dialogue-close");
				g_playerViewRequested.store(
					false,
					std::memory_order_release);
				CancelPendingPlayerShotExpression(
					"dialogue-close");
				ClearPlayerExpressionOverride(
					"dialogue-close");
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
				DisarmPlayerVisibilityGuard();
				g_visualPlayerRoot.store(
					0,
					std::memory_order_release);
				g_visualSpeakerRoot.store(
					0,
					std::memory_order_release);
				g_visualCameraApplyLogCount.store(
					0,
					std::memory_order_release);
				g_visualCameraFailureLogCount.store(
					0,
					std::memory_order_release);
				g_nextVisualRootRefreshAt = 0;
				g_visualRootSpeakerHandle = 0;

					REX::INFO(
						"DialogueMenu closed; native model and camera teardown left untouched");
				}
			}
		}

		if (dialogueOpen &&
			g_shipDialogueBypass.load(std::memory_order_acquire)) {
			// Absolute spectator mode for ship communications. Do not run any
			// camera/body/visibility/expression logic or poll mod hotkeys.
			g_vWasDown = false;
			g_expressionWasDown = false;
			return;
		}

		UpdatePendingBodyPreparation();
		UpdatePlayerVisibilityGuardCache();
		UpdateAutomaticCamera();
		UpdatePendingPlayerShotExpression();

		if (!dialogueOpen) {
			g_vWasDown = false;
			g_expressionWasDown = false;
			return;
		}

		const bool vIsDown =
			(GetAsyncKeyState(g_manualSwitchKey) & 0x8000) != 0;
		if (g_switchMode == SwitchMode::kManual &&
			vIsDown &&
			!g_vWasDown) {
			QueueManualToggle();
		}
		g_vWasDown = vIsDown;

		const bool expressionIsDown =
			(GetAsyncKeyState(g_expressionCycleKey) & 0x8000) != 0;
		if (expressionIsDown &&
			!g_expressionWasDown) {
			QueueExpressionCycle();
		}
		g_expressionWasDown = expressionIsDown;
	}

}

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	// Two CALL5 hooks plus one VFT hook use the existing trampoline budget.
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
		"PointCameraAtPlayer 1.18.14 cockpit third-person hail normalization build loaded");
	REX::WARN(
		"Experimental build for Starfield 1.16.244.0 only");

	g_taskInterface = SFSE::GetTaskInterface();
	if (!g_taskInterface) {
		REX::CRITICAL("SFSE TaskInterface is unavailable");
		return false;
	}

	g_taskInterface->AddPermanentTask(Tick);

	REX::INFO(
		"Visual-camera isolation remains enabled for ordinary actor dialogue. "
		"Dialogue opened from a ship-piloting camera, active ship HUD, or remote first-person hail is now normalized/bypassed: "
		"no body preparation, camera override, visibility guard, expression handling, "
		"dialogue-target write, or controller write is performed during ship comms.");

	return true;
}