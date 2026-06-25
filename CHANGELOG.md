# Changelog
All notable changes to this project will be documented in this file.

# Release 4.0.0-beta.24
- Added MCP (Model Context Protocol) support on Unreal Engine 5.8, letting AI coding agents work directly in the editor.
- Added a UI workflow to set up MCP and connect an AI coding agent (Claude Code, Cursor, VS Code, Gemini, or Codex).
- Added the Convai Toolset: AI-callable editor actions so an agent can set up and extend Convai characters instead of hand-editing Blueprints.
  - SetupConvaiCharacter, SetupConvaiPlayer, and SetupConvaiPawnMovement wire a MetaHuman and the player for conversation and navigation; AddNavMeshVolumeForCurrentLevel makes the level navigable.
  - AddConvaiAction (typed and fixed-choice parameters) and CreateConvaiActionHandler declare actions and generate their handler events.
  - SetBlueprintPropertyAndPropagate edits a component- or actor-level property and pushes the change onto characters already placed in the level.
- Added Convai AgentSkills: an in-editor knowledge base an agent reads to use Convai correctly, covering project and character setup, conversation, actions, animation, dynamic context, gaze, scene awareness, narrative, player input, vision, and expressiveness.
- Kept the plugin compiling across UE 5.0-5.8; the new AI tooling is editor-only with no runtime dependency and activates on 5.8+.


# Release 4.0.0-beta.23-hotfix
- Packaging: ship the plugin to Fab as a source-only submission (no Unreal-generated Binaries/Intermediate), matching Fab's required plugin file structure.
- Packaging: dropped the precompiled "Installed" flag from the plugin descriptor so Fab compiles the plugin from source for each engine version.
- Packaging: every module now declares a PlatformAllowList (Convai and ConvaiVisionBase: Win64, Android; ConvaiEditor and ConvaiAnimGraph: Win64), as required by Fab.

# Release 4.0.0-beta.23
- Added support for Unreal Engine 5.8.
- Fixed an Android microphone capture issue.
- Improved facial-animation stability with thread-safe access to blendshape data.

# Release 4.0.0-beta.22
- Added a spatial-awareness subsystem that gives each chatbot a continuous sense of its surroundings, delivered to the LLM as plain-language facts.
  - Describes nearby objects, other characters, and the player by distance band, direction, and facing, gated by line of sight.
  - Adds inter-entity relations (e.g. "on top of the pressure plate", "to the left of the fountain"), anchored to the nearest higher-priority neighbour.
  - Adds a master enable plus line-of-sight, distance-band, and relation controls under Project Settings > Plugins > Convai Spatial Awareness, with per-chatbot Surroundings / Relations toggles.
  - Replaces the old per-object proximity mechanism, which has been retired.
- Added the ability to merge identically-named Convai Object components into a single logical object so a pile of crates or a row of plates reads to the chatbot as one thing.
  - Per-component opt-in (Merge With Same-Named Objects) with a Merge Group Index to keep separate sets of the same name apart.
  - Merged objects expose one position, one description, and one collective gaze highlight that lights up the whole set.
  - Added a global Duplicate Name Suffix Style setting (Numeric or Alphabetical) for disambiguating non-merged same-named objects.
- Added ephemeral context events: Add Context Event gains a bEphemeral option that nudges the AI exactly once on the next update without persisting it.
  - SetObjectInAttention now emits a one-flush "is paying attention to <object>" cue by default (toggleable), so gazing at or attending an object tells the AI once without leaving a lingering state.
- Added FConvaiAvatarInfo: the get-characters request now returns full character details (name, voice, model details, traits, emotions, actions) instead of only character IDs.
- Improved character navigation in the example Blueprints: the character now skips a move to an object it already knows is unreachable instead of failing into it.
- Fixed the pointing animations so they play and aim correctly from LLM-issued point and point-at actions.
- Fixed step-onto arrival so a chatbot is correctly treated as having reached a wide object once it stands on the object's footprint, instead of never registering arrival for objects wider than its acceptance radius.
- Fixed spatial-awareness and duplicate-name settings being ignored at runtime: toggling them in Project Settings now takes effect live without an editor restart.
- Fixed one-flush ephemeral context events being lost when the chatbot was disconnected; the dynamic-context flush is now deferred until reconnect so the event still goes out on the first flush.
- Improved SetObjectInAttention diagnostics: it now logs a warning explaining why a call had no effect (actions disabled on the chatbot, the object not in the environment list, or the object not in the connect-time action config).
- Fixed an audio resampler divide-by-zero that could occur when an invalid sample rate was reported.
- Fixed the Convai editor update check so it correctly detects V4 releases.
- Fixed Convai editor theme initialization.
- Fixed the YouTube latest-video thumbnails shown in the Convai editor.
- Removed leftover debug logic from the bundled example Blueprints.

