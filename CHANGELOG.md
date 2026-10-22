# Changelog
All notable changes to this project will be documented in this file.

# Unreleased

# Release 4.0.0-beta.33

## Cloud Avatars

- Missing files that you have already reviewed no longer get stuck in the save prompt when Unreal leaves empty placeholders for them. Unsaved assets still need to be saved before uploading.
- Avatars with built-in groom hair physics can now upload to the supported Unreal 5.8 Windows Avatar Studio runtime without copying its shared hair solvers or turning physics off.
- The uploader now loads Unreal's importer support needed by newer assets while cooking, fixing a packaging error with their saved import information.
- Copied scenes now rebuild their editor animation instances when avatar dependencies change, instead of keeping animation state tied to older Blueprint classes. The original scene and animation settings stay unchanged.
- Custom animation Blueprints now automatically include the Convai assets they reference when creating or updating an avatar, helping animations work the same way in Avatar Studio as in your project.
- Fixed source downloads asking Windows users to enable plugins intended for another platform, preventing installation from finishing.
- Retrying an environment update now remembers removed files if source control could not record their deletion on the first attempt.
- Fixed an environment update getting stuck on files that Unreal kept in use after the tool reloaded the previous scene to replace it. The tool now replaces the unloaded scene without loading its old dependencies again.
- Fixed uploads being blocked by Unreal's editor-only engine materials added while copying a level. Assets used by the uploaded scene are still checked and copied.
- Environment refresh checks out tracked files before replacing them, including writable Perforce files and records removed files for source-control deletion. If checkout is unavailable, it explains what to do and leaves the environment files intact.
- Improved upload logs to identify the asset holding an unresolved reference, including references stored as soft paths.

## API requests

- Added **Max API Retries** to the existing Connection settings. Safe API reads can make two additional attempts after temporary network or HTTP failures; set it to zero to disable retries. Session reconnect attempts remain a separate setting.
- API retries share one deadline and stop when cancelled or when the session changes. APIs that create, allocate, update or delete data remain ineligible unless their repeatability is explicitly established.
- Native subclasses of `UConvaiAPIBaseProxy` must be rebuilt with the updated plugin. See [API request retries](Docs/APIRequestRetries.md) for child policy and response-handler hooks.
- Every REST node fires exactly one outcome. The LTM nodes fire OnFailure once, not twice, without an API key or token; Create Speaker ID, Delete Speaker ID, Get LTM Status and Set LTM Status no longer send a request after rejecting empty input; Set Knowledge Bank Document Status with no document IDs fails instead of posting an empty body.
- Delete Knowledge Bank Document, Set Knowledge Bank Document Status and Set LTM Status pass the server's error body to OnFailure instead of an empty string.
- Speech To Text delivers the transcript decoded (non-ASCII characters, quotes and backslashes arrive as themselves) and keeps its last character. A 2xx reply with no `result` field fires OnFailure with the raw body.
- Fetch Narrative Sections and Fetch Narrative Triggers fire OnFailure with an empty list when a 2xx reply is not a JSON array; before, neither pin fired.
- The three deprecated Convai Chatbot Get Response nodes percent-encode every form value, so typed text with `&`, `%`, `+` or non-ASCII characters reaches the server as typed. The From Audio nodes deliver the classified tag with Voice Response off. A 2xx reply that is not a JSON object, has no `text` field, or has `"text": null` now fires OnFailure; an empty `text` still succeeds with a warning.
- Convai Download Image picks the decoder from the downloaded bytes, so a JPEG, PNG or BMP decodes whatever the URL ends in.
- Convai Get All Characters IDs, List Speaker IDs and Fetch Narrative Sections / Triggers skip a list entry that is not a JSON object instead of failing the call or delivering an empty record.
- The legacy REST nodes fire OnFailure for a missing file or a null Sound Wave instead of crashing, and a stereo 16-bit file with no whole frame yields an empty result instead of a bare header.
- **Stereo To Mono** returns the input unchanged when a WAV chunk header claims more bytes than the file holds, instead of reading past its end.

## Build

- C++ projects no longer recompile the plugin. Development Editor builds now use the editor binaries a compiled release ships with, when the release matches your Unreal Engine version. Packaging, DebugGame Editor and source-built engines still compile the plugin from source.
- To build a compiled release from source, for example after changing its C++, delete `"Installed": true` from `ConvAI.uplugin`.
- While the shipped binaries are in use, the debugger may not find the plugin's source files, because the shipped symbols point at the build machine's paths.

## Objects

- A tracked property whose type is a Blueprint enum now reaches characters as the entry's name, such as `Open`, instead of `NewEnumerator1`. Get Context State Value and Watch Property targets use the same names, so State Value Descriptions written with those names now match. Native C++ enums are unchanged.
- Spawning actors with a Convai Object Component in quick succession, or calling **Set Convai Object Enabled** more often than the poll interval, no longer stops tracked properties, movement and spatial facts from updating. Each registration used to restart the poll.
- An un-aliased tracked property inside a Blueprint struct reaches characters by the member's name (`Door.Stats.Health`, not `Door.Stats.Health_12_<GUID>`), the same in the editor and in packaged builds; two members that would share a name keep their full stored names. State keys, Watch Property targets and Get Context State Value use the new name; set an **Alias** for a fixed key.
- Seeding tracked-property values no longer fires an armed **Watch Property**. This includes re-seeding a merged set's surviving member when another member is destroyed or disabled.
- A Convai Object re-enabled after its name came free takes that name instead of keeping its old numbered suffix ("Crate 2"), which could duplicate another object's name.
- An entry added to the chatbot's Environment objects other than through **Add Object** is set up at Begin Play or by an explicit Ensure call, not by the next Add Object.
- Spatial awareness: the reachability budget counts pathfinds (a merged set costs one per member, Movement Points one per point), waiting checks are served oldest first, and a subject hidden by line of sight keeps its last walking verdict.

## Dynamic context

- Update Context text is kept and re-sent at the front of every managed context update, so a later Set Context State or action change no longer erases it. Text sent again, for example the same line every session, is kept once, at the position where it was last sent.
- Dynamic Environment Info now travels in the managed context update (`update-dynamic-info` is no longer sent). It is cut at 15,999 bytes with one warning; the property keeps its full value. The event history keeps its newest 32,000 bytes.
- The first context update of a session is sent on the character's first tick after the session becomes ready instead of about 0.5 s later. Narrative triggers queued before ready also fire then.
- A context update that would pass the 64,000-byte transport limit is trimmed as it is built: oldest events first, then the end of Dynamic Environment Info, then the oldest Update Context lines. States, facts and the action block are kept, and one warning says what was left out.
- **Set Context Fact** with the sentence already set sends nothing.

## Lip Sync

- CC4 Extended lip sync now names its curves with CC4 names, such as `Mouth_Drop_Lower`, so an anim graph that reads curves by name moves a CC4 rig. Before, frames were named with MetaHuman `CTRL_expressions_*` or ARKit names, picked by the frame's size. A project that remapped those names to CC4 curves with a Blendshape Mapping must remove that mapping. This applies when the project's **Lip Sync Mode** (Project Settings ▸ Plugins ▸ Convai) is **Auto**, the default, and the character's Convai Face Sync is set to **CC4 Extended Blendshapes**. A project-wide Lip Sync Mode of CC4 Extended Blendshapes still requests no lip-sync data from the server, and the `-LipSyncMode` switch cannot select CC4.
- Face Sync's rest pose in CC4 Extended mode is now every CC4 curve at 0. It was the viseme rest pose.
- A face frame with more values than its format has curve names no longer stops the game or reads past the end of the name list. The extra values are dropped, and the log warns once per connection with both sizes.
- The Face Sync anim node reads its `LipSyncAnim*` overrides once, on the game thread, when its anim instance is created. An override set with **Set Custom Param** after that applies to the next anim instance; before, it was also picked up whenever the anim graph re-initialized.
- A C++ subclass of Convai Face Sync that overrides `ConvaiGetFacialDataNames` again decides which curves interpolate. The list is read once per Lip Sync Mode.
- **Pause Lip Sync** and **Resume Lip Sync** called off the game thread apply on the next tick instead of calling the lip-sync component from that thread.
- A lip-sync or vision provider that is not a UObject, assigned in C++ to `ConvaiLipSync` or `ConvaiVision`, stays bound and feeds Get Facial Data, Get Facial Data Names, Get Face Blendshapes and Has Playable Face Frames. Before, it was dropped on the next tick.
- Corrected the Lower and Upper Face Smoothing Speed tooltips on the Convai Face Sync anim node.

## Voice playback

- The chatbot's voice is pulled by the audio device as it plays. A stop from a network thread can no longer be undone by the same frame, no part of a cut reply plays after the stop, and the next reply keeps its start cushion.
- Talking reliably starts for a reply pulled whole between two frames after a stop.
- The audio render thread never waits for the game thread or a stop; when either holds the voice, it renders one buffer of silence.
- `Convai.Build.cs` adds a private dependency on `AudioExtensions`.

## Actions

- C++ implementations of `IConvaiConnectionInterface::OnActionSequenceReceived` are now called on the game thread.
- An action's target (`RefValue`: its resolved component and position) is now looked up when the action arrives, on the game thread, so `ResolvedComponent` is filled in as documented.
- Packaged builds no longer offer action Enum entries marked Hidden, and native enums keep their display names there. The editor saves each Enum parameter's labels (`FConvaiActionParam.SavedEnumLabels`) when it saves or cooks; parameters built at runtime keep the old behaviour.
- The deprecated **Get Action Param As Number** returns 0, with a warning, for values that read as infinite or NaN, matching Get Param As Number. **Get Param As Byte**'s tooltip now says it returns the matched entry's value, not its index.
- **Move To** ends with Missing AI Controller when its pawn is unpossessed or destroyed, including when play stops mid-move. An Escort that loses its pawn says "You cannot move right now."