# Release 4.0.0-beta.20
- Overhauled the bundled MetaHuman animations.
- Added gesture and pointing animations that are triggered automatically from LLM-issued actions.
- Added Convai Object Component for tagging actors and exposing their properties as live context to the chatbot.
- Added gaze-driven attention: the actor under the player's gaze becomes the object in attention, with a silhouette highlight and an on-screen cursor.
- Added proximity-driven state: object components report when they are reachable from a chatbot's pawn so the bot can react.
- Added replicated PointAtTarget alongside LookAtTarget on the chatbot component.
- Added "Create Convai Action Handler" right-click entry in Blueprint graphs for scaffolding typed action handlers.
- Added "Setup Convai Pawn Movement" content-browser action that seeds navigation defaults on any pawn Blueprint.
- Added component and socket targeting plus per-entry movement overrides for actions.
- Improved action completion: handlers can wait for bot speech to finish before the next action, post a dynamic-context event in the same call, and abort an unrecoverable sequence cleanly.
- Added fuzzy matching for enum and choice action parameters so minor LLM spelling drift no longer drops the action.
- Added VAD (voice activity detection) parameters in project settings and the connect request.
- Added Convai auth caching so subsequent connects don't re-authenticate from scratch.
- Added GetBodyAndFaceSkeletalMeshComponents helper for resolving MetaHuman and CC5 rigs.
- Improved tail-of-utterance audio handling to reduce clipping on soft trailing consonants.
- Improved lip-sync robustness with a starvation fallback when frames briefly stop arriving.
- Enabled actions on by default in the chatbot environment.
- Switched the action wire format to `{name}` braces with more robust response parsing.
- Fixed microphone hot-swap so switching device mid-session no longer requires restarting the stream.
- Fixed Android crash on app shutdown.
- Fixed several UE 5.0 / 5.4 / 5.5 / 5.7 compilation and compatibility issues so the plugin builds clean across all supported engine versions.
- Disabled the ConvaiEditor module on UE 5.1 and earlier (the property-binding editor feature it relies on isn't available there).
- Regenerated the gaze-highlight material under UE 5.0 so the asset loads across all supported engine versions.

# Release 4.0.0-beta.19
- Actions integration with WebRTC: chatbot now sends `action_config` at /connect when `bEnableActions` is on, and parses structured `action-response` payloads (`{name, target?}`) into typed action sequences.
- Refactored `Environment` from `UConvaiEnvironment` UObject to `FConvaiEnvironmentData` USTRUCT. Granular Add/Remove/Clear methods on the chatbot for Objects, Characters, Actions; debounced `update-scene-metadata` flush; `bFlushImmediately` advanced opt-in for time-critical updates.
- Split legacy `MainCharacter` into `ConversationPartner` (replicated, scene-aware) and `LookAtTarget` (animation-only). New `bAutoFillConversationPartnerFromPlayer` toggle (defaults on) populates the partner from the first registered Convai Player Component or pawn 0.
- New `SetObjectInAttention(Entry, Text, ShouldRespond, bFlushImmediately)` rides the dynamic-context flush; auto-registers the entry in `Environment.Objects` if missing.
- Added `bEnableActions` chatbot toggle. `Environment.Actions` defaults seeded with `Move To`, `Follow`, `Stop Moving`, `Wait For`.
- Backward compat: legacy `UConvaiEnvironment` and `UConvaiActionContext` resurrected as deprecated shims so older BP graphs that did `Chatbot->Environment->XxxMethod()` keep compiling with deprecation warnings.
- **Actions V2 — typed action templates:** `FConvaiAction` with structured `Parameters` (typed via `EConvaiActionParamType`: Auto / Reference / String / Number / Bool / Enum). Optional `Connector` for compound actions (`Put ball on table`), `Choices` for fixed-list constraints, or `EnumType` to draw choices from a `UENUM`.
- Live two-way editable wire-format preview on `FConvaiAction.RenderedString` — edit either the structured fields or the rendered string and the other side syncs (parse failures silently revert).
- Unified action-result shape: `FConvaiResultAction.Parameters: TMap<FString, FConvaiResultParam>`. Each `FConvaiResultParam` populates `StringValue` / `NumberValue` / `BoolValue` / `RefValue` / `ByteValue` best-effort regardless of declared `Type`. Legacy `RelatedObjectOrCharacter` and `ConvaiExtraParams` kept as deprecated mirrors.
- New BP-pure accessors: `Get First Param`, `Get Param`, `Get Param Type`, `Get Param As String/Number/Bool/Ref/Byte`, `Has Param`. Old `Get Action Param` family kept and tagged deprecated.
- `HandleActionCompletion` gains optional `EventText` and `ShouldRespond` so handlers can post a dynamic-context event in the same call. New `AbortActionSequence(EventText, ShouldRespond)` clears the queue without retrying — for unrecoverable failures where the bot should replan.
- Removed the `EnableNewActionSystem` global toggle / `IsNewActionSystemEnabled` helper — the action queue path is now always on.
- Numeric / quoted-text param extraction out of the action target (`"5 seconds"` → `Number=5`; `"hello"` → `Text="hello"`).
- New **Setup Convai Pawn Movement** content-browser action: right-click any Actor-derived Blueprint asset → **Convai → Setup Convai Pawn Movement**.
  - Reparents pure `Actor` Blueprints to `APawn` and adds a `FloatingPawnMovement` component with Convai-tuned defaults (max speed 375, acceleration 200, deceleration 250, turning boost 3).
  - For `Pawn` Blueprints, adds `FloatingPawnMovement` if missing.
  - For `Character` Blueprints, tunes the existing `CharacterMovementComponent` (max walk speed, max acceleration, braking deceleration, nav-agent props).
  - Logs a warning when the parent class is an Actor subclass that doesn't eventually inherit from `APawn`/`ACharacter` so users can reparent and re-run.
- New developer docs at `Convai/Docs/ActionsAndEnvironment.md` and `Convai/Docs/ActionsV2.md`; four-page GitBook walkthrough under `Convai/Docs/GitBook/`.

# Release 4.0.0-beta.18
- Added Android packaging support.
- Added dynamic context batching and logging for chatbot interactions to reduce redundant updates.
- Added starvation blending and playable frame checks for improved lip sync stability.
- Added command-line overrides for lip sync animation parameters and simulation freeze functionality.
- Added custom parameter handling and client version retrieval to ConvaiUtils.
- Added reset idle timer and user idle warning support.
- Enhanced component resolution in FaceSync Animation Node to include parent actor when searching for UConvaiChatbotComponent.
- Renamed RunLLM parameter to ShouldRespond for improved API clarity in context update methods.
- Streamlined context state updates to always use debounce window for consistency.
- Updated LipSyncTimeOffset to 200ms for better audio/animation alignment.
- Updated ContextAggregationDelay from 0.3s to 0.5s for improved dynamic context handling.
- Refactored narrative trigger handling to streamline context processing.

# Release 4.0.0-beta.17
- Optimized Lipsync computational performance in Metahuman Face Animation Blueprint.
- Improved audio playback time estimation and removed deprecated FSoundSource caching.
- Enhanced audio finish detection with tolerance to avoid floating-point rounding edge cases where playback would never be marked as finished.
- Added performance timing statistics for audio streamer and face sync components in debug mode.
- Added detailed audio playback debug logging (playback time, wall clock, byte-based time, remaining duration, ring buffer state).
- Improved audio capture cleanup to ensure safe resource release during component destruction and end play.
- Fixed potential dangling references in async tasks by switching to weak pointers.
- Fixed re-entrant callbacks during Convai client disconnection.
- Fixed crash caused by cross-object delegate cleanup in BeginDestroy for chatbot and player components.
- Fixed player transcriptions not being appended properly.
- Added emotions provider plumbing.
- Fixed ConvaiConnectionConfig struct layout mismatch with DLLs by removing emotion_provider from the shared struct and resolving struct offset issues.

# Release 4.0.0-beta.16
- Support Android.

# Release 4.0.0-beta.15
- Fixed lipsync slow receival.

# Release 4.0.0-beta.14
- Added Update Context functionality.
- Updated OutputFPS to 60 for improved lipsync output.
- Improved lipsync audio handling and cleaned up unused variables.
- Improved audio playback time calculation accuracy and simplified play voice data flow.

# Release 4.0.0-beta.13
- Added connection indicator UI widget.
- Fixed attendee disconnect state notification handling.
- Fixed editor UI to show changelog only for installed plugin version.
- Improved connection manager reliability and state handling.
- Fixed audio not being sent when connection is in orphan state.
- Improved server event dispatching.
- Enhanced logging for better debugging.

# Release 4.0.0-beta.12
- Deprecated old ConvaiBaseCharacter and ConvaiBasePlayer BP Classes.
- Added connection indicator widget to show the connection state.
- Added LLM start/stop events (OnLLMStarted, OnLLMStopped) to ConvaiChatbotComponent.
- Added emotions support.
- Added toggle STT functionality.
- Improved lip sync at the end of response.
- Fixed lip sync desync corner case due to taking frames from previous response.
- Fixed PIE crash when recording audio at the same time of starting a new connection.
- Fixed minimum buffer duration updated to 0.2 seconds.
- Enhanced GetTestCharacterID to trim whitespace and support command line overrides.

# Release 4.0.0-beta.11
- Improved compatability with CC5 Reallusion characters.

# Release 4.0.0-beta.10
- Fixed DLL loading issue
- Fixed crash in the Convai Dashboard
- Added support for Blueprint-only projects

# Release 4.0.0-beta.9
- Improved character interrupt handling.
- Improved lipsync accuracy.
- Added connection state functions to ConvaiSubsystem and ConvaiChatbotComponent.

# Release 4.0.0-beta.7
- Fixed corruption of some transcription characters.

# Release 4.0.0-beta.6
- Added ARKit blendshapes lipsync support.
- Updated default LipSyncMode to BS_MHA (MetaHuman) in ConvaiFaceSyncComponent.
- Added RequiresPrecomputedFaceData method to ConvaiConnectionInterface.
- Fixed blendshape selection logic to use ARKit names conditionally.

# Release 4.0.0-beta.5
- Updated BlendShapesNames to new naming convention for ARKit.

# Release 4.0.0-beta.4
- Implemented bulk neurosync blendshape handling with stats logging.
- Added audio frame tracking and improved voice handling in ConvaiAudioStreamer and ConvaiChatbotComponent.
- Implemented blendshape frame count verification and logging for improved analytics.
- Added end user ID functionality and device unique ID support.
- Fixed voice playback overlap by stopping voice when audio starts.
- Removed redundant checks in StopVoice function and stopped broadcasting blendshape event on StopLipSync.
- Fixed ConvaiLipSync initialization check in GetLipSyncMode.
- Removed unnecessary CalculateStartingTime call in ConvaiApplyPrecomputedFacialAnimation.
- Added MetaHumanCtrlNames support.
- Fixed editor freezing issues.
- Handled neurosync blendshapes in face sync component.
- Fixed Linux library linking issues.
- Increased audio content check delay from 0.3s to 0.5s.
- Fixed audio not playing when MinBufferDuration was higher than received audio.
- Vision optimization improvements.
- Set default MinBufferDuration value.

# Release 4.0.0-beta.3
- Added 5.7 support
- Added Blueprint only project support

# Release 4.0.0-beta.2
- Added Editor Login Screen and Dashboard.
- Added better ehco and noise cancellation.
- Added Initial Viseme based lipsync support using FaceSync component.
- Added Gamma correction to vision.
- Fixed the F10 Settings menu.

# Release 4.0.0-beta.1
- Initial webrtc integration.

# Release 3.6.7-Beta
- Add UseSystemCertificates option for SSL configuration on Windows.
- Fixed Convai logging not showing after packaging.
- Fixed multiplayer crash due to sending text data over non-game thread.

# Release 3.6.6
- Add EnableSync parameter for audio and lip sync synchronization control.

# Release 3.6.5
- Adjust MinBufferDuration and AudioLipSyncRatio defaults to reduce audio stutters.

# Release 3.6.4
- Improve data validation in ConvaiAudioStreamer to prevent crashes from invalid input.
- fixed MinBufferDuration not working as expected when lipsync is active.

# Release 3.6.3
- Improve memory management in ConvaiGRPC.

# Release 3.6.2
- Potential fix for missing audio for Elevenlabs and some of Azure voices.
- Improved Performance when receiving AI audio responses.

# Release 3.6.1
- Fix headers fr linux build.

# Release 3.6.0
- UE5.6 support.
- Optimized GetAllChatbotComponents and GetAllPlayerComponents function.
- Added Convai Logger.

# Release 3.5.4-hotfix-1
- Fixed racing condition when playing audio.
- Updated Android Play Core to 2.0.3.
- Refactored old unused classes and code.

# Release 3.5.4
- Further improved LipSync accuracy and synchornization.
- Fixed an issue where [player speech transcription would fail after short character responses](https://forum.convai.com/t/player-speech-transcription-fails-after-short-character-responses/3735/2).
- Improved gRPC connection stability to prevent conversation failures.
- Resolved a rare crash caused by the server sending large volumes of audio in small chunks.

# Release 3.5.4
- Further improved LipSync accuracy and synchornization.
- Fixed an issue where [player speech transcription would fail after short character responses](https://forum.convai.com/t/player-speech-transcription-fails-after-short-character-responses/3735/2).
- Improved gRPC connection stability to prevent conversation failures.
- Resolved a rare crash caused by the server sending large volumes of audio in small chunks.

# Release 3.5.3-beta
- Improved LipSync accuracy and synchornization.
- Fixed Voice cutting off for Elevenlab voices.
- Fixed PixelStreaming compatability.

# Release 3.5.2
- Fixed rare crash when new voice data starts to play when the character is already finishing its current sentence.

# Release 3.5.1
- Added Long Term Memory V0.
- Fixed voice not being sent to clients on Multiplayer.
- Added Dynamic Environment Info.

# Release 3.5.0
- UE5.5 support

# Release 3.4.2-beta
- Fixed player time out warning message when using text.
- Increased gRPC connection robustness by adding small delays and read retries.

# Release 3.4.1-beta
- Fixed IsListening() was not properly returning the character state.

# Release 3.4.0-beta
- Fixed multiple connection issues.
- Can now play custom montages over MetaHuman Face animation.

# Release 3.3.2
- Added intermediate fix for audio being cutoff in multiplayer.
- Added `ConvaiGetAvailableVoices` function.

# Release 3.3.1
- Fixed Narrative keys not being set properly.
- Better lip synchornization using timestampes.
- Added "Convai Download Image using RPM link" function.
- Exposed Convai Player component for c++ use. 

# Release 3.3.0
- Added Linux Support.
- Fixed issue where if an object is similar to a character name it would be picked up as the related object or character.
 

# Release 3.2.2-Beta
- Fixed NPC2NPC crash when interrupting the character early on.

# Release 3.2.1-Beta
- Fixed body gestures not animating while talking.
- Added [Narrative Trigger Keys](https://docs.convai.com/api-docs/plugins-and-integrations/unreal-engine/guides/narrative-design-keys)
- Improved emotion animations for MetaHumans.

# Release 3.2.0
- Added 5.4 support

# Release 3.1.4
- Added Pixel Streaming support and documentation.
- Added x86_64 Android support.
- Added Narrative Design helper functions
- Added more blueprint documentation.
- Improved action accuracy with objects and characters.
- Improved MetaHuman Facial and body Animation logic.
- Improved lipsync.
- Updated Reallusion Animation blueprint.
- Fixed issue when adding custom MetaHuman animations.
- Fixed crash with Unreal Engine 5.3 when there is a connection issue.
- Fixed multiplayer crash.
- Fixed rare crash when using Invoke Speech with bad connection.

# Release 3.0.1 Hotfix
- Fixed audio capture on Android would sometimes not capture properly.
- Fixed lipsync crash on Android and improved performance.
- Fixed rare crash on Android when a huge amount of audio is spoken by the character.
- Improved overall audio capture.
- Fixed `Update Character` node not updating language.
- Updated Convai ReadyPlayerMe plugin for improved head tracking.
- Fixed lipsync not working issue when having a player component along with the chatbot component in the same character blueprint.
- Added documentation guide for Mac microphone permissions issue.
- Fixed startup crash for packaged apps on MacOS UE5.3 due to microphone permissions.

# Release 3.0.0
**Added**
- New lipsync component FaceSync which no longer requires ConvaiOVRLipsync plugin.
- Mac Lipsync support.
- Emotions.
- Object in Attention.
- New Convai Chatbot Component functions:
    - Invoke Speech.
    - Invoke Narrative Design Trigger.
    - Force Set Emotion.
    - Reset Emotion State.
    - Get Emotion Score.
    - Get Talking Time Elapsed.
    - Get Talking Time Remaining.
    - Set Lipsync Component.
    - Clear Action Queue.
- New Convai Chatbot Component Events:
    - On Narrative Section Recevied.
    - On Emotion State Changed.
- ConvaiGetLookedAtObjectOrCharacter function
- Actor loses focus with player after predefined time.
- Move To and Follow Actions failures will trigger a character response.
- Environment replication for multiplayer usecases.

**Improved**
- MetaHuman body Animations, Facial expressions and eye look at.
- Realusion Animation blueprint, lipsync and eye look at.
- Settings widget UI.
- Actions accuracy.
- Audio capture for low frame rate.

**Fixed**
- Packaging issue if Convai Player Component was in the scene prior to packaging.
- Crash when many interactions were happening simultaneously.
- ConvaiBasePlayerWithVoiceActivation Beta class was only working for the first interaction.
- Overall plugin stability and warnings.

**Deprecated**
- ConvaiOVRLipsync plugin and component - Please delete the plugin from your project and use the new FaceSync component instead.

# Release 2.9.0
**Added**
- Unreal Engine 5.3 Support.

**Fixed**
- Actions with more than 1 word was sometimes ignored.
- Player name would show as Unknown in multiplayer at some cases.

# Release 2.8.0
**Added**
- New Action System. (Tutorial Coming Soon!)
- Functions: `Get All Convai Player Components` and `Get All Convai Chatbot Components`.

**Updated**
- Improved MetaHuman body Animations and Facial expressions.
- Better OVR Lipsync for MetaHuman.
- Better capturing of text input as an extra parameter in Actions.

**Fixed**
- Rare crash caused by UE grabage collection during lipsync.
- Rare crash (Editor only) caused by exiting play mode before the AI character has finished its response.
- Random Freeze that happens when talking to an AI character for the first time.
- Crash when using `Convai Speech to Text` function with an imported audio.
- MetaHuman rotating 90 degrees when in play mode.

# Release 2.7.0
**Added**
- Full multiplayer support with real-time AI avatar conversations and voice chat.
- Environment awareness for AI avatars to do actions on surrounding objects and other players in multiplayer.
- Player name customization feature.
- Toggle for switching between client and server API keys.
- Multiplayer support for animations: Reallusion, Ready Player Me, and Unreal Engine Metahumans.
- Chat UI with multiplayer chat replication.

**Updated**
- Three distinct modes for Chat UI added for a more intuitive user experience.

**Fixed**
- Game crash issue caused by curse words.
- Bug where non-English texts were not processed correctly.
- "Upstream Max Limit Exceeded" error when using the gRPC API.

# Release 2.6.1
**Added**
- High-Quality character voices support in multiplayer.

**Updated**
- Updated Documentation.
- Increased Actions accuracy.
- Replicate Chatbot events in multiplayer for easier integration in multiplayer apps.

**Fixed**
- LipSync lag.
- Interrupting a character in multilayer would sometimes not interrupt it on all other clients.

# Release 2.6.0
**Added**
- Mac Support.
- New MetaHuman body animations and Facial expressions.
- Reallusion characters support.

**Updated**
- Improved MetaHuman lipsync.

**Fixed**
- Sending non-English text won't get processed.
- Potential microphone issue when it gets detected as zero channel.

# Release 2.5.1
- Added official Android support to the plugin on the marketplace.
- Fixed issue with Behaviour Tree where unhandled actions were not sent to the character blueprint.
# Release 2.5.0
**Added**
- Font support for non-English languages.
- Updated UI.
# Release 2.4.0
**Added**
- Added Unreal Engine 5.2 Support.
- Added 'IsListening' function to detect when the character is actively listening to the player.
- Enhanced Mic Controls: Added the 'SetMicrophoneVolumeMultiplier' and 'GetMicrophoneVolumeMultiplier' functions to allow better control over audio settings. A mic gain slider has also been introduced for easy adjustments to the Mic widget.
- Extended Language Support: Increased Voice Capture Sample Rate to 16kHz, improving support for Korean Speech-To-Text.

**Updated**
- User Interface: The UI is now only initialized if the blueprint is controlled by a player, providing a smoother user experience.
- Blueprint Organization: Reorganized blueprints for easier navigation, complete with improved commentary for better understanding.
- Animation Tuning: The talking animation has been toned down to offer a more natural visual experience.

**Fixed**
- Player Component Acquisition: Resolved an issue where obtaining the player component in the mic widget was problematic.
- Packaging Issues: Addressed packaging issues with ConvaiOVRLipSync and a problem where fonts disappeared after packaging.
- Texture Display: Fixed a grey texture issue with the ReadyPlayerMe character in the demo project.
- Crash Issues: Fixed instances where crashes occurred due to logging an invalid variable and during mic recordings when no audio data was available.
- Component Initialization: Fixed ConvaiAudioComponent initialization errors.
- Missing Player Controller in the Demo Map.

# Release 2.3.0
**Added**
- **Beta** Android support.
- Audio permission on Android.
- Mini-Example demo project.
- Mic input device selection functionalities:
    - SetCaptureDeviceByName.
    - SetCaptureDeviceByIndex.
    - GetActiveCaptureDevice.
    - GetAvailableCaptureDeviceNames.
    - GetAvailableCaptureDeviceDetails.
    - GetCaptureDeviceInfo.
    - GetDefaultCaptureDeviceInfo.
- Mic settings menu widget blueprint with the `ConvaiBasePlayer` blueprint.
- language code and avatar image link outputs to the `ConvaiGetCharacterDetails` function.
- `Convai Download Image` function to download images.
- Convai attribution logo widget.

**Updated**
- Audio capturing by implementing `ConvaiAudioCaptureComponent` to replace `IVoiceModule` for audio capture.
- `glTFRuntime` in the MetaHuman demo project and `ConvaiReadyPlayerMe` plugin bundle.

**Fixed**
- C++ project error on restart.
- Microphone issues on some clients.
- Dark face for ReadyPlayerMe avatars.

# Release 2.2.0
**Added**
- `ConvaiPlayerWithVoiceActivation` blueprint which adds voice activation functionality instead of push-to-talk.

**Updated**
- MetaHuman face animation now looks more natural.
- MetaHuman lip-sync is smoothed out to avoid rough transitions.

# Release 2.1.2
**Added**
- Character voice interrupts with fading.
- `Interrupt Voice Fade Out Duration` variable to control voice fade duration and was added to the `Convai Chatbot` component.
- `Interrupt Speech` BP function to force a speech interruption, and was added to `Convai Chatbot` component.

**Fixed**
- Bug where the character would stop responding when being talked to via text then voice.
- Bug where voice was not being attenuated when traveling far away from the character.

**Updated**
- `ConvaiBasePlayer` blueprint to allow players to talk at their own pace.

# Release 2.1.1
**Fixed**
- Packaging issue due to out-dated MetaHuman animation blueprint.
- Rare crash on End play in the editor.

# Release 2.1.0
**Added**
- MetaHuman animation blueprints.
- demo map for Action API.
- compatibility with OVR lip-sync.
-  `OnVisemesReady` event and `GetVisemes` BP function to Convai Chatbot component.
- `GetAPIKey` and `SetAPIKey` BP functions.
- `IsTalking` and `IsProcessing` BP functions.
- `ConvaiGetLookedAtCharacter` BP function.
- Better logs and error messages.
- `Character Name`, `Backstory`, `Voice` and `ReadyPlayerMe link` variables to the chatbot component which are automatically populated every time a new `character ID` is set.
- Environment objects can now be set on the Convai chatbot component without the need to explicitly specify them in `Send Text` or `Start Talking` BP functions.

# Release 2.0.5
**Fixed**
- Conflicts with the Firebase blueprint library plugin.
- Session ID not updating properly.
- Bug where stop talking won't be called as intended.

# Release 2.0.4
**Fixed**
- Warning due to improper initialization of `OptionalPositionVector` variable in the `ConvaiAction` structure.
- Packaging issue on UE 5.1 due to conflicts with the gRPC library.

# Release 2.0.3
**Fixed**
- Bug where stop talking wont be called as intended.

# Release 2.0.2
**Added**
- Backward compatibility with projects that implemented V1.x of the plugin.

# Release 2.0.1
**Fixed**
- Editor crash with gRPC.

# Release 2.0.0
**Added**
- gRPC streaming for a huge latency reduction.
- Convai Chatbot and Player components.
- Various events and blueprint functions with the new components.

**Deprecated**
- Deprecated all functions that work off the REST API.

# Release 1.0.0
**Added**
- REST API blueprint functions for Convai.