## Push-to-talk

- A failed or timed-out release no longer turns push-to-talk off until the next connection: the next press works. This replaces the 4.0.0-beta.32 note. The exception is a release the server answers with `user_input_commit_timeout`, `stt_reset_failed` or `stt_reconnect_failed`, which mean that session takes no more voice. Push-to-talk then stays off until the session reconnects, and with **Session Reconnect Attempts** above 0 the plugin reconnects it at once. A press refused this way logs one warning.

## Microphone

- With **Auto Initialize Session** on, a player component on a pawn now starts its session only once a local player controller possesses the pawn. A pawn nobody possesses, another player's pawn and a pawn an AI controller possesses no longer start one, and neither does a component on another player's player controller. A component on any other actor still starts in Begin Play.
- When the player possesses a pawn again, that pawn's player component takes the microphone back from the component on the pawn they left, so push-to-talk bound to the possessed pawn keeps working.
- One microphone per game instance: a newer player session displaces the older one, which stops capturing and gets the microphone back when the newer owner leaves. **Unmute Streaming Audio** on a displaced component returns false with a warning.
- After a game-thread stall or a paused world, an open microphone sends only the newest 0.2 s of its backlog, so a stall no longer leaves the player's voice permanently delayed. A game drawing steadily at a low frame rate still sends all its audio, and a push-to-talk hold keeps up to 1.5 s. The microphone drain no longer slows with time dilation.
- A speaking bracket the server opened waits for its stop or a final transcript, so long speech on OpenAI Realtime is no longer cut off, unless the player is muted or stops streaming during it. One that only transcripts hold open closes after 10 s with no partial transcript, and a session that ends while the player is speaking fires **On Finished Talking** once.
- The Convai Player no longer changes the engine's `voice.MicNoiseGateThreshold` and `voice.SilenceDetectionThreshold`.
- An adopted capture component, the Pixel Streaming microphone included, keeps its own gain unless one is set through the player or a saved gain other than 1.0 exists, so the Pixel Streaming microphone no longer starts at twice the volume. An adopted component destroyed at runtime hands the microphone back to the built-in device.
- On a machine with no working microphone, the capture health check warns once per run of failures instead of on every restart.
- **Save Microphone Settings** clamps the gain to 0-2 (a non-finite gain becomes 1.0) before storing and applying it.
- Shipping builds no longer write `Saved/AudioDebug/recorded_audio.wav`, and ending play during a mic test no longer writes it in any build. **Start Recording** is refused during a push-to-talk hold, and **Mute Streaming Audio** no longer silences a running mic test.
- An audio processor (`IConvaiAudioProcessingInterface`) receives `SetProcessedAudioReceiver(nullptr)` when the player lets go of it, and its processed-audio callback may run on any thread.
- Vision refuses frames the library can't read (an odd side, a side under 16, or a short buffer), and the first frame is now sent.

## Connection

- A session the server ends with `session-ending` and `reconnectable: false` is no longer reconnected. If it was closed for inactivity, **Idle Behavior** still applies, except that **Keep Session Open** does not reopen it.
- **Set API Key**, **Set Auth Token**, **Set Custom Param** and **Set VAD Settings** must be called on the game thread; from another thread they now raise an ensure.
- Custom params the plugin cannot use are replaced where it uses them, with one warning per key: a `ClientReadyRetrySecs` below 0.1 s becomes 0.1 s, a `ClientReadyTimeoutSecs` below 5 s becomes 5 s, a negative `ObjectPollIntervalSeconds` becomes 0.25 s, and a `ChunkSize` or `OutputFPS` of 0 or less becomes 10 or 90. **Get Custom Param** and the other Convai getters still return the value as set.
- Ending play while a reconnect waits out its backoff no longer logs "DisconnectSession: Character session is not the current active session".
- A Blueprint that starts or stops a session from inside a getter the connect reads can no longer start a connection on a client that is being destroyed.
- `SendAudio` must be called on the game thread; from another thread it raises an ensure.
- An empty `-ConvaiProdURL=` or `-ConvaiBetaURL=` no longer clears a URL set in Project Settings.
- Fixed the editor reading freed memory on its first frame (it crashed under `-stompmalloc` or memory checkers).

## Logging

- `ConvaiLogs` now lives in the project's Saved folder as the engine resolves it, so a Shipping game writes it to the user's saved folder (on Windows `%LOCALAPPDATA%\<Project>\Saved\ConvaiLogs`) instead of its install folder. Packaged builds keep the newest 20 log files, and a file rolls over to a new one past 50 MB.
- In the editor and in Development builds, the plugin's Verbose lines, streamed packets such as `bot-llm-text` among them, reach `ConvaiLogs` only when their category is raised, for example with `-LogCmds="ConvaiSubsystemLog VeryVerbose"`. A Shipping build, which compiles logging out by default, never writes them.
- A typed prompt's reply is written to `ConvaiLogs` as one Log line with the full reply text, as a spoken reply already was.
- Connect metadata and the action config are logged by size (in full at Verbose), and high-rate packets log a size summary (in full at VeryVerbose). Face Sync turn stats are logged only with `ConvaiDebugMode`.

## Cloud Projects

- **Cancel** during the Package stage now stops the cook: the panel runs Unreal Automation Tool itself and ends its whole process tree, discards the partial output, and a new upload can start at once. Packaging output goes to the Output Log under `LogConvaiCloudPackaging`. Don't run Unreal's own Package Project while an upload is packaging.
- Upload changes and Create save unsaved content first. Project content (`/Game` or the project's plugins) that can't be saved stops the upload and is named; other content is logged as a warning and its copy on disk is used.
- Uploads of 2 GiB or more are refused before a version is reserved. Archive entries are stored uncompressed and checked after writing, so uploads are somewhat larger.
- The version number never goes down, Make active and the upload's activation no longer overwrite dashboard settings with defaults, and the panel no longer rebuilds on every progress update.
- Prepare adds the Pixel Streaming capture to project (`/Game`) children of the SDK player Blueprints and to C++ player pawns, never a second one where a Blueprint already has it.
- Fixed: Publish and Save draft stuck disabled after a refresh while idle; the selected row losing its highlight after a search; signing in as another account showing the previous account's projects.

## Cloud Avatars

- Cancel works while an avatar's source is archived and while it is validated; the notices read "Preparation cancelled." and "Operation cancelled. Nothing was uploaded." (or installed). A Cancel right after archiving no longer leaves a download that the next editor start installs without asking.
- Thumbnails decode off the game thread, and progress updates keep the library's scroll position and Advanced options.
- Avatar files are written atomically, and a temporary file left by a crash no longer blocks Upload changes.
- Packaging keeps the newest run and deletes older runs' output; the first prune after updating can free several GB and logs what it deleted.
- A draft whose cloud avatar could not be linked is retried on every refresh, across restarts. It is dropped only if this session deleted the avatar or the server reports it gone, never because a listing missed it.
- A failed in-place upload that changed files unloads that avatar's content, closing its open editors, so no half-loaded package is left behind.
- The local avatar list reports damaged records instead of skipping them, and rejects every folder that claims an asset ID another folder also claims.

# Release 4.0.0-beta.32

## Push-to-talk

- Releasing the talk key no longer cuts off the end of what the player said. The plugin now sends the last microphone audio before it ends the turn, then waits for the server to confirm the turn.
- A new press waits until the previous release is confirmed. While the server is still finalizing, the same release is retried at most twice, and the player's speech is never sent again.
- Added four events to the Convai subsystem under **Convai | Push To Talk**: **On Ptt Commit Completed** when the server has the whole turn, **On Ptt Commit Empty** when it heard nothing, **On Ptt Commit Failed** with the reason, and **On Ptt Release Accepted** for servers that only confirm the release was queued. After a failed or timed-out release, push-to-talk stays off until the next connection.

## Connection

- Ending or restarting a session now waits for any native callbacks still running before it releases the session's listener. Before, a session that closed while audio or messages were still arriving could call into memory that had already been freed.
- Idle warnings and interaction IDs from a session that has already ended no longer reach the session that replaced it.
- Updated the bundled native client to 0.2.15, which these fixes need. It also tells Unreal about a character that was already in the room when the player joined, which could leave the session stuck on **Connecting**.

## Microphone

- Mono microphones are now recorded at full level. They used to come through about 6 dB quieter than stereo microphones.

## Connection

- Renamed **Reconnect Attempts** to **Session Reconnect Attempts** to clarify its scope: the full retry budget per failure, including the first retry, reset when the character becomes ready. The default remains 0; transport recovery, microphone recovery, and game calls to Start Session are separate.
- **Idle Behavior > Keep Session Open** replaces **Reopen Immediately** and renews server idle warnings even with session retries off. AFK Time is disabled for this mode; network recovery still respects Session Reconnect Attempts. Existing saved selections carry over.

## Cloud Avatars

- Environment uploads now keep the placed avatar and its settings in the copied level, with clearer checks for supported avatars and cameras.
- Fixed copied sky and other Blueprint assets retaining references to original project files or losing saved settings during preparation.
- Preserved per-instance component settings and inherited overrides when copying an avatar and its environment.
- Fixed a crash while checking copied Blueprints after preparing an avatar and its environment.
- Windows packaging no longer requires plugins that support only other platforms.
- Added a fallback download location for uploader settings when the primary location is unavailable.

## Cloud Projects

- Added **Runtime settings** so you can change how an uploaded project runs without packaging or uploading it again. Browser options cover the microphone, mouse behaviour, viewport matching and idle timeout; Unreal options cover the stream resolution, screen percentage and extra launch arguments.
- Each browser option can be left on **Use runtime default**, which leaves the setting alone instead of forcing it on or off.
- **Save and redeploy** applies changed settings to the running build straight away. **Save only** stores them for the next time the project is deployed. If nothing has actually changed, the tool now says so instead of reporting a redeploy that never happened.
- The **Versions** list shows every upload with its build state and size, and lets you make an older version active again, take over a rollout that is still in progress, or cancel one and keep the current version live.
- Added **Logs** on each version, so a failed cloud build can be read without leaving the editor.
- Added **Publish**, for setting the page name, description and who can open it — **Public**, **Unlisted** or **Private** — and for saving those details as a draft first.
- Hosting capacity set on the Convai dashboard is now left untouched when you activate a version from Unreal. Previously it could be replaced with default values.
- Fixed the project list showing the wrong version and state when the active version was not the most recent upload.
- Preparing a project for upload now only edits your own Blueprints, not the ones that ship with the plugin.
- **Open in browser** now opens the player that matches the Convai environment you are signed in to.

## Fixes

- Characters without a Character ID no longer try to connect automatically or interrupt another character's connection.
- The character's **Session ID** is now filled in from the server, so Blueprints can read it for conversation history. It is available before the Connected events, kept after a disconnect, and stays -1 until received or after Reset Conversation.
- Convai MetaHuman animations no longer distort the body when the project's MetaHuman skeleton differs from the one the content was saved with. Retarget poses are now refreshed in the editor and when cooking.

# Release 4.0.0-beta.31

## Highlights

- Scene Auto Tagger captures and Edit View use more consistent lighting, preserving object detail in scenes with harsh lights or deep shadows. Nearby and repeated views can finish their initial wait sooner when their resources are already ready.
- Added a Cloud Avatars library for creating, downloading, and updating custom avatars, with editable source, Windows packages, dependency preparation, and portraits.
- Added configurable reconnection after network interruptions and idle sessions, plus fixes for session restarts and late-starting player components. Enable it with **Reconnect Attempts** in Project Settings; it remains off by default.
- Packages cover Unreal Engine 5.3–5.8 on Windows and Android. Scene Auto Tagger remains available in the Windows editor on Unreal Engine 5.6–5.8.

## Reconnect

- Reconnect – The plugin now reconnects a character by itself after a network or server drop, and a session the server closed for inactivity comes back the moment the player talks, types or presses push-to-talk again. Set **Reconnect Attempts** (Project Settings ▸ Plugins ▸ Convai ▸ Connection) to turn it on; it defaults to 0 (off) in this release. No new events: **On Server Connection State Changed** reports one **Reconnecting** per drop, then **Connected**, or one **Disconnected** with the reason when it gives up; the character's own **On Attendee Connection State Changed** still reports the drop. A Blueprint that restarts the session from **Disconnected** keeps working: the restart is folded into the pending attempt instead of opening a second one. The debug overlay shows `RECONNECTING n/N` or `DORMANT`.
- Idle Timeout Behavior – A session the server closes for inactivity used to be gone until the game restarted it. With reconnect on, **Idle Timeout Behavior** (Project Settings ▸ Plugins ▸ Convai ▸ Connection) decides what happens next: **Reopen When Player Returns** (the default) reopens it the moment the player talks, types or presses push-to-talk; **Reopen Immediately** reopens it straight away, once per idle close; **Stay Closed** keeps the old behaviour.

## Fixes

- Kept Cloud Avatars and Cloud Projects compatible with older supported engines, including streamed downloads, large-upload progress, avatar portraits, scene copying, and ZIP packaging.
- Restored Unreal Engine 5.6-5.7 compilation for avatar diorama lighting, deletion, external-package traversal, and capture warm-up checks while retaining the newer 5.8 APIs where needed.
- Session Fix – Restarting a session from a Disconnected event no longer re-enters itself (up to 59 levels), freezes the game thread while it joins a stalled connect (up to 171 s), or opens dozens of throwaway sessions; the restart now runs on the next tick. Stopping a session no longer waits on the network.
- Player Fix – A **BP Convai Player Component** that begins play after the character connected now attaches to the live session; before, its microphone and text were silently dead.
- Migration – A player's **Stop Session** no longer marks the next disconnect as explicit. Text typed before the character is ready is held until it is. Template keys, dynamic info and the character's context now go out when the character is ready, and again after every reconnect. Narrative triggers and context queued during an outage are delivered only if the gap was short (under 30 s); after a longer gap, or a close the server or the game chose, context lands silently and stale triggers are dropped. Context and triggers now wait for the character to be ready at any **Reconnect Attempts** value. **On Failure** still fires as before in this release. With **Reconnect Attempts** at 0 the microphone watchdog still reconnects once, as before.
- Push To Talk – Each release now carries a unique id, so a server still finishing one release refuses an overlapping one instead of silently merging the two captures.
- Quieter logs: `bot-output` is recognized instead of being reported as an unknown packet several times per sentence.

## Cloud Avatars

- Fixed repeated save prompts caused by temporary Unreal editor packages being mistaken for unsaved avatar assets.
- Upload refresh preserves unchanged copied enum assets that Unreal's Niagara registry still references.
- Fixed copied child actors and Blueprint components retaining their original classes, while preserving customized component settings.
- Added **Include Convai content** under **Advanced upload options**, off by default, to copy referenced Convai animations and other assets with an avatar. The choice is saved per avatar.
- Editable-source downloads retain the saved Convai-content choice and references to the copied assets.
- Portrait framing now separates camera depth from image width, so props extending toward the camera no longer make the avatar unnecessarily small.
- Upload changes releases Unreal's cached Blueprint actions that could incorrectly keep an avatar's prepared assets in use. Unsaved edits and genuine live references still prevent unsafe replacement.
- Creating or updating an avatar offers a Save Selected / Cancel dialog for its unsaved assets before continuing. Unrelated project edits are left alone.
- Packaging honors the exact missing external files approved during avatar review, instead of incorrectly asking to enable their missing plugin again.
- Incomplete plugin installations now identify missing uploader files before preparing an avatar and explain how to restore the same SDK release. Local helper failures no longer default to a misleading GitHub connection error.
- Fixed an upload failure where MetaHuman hair dependencies could still reference assets outside the prepared avatar plugin after copying.
- Added an editor library for creating, downloading, and updating custom avatars for Avatar Studio and ConvaiSim. Public avatars are excluded from this management view.
- Added automatic dependency preparation, transparent portraits, and an uploader workspace linked to each avatar's permanent content plugin.
- Added independent Windows package and editable source uploads, metadata-only edits, download replacement review, and operation logs.
- Added warnings for unavailable Windows packages and engine-version differences, plus per-file upload byte counts and storage-reported file sizes.
- The library now loads private records and thumbnails before checking detailed files for a selected avatar. Unknown availability stays distinct from missing files.
- Added native 1024 × 2048 transparent portraits and square cards styled with the Scene Auto Tagger controls. The original portrait stays available for editing and inspection.
- Locally created avatars keep their original project Blueprint as the editable source. Uploading refreshes its prepared copy when saved files change, with an exact review of known conflicting edits. An unchanged original preserves edits made only to the prepared copy; downloaded avatars use their installed Blueprint.

## Cloud Projects

- Added **Tools > Convai > Cloud Projects** to package and upload a project for browser streaming from the Unreal editor.

## Release tooling

- Added manual feature-preview releases with engine-version and Windows/Android selection. Manual builds default to Unreal 5.7 and 5.8 for Windows; automatic normal-channel releases retain all supported engines and both platforms.
- Preview filenames and release manifests identify the selected platforms, and published versions cannot be reused with different build contents. Development tests are excluded from release packages.

## Scene Auto Tagger

- Kept capture warm-up automation in development-only test files so release packages exclude the test code.

- Object captures now use a capture-local key/fill rig, controlled studio reflections, and neutral post-processing, isolating analysis images from level lights, exposure, color grading, and indirect/environment lighting while preserving source objects and materials. Angle probes, retained images, and Edit View share this profile; scene-context drafts still use the editor viewport.
- Suppressed direct highlights on diffuse-only artwork captures, reduced studio ambient veiling on glossy dark materials, and moved neutral fill lighting higher and to the side to retain surface contrast and sculpted detail.
- Added one guarded exposure recovery for neutral dimensional captures that remain too dark after calibration, preserving dark marble detail without brightening already-readable subjects or planar artwork.
- Shortened probe waits for already-primed views and recent nearby captures only when current conventional meshes, textures, and materials are ready. The shortcut preserves three real Editor frames and all final-image warm-up/calibration settling; Nanite, virtual-texture, skinned, unknown, and still-loading resources keep conservative waits.
- Added development-only real-render lighting regression coverage with saved image comparisons and source-component preservation checks, plus focused warm-probe readiness and nearby-capture boundary tests.

# Release 4.0.0-beta.30

## Highlights

- Talking Power lets animation Blueprints react to the strength of the character's playback audio, including pauses.
- Scene Auto Tagger is available on Unreal Engine 5.6–5.8. It is disabled in the UE 5.3–5.5 packages because those engines lack the required rendering features. Chatbot, Talking Power, animation and Scene Objects functionality remain included.

## Talking Power

- Added audio-driven Talking Power for animation Blueprints. Character animation can follow the strength of the audio being played, including pauses, instead of relying only on a speaking/not-speaking flag.

## Compatibility and packaging

- Added a Convai Pixel Streaming Audio Capture component so the player component can receive microphone audio from Pixel Streaming.
- Limited the Pixel Streaming dependency to its supported desktop platforms so it does not block Android packaging.
- Added registration of separate action-handler objects, keeping the character actor's existing handlers first.
- Improved connection-event ordering when an attendee arrives before the server connection completes, and kept an inactive chatbot's teardown from suppressing another chatbot's reconnect.
- Added version-specific engine API adapters while keeping the existing UE 5.8 paths. Source installations on UE 5.0–5.5 must exclude Scene Auto Tagger using Resources/ConfigureEngineModules.ps1; packaged UE 5.3–5.5 downloads already apply this exclusion. UE 5.6–5.8 retain Scene Auto Tagger.
- Improved release filtering so internal tests and private Core implementation/debug files stay out of public packages. Core remains a compiled dependency; this release does not claim digitally signed Core libraries.

## Scene Auto Tagger

- Added the Scene Auto Tagger editor workflow for Current Level and Selected Actors: discover relevant objects, capture actor-owned evidence, generate names and descriptions through Convai Vision, review proposals, and apply accepted entries in one undoable transaction.
- Added analysis quality presets, optional per-map Scene context and Description focus, character selection, and a protected scene-wide Draft from current view task. Analysis starts only after consent and an explicit user action.
- Added deterministic candidate views, streamed-resource warm-up, retained Analysis images, Edit View camera correction, recapture, saved-image reanalysis, and actor-scoped refinement notes.
- Added exact-geometry reuse, conservative evidence-based duplicate reconciliation, contact-sheet batching, bounded parallel requests, cancellation, targeted retries, and preservation of completed results when other rows fail.
- Added responsive review, checkbox-scoped batch actions, selected-row context actions, and the ability to add selected Level actors to an existing review. Apply preserves manually authored and component-scoped Convai data.
- Added scene-object management and movement-point authoring, with reviewed merged objects retaining stable group identity and navigation selecting a reachable concrete member.
- Integrated Core 0.10.0 through the compact ABI4 boundary. Unreal owns discovery, rendering, streaming, batching, retries, review, and Apply; Core owns view/evidence policy and protected Vision prompts/transport. The formal dependency release records exact desktop artifacts and provenance; only Win64 is enabled in Unreal.
- Kept capture lighting isolation and further warm-resource optimizations as follow-up work; captures currently retain authored scene lighting with bounded recovery for unreadable objects.

# Release 4.0.0-beta.29.1
- Emotion Fix – A character's feelings no longer blank out in the middle of a reply. The server reports "neutral" far more often than it reports a real emotion, and every one of those reports cleared all eight scores to zero — so emotion-driven animation and Blueprints reading **Get Emotion Score** saw a feeling appear and vanish within the same answer. Neutral reports are now ignored and the last real emotion holds until the server reports a different one.
- **Emotion Offset** on the BP Convai Chatbot Component now defaults to 0.2 instead of 0, lifting every reported emotion's score so expressions read on the face instead of sitting just above zero. Set it back to 0 for the previous strength.

# Release 4.0.0-beta.29
- Echo Cancellation Fix – The microphone and the character's playback are now taken from the same place in Unreal's audio path, so echo cancellation has two streams on one clock to compare. Previously the microphone was captured through a recorder that is stopped for the whole of each send cycle, losing 1-2.5% of its samples against the playback reference every second — the two timelines drifting apart faster than any canceller can follow, which is why the character kept hearing itself through the speakers. Two related faults are fixed alongside it: two capture paths could arm at once and send the microphone at twice the expected rate, and multi-channel audio was downmixed by taking the first channel and letting one channel's samples leak into the next. On by default; nothing to configure.
- Push To Talk Fix – Releasing the key no longer ends a turn the server has already ended and answered on its own. That second stop committed an empty recording, which the speech service rejects; the reply for that turn was lost. A hold the server never bracketed still ends on release, as before.
- Connection Error Fix – Advisory messages from the server are no longer reported as connection failures. Every server error was treated as fatal, so a perfectly healthy session could raise dozens of failure events (90 in one measured session) that Blueprints listening for a lost connection would act on. Errors that really do end the session still arrive as failures.
- Chat Transcript Fix – The character's reply no longer repeats itself in the chat widget ("Hello Hello. Hello."). Broadcasts carry one sentence of the answer at a time, appended to the ones before it, as they did in beta.27; the widget that ships with the plugin renders that correctly with no changes. The delegate's contract is now documented on **On Transcription Received**: the character's broadcasts are fragments to append and its final is empty, while the player's carry the whole utterance so far and the final carries the server's corrected rendering. Known gap: when the player types instead of speaking, the server sends no character transcript, so the answer does not reach this delegate on that path.
- Quieter logs: `final-user-transcription` is recognized instead of being reported as an unknown packet on every utterance.

# Release 4.0.0-beta.28
- Push To Talk Fix – **Enable Push To Talk** on the BP Convai Player Component now does what it says: the character answers when you release the key instead of at the first pause for breath. Releasing tells the server your turn is over, and the silence timer the server uses to end a turn on its own is raised to **Push To Talk Stop Secs** so a mid-sentence pause cannot trigger a reply. Nothing to rewire — the existing press and release already hold the microphone through **Mute**, which is where this hangs. Open-mic projects are unaffected.
- Emotion Fix – Character emotions now respond to what the character is actually feeling. The server reports emotions on the Plutchik wheel, where each feeling comes in three strengths (Serenity / Joy / Ecstasy, Acceptance / Trust / Admiration, and so on), but the plugin only recognized eight legacy names — so most of what the server sent was discarded and every emotion score stayed at zero. All 25 names are now understood, the older names still work, and a report that arrives without a strength is treated as moderate instead of an arbitrary value. Emotion-driven animation, expressions, and Blueprint logic reading **Get Emotion Score** start reacting without any project changes.
- Crash Fix – Fixed an audio thread crash that could occur while disconnecting and reconnecting.
- Meta Quest crash fix — Fixed a crash that occurred when connecting to Convai in the packaged app on Meta Quest.

# Release 4.0.0-beta.27
- The included **BP Convai Chatbot Component** now has the **Escort** action ready to use, so characters can lead a player or another character to a destination without recreating the Blueprint handler. The unused **Wait for Action** entry is disabled.
- Added Blueprint-accessible Knowledge Bank operations and fixed REST requests using a custom Convai server URL.
- Improved move-task cancellation during level shutdown and garbage collection to avoid accessing navigation components that Unreal is already destroying.
- Fixed microphone tests being a no-op while a conversation was live: recording now works during an open stream, the character doesn't hear the test recording, and the project's mute setting is restored afterward.
- Simplified **Invoke Speech** and **Invoke Narrative Design Trigger**: the Generate Actions and Replicate On Network options are gone — behavior now follows the chatbot's own configuration. Invoke Speech also gained a Delivery option (it can wait for a quiet moment instead of interrupting), and its message is heard exactly once instead of piling up in the conversation context. Existing Blueprints that wired the removed pins need a node refresh.
- Body and face mesh detection now only considers meshes with a running animation Blueprint, so hair, cloth, and static props can no longer be mistaken for a character's body or face.
- Characters can now give directions from the player's point of view. Every described object also tells the character where it sits from the player's position ("From Player's position, the crate is close by, to the left"), following the player's camera — so asking "where is it?" or "what am I looking at?" gets an answer that matches what the player actually sees, instead of directions from the character's own viewpoint. Controlled by **Describe From Player Perspective** under Project Settings > Plugins > Convai > Spatial Awareness (on by default).
- The debug overlay (Ctrl+Alt+K) now always shows the full picture: the exact surroundings sentences the AI reads, plus the FACTS and EVENTS sections, are visible in every build instead of only in internal developer builds.
- Kept compatibility with Unreal Engine 5.0 through 5.8.

# Release 4.0.0-beta.26
- Added reusable **Convai Move To** and **Convai Escort** Blueprint nodes. Escort characters can lead someone to a destination, wait when they fall behind, resume when they catch up, and handle destinations that are already nearby.
- Added cooperative action-plan cancellation. Characters can change course while an action is running, and custom Blueprint actions can clean up before replacement actions begin.
- Improved parameterized actions with more reliable named values, spaces, punctuation, localized choices, and object-style responses. Large action lists and multibyte text are no longer cut off by fixed-size buffers.
- Improved realtime compatibility by reporting the supported protocol and client version correctly, recognizing current lifecycle messages, and forwarding interaction IDs.
- Improved goal resolution and merged-object navigation so movement tasks choose reachable destinations and distinguish arriving from already being there.
- Added the authenticated editor route needed by the optional Convai Pak Manager and fixed linking for external editor plugins that resolve their own Convai services.
- Updated the included AgentSkills and action documentation for the new movement, escort, cancellation, and response behavior.
- Kept compatibility with Unreal Engine 5.0 through 5.8.

# Release 4.0.0-beta.25.1
- Fixed the plugin failing to load on Unreal Engine 5.8 installed from the Epic Games Launcher, where opening a project showed "Plugin 'ConvAI' failed to load because module 'ConvaiToolset' could not be loaded". Projects now open normally; Unreal Engine 5.0-5.7 was not affected and needs no changes.
- Strengthened release testing: every update is now automatically verified to open cleanly in the Unreal editor on UE 5.6, 5.7, and 5.8 before it ships.

# Release 4.0.0-beta.25
- Added movement awareness for scene objects, including direction, stop detection, five sensitivity levels, optional movement states, and configurable start/stop responses.
- Added Movement Points and named destinations so characters can choose reachable approaches, understand when they have already arrived, and react when paths open or close.
- Added delivery controls for live context updates. Responsive events can wait for a natural pause, send immediately when timing matters, or update the AI silently.
- Added the experimental **Remind Self** and **Watch Property** actions, helping characters continue multi-step tasks and react to the next meaningful property change.
- Improved tracked properties with nested object paths, aliases, Blueprint-rename recovery, clearer Details panels, and concise change wording.
- Improved conversation-state and player-speaking events, including recovery when voice start/stop signals are missing.
- Added the Convai Debug Overlay for inspecting surroundings, context states, reachability, tracked properties, and action results during development.
- Reduced prompt repetition across duplicate objects and spatial facts, and improved action goal resolution for Blueprint movement.
- **Blueprint migration:** object-goal fields and the **Resolve Goal Location** outputs have clearer names; existing graphs using the previous pins must refresh and reconnect those nodes.
- Kept the plugin compatible with Unreal Engine 5.0 through 5.8.

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
