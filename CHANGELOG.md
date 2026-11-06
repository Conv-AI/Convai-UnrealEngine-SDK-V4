# Changelog
All notable changes to this project will be documented in this file.

# Unreleased

# Release 4.0.0-beta.34

## Connection

- A usage limit the server reports in the first second or so of a session is no longer forgotten when the session finishes connecting, or when **Start Session** takes over a session opened by **Prepare Character Connection**, so the session is not retried even with **Session Reconnect Attempts** above 0.
- Context updates (**Update Context** and the chatbot's own context updates), scene updates, the chatbot's **Narrative Template Keys** and the C++ `UConvaiConnectionSessionProxy::UpdateDynamicInfo` that the message queue refuses, for example while a session closes or when 64 messages are already waiting, now log a Warning naming the message type, such as "The message queue did not accept context-update", at most once per type per connection. A refused client-ready, which the plugin resends every 0.5 s until the character is ready, logs the same line at Log. Before, only the native client's own line, which names no message, recorded these refusals.
- When a session closes within 0.1 s of the player component's **Send Text**, through **Stop Session**, a **Start Session** that replaces it or the end of play, the plugin now logs a Warning that the text may not reach the character: the native client drops queued messages it has not sent. The text itself is not logged. Before, nothing in the log said so.
- When the native client fails to send a message it had already queued, the plugin now logs a Warning, "The native client failed to send a queued message", with the native error. Every session shares the native log, so the message can belong to a session that has already closed. Before, the failure showed only as the native client's own line in `ConvaiLogs`, and in the Output Log only with `ConvaiClientLog` at Verbose.

## Push-to-talk

- When another push-to-talk player component takes the microphone while the talk key is held, for example when the player possesses another pawn mid-hold, the hold is now released as if the key were let go: what was said is sent and the turn ends. Before, the hold ended without telling the server the turn was over. As after any release, the new component's presses are ignored until the server answers the release.
- When a push-to-talk release cannot send `stt-toggle` or `force-user-stopped-speaking` (**On Ptt Commit Failed** with `release_send_failed` or `release_retry_send_failed`), or the mute after a failed audio drain cannot be sent, the plugin now logs which message failed, at Log. Before, nothing said which one.

## Microphone

- A player component that gets the microphone back, or starts its session, while its mic test runs now starts streaming when **Finish Recording** ends the test, including when **Auto Initialize Session** starts the session as the player possesses the pawn. Before, it held the microphone but sent nothing until the game called **Unmute Streaming Audio** or started the session again.
- **Mute Streaming Audio** on a player component that another one displaced now keeps it muted when the microphone comes back to it. Before, it streamed again when the newer owner left or the player possessed its pawn again. **Unmute Streaming Audio** on a displaced component is still refused, so call it once the microphone is back.
- The 10 s limit on **Is Speaking** now also applies when the server reports that the player started talking after they muted (including letting go of the push-to-talk key), called **Mute Streaming Audio** or started a mic test: **On Finished Talking Delegate** fires 10 s after that start, or after the last partial transcript, unless the server's stop comes first. Before, **Is Speaking** could stay on until the player spoke again.
- The player component's **Save Microphone Settings** and **Save Microphone Settings From Player Component** now keep the saved microphone while it, or the chosen one, is unplugged and the player uses the system default, so the next launch picks it up once it is back. Before, they saved the system default. **Reset Microphone Settings To Defaults** goes back to the system default. This replaces part of the 4.0.0-beta.33 note.
- **Get Active Capture Device** now reports the microphone in use, with its current index, after another device is plugged in or out, and saving stores that microphone. Before, it reported whichever device had moved into the old index, and saving stored that one. While the microphone in use is unplugged, it reports the system default with index -1.
- When the microphone health check restarts a microphone that delivers no audio, its log lines now say how long the restart took ("restarted capture in 12 ms"), and Unreal Insights shows each restart as `UConvaiAudioCaptureComponent::RestartCapture`.
- A microphone reopened during play now captures: one the health check reopens, for example once another app releases it or once one is plugged in, and one chosen with **Set Capture Device By Name** or **Set Capture Device By Index** while the player is capturing. Before, the reopened microphone sent silence for the rest of the session, and the health check kept reopening it, up to every 30 s, each time stalling the game for about 400 ms.

## Objects

- At Begin Play, and from **Ensure Object Components For Environment Objects**, every Environment entry's actor now gets a Convai Object Component. Before, when the component made for an earlier entry took the name of a later entry ("Crate 2"), that later entry's actor got none. When it took a new numbered name or added named destinations, Development builds hit an "Array has changed during ranged-for iteration" ensure, and any build could read freed memory, which can crash.
- A tracked-property change that waits for a pause in the conversation (**Delivery** set to **Wait Until Conversation Is Idle**) now keeps waiting, with its **Should Respond** setting, when another member of its merged object set (**Merge With Same-Named Objects**) is destroyed, turned off or stops tracking a property, and when a member with the same value joins the set. Before, it was sent at once with Never, so the character did not answer it, or, when a **Watch Property** had promoted it, at once as Always, so the character could answer mid-conversation.
- A tracked-property change that waits for a pause in the conversation also keeps waiting, with its **Should Respond** setting, when another member of its merged object set with the same value starts sending that property later: when it can read the property only some time after joining the set, when it adds the property with **Add Tracked Property**, or when an **Alias** change in **Update Tracked Property** makes one of its tracked properties send under the same name. Before, the change was sent at once with Never, so the character did not answer it, or, when a **Watch Property** had promoted it and that member's property does not wait for a pause, at once as Always.
- Reconnecting within 30 s of a connection drop, or starting another session, while the character still has a tracked property's value no longer cancels a change to that property that is waiting for a pause in the conversation: the character gets it at the first pause, with its **Should Respond** setting. Before, connecting sent the value at once with Never, or as Always when a **Watch Property** had promoted the change. A reconnect that starts 30 s or more after the drop, or reopens a session closed for inactivity, still sends the value at once with Never; one that passes 30 s while it connects delivers the change at the first pause without asking for an answer. A property the character does not have yet, such as one that changed before the first session started, is still sent at once when the session starts.
- When a Blueprint renames one member of a merged object set at runtime and another member is destroyed or turned off, and no member keeps the old name, the object and its tracked-property values under the old name now leave every character, including values only the renamed member tracks, so the character can no longer talk about or walk to the removed member. The renamed member stays on every character under its new name. Before, the old name stayed until the renamed member left.
- When a Blueprint renames a whole merged object set at runtime and one member is destroyed or turned off, the set now keeps its object and tracked-property values under the new name, and nothing stays under the old one. Before, it lost its object and values under the new name, and the old name stayed until the last member left.
- A tracked Blueprint function that adds, removes or updates tracked properties on its own Convai Object Component, or edits its **Tracked Properties** array, no longer corrupts memory or crashes, and its value is no longer sent under another property's key. When the function stops tracking itself or a property listed before it, or writes the array so that it lists other properties, another order or another **Alias**, that poll stops, and the next poll (0.25 s later by default) reads the rest. When the object registers, a property the function stops tracking is not sent, and one it starts tracking is sent by the next poll. In **Add Tracked Property**, the property being added is not sent if the function stops tracking it, and is sent by the next poll if the function writes the array that way.
- A tracked Blueprint function that destroys its own actor, or turns its object off with **Set Convai Object Enabled**, while it is read no longer leaves the object's values on every character after the object left. Read as the object registers or in **Add Tracked Property**, it no longer brings the object itself onto the characters either.

## Dynamic context

- An **Update Context** text that repeats a line, such as the same fact under two headings, now keeps every copy, in order, when the chatbot sends it again with later context updates. Before, only the last copy was kept, so in that example the fact disappeared from under the first heading.
- **Reset Dynamic Context** called before the character is ready, for example while a session reconnects, no longer erases what is sent after it: the reset reaches the character first, followed by the **Update Context** text, states, facts and events sent since the call. Context staged before the call and not yet sent is dropped with the reset instead of being sent when the character becomes ready, so it no longer makes the character reply; an attention object set before the call is kept. **Get Context State Value** returns nothing for a cleared state right after the call, instead of the old value until the character is ready.

## Cloud Avatars

- After a failed **Upload changes** leaves some of an avatar's files in use, such as ones placed in the open level, Unreal can no longer save its partly updated versions of those files. They no longer show as unsaved, so **Save All**, the exit prompt, autosave, **Package Project** and Cloud Projects uploads leave them out, **Get latest** replaces them without asking, and saving one directly fails. Meanwhile they cannot be opened for editing, and a level whose actors or built lighting are among those files cannot be saved. The files can be saved again once they unload, for example at the next **Upload changes** after nothing uses them, or after a restart. Before, they stayed marked unsaved, any of those saves could write the partly updated files, and the next **Upload changes** then kept them as if they were your own edits.
- If you change one of those files anyway, the next **Upload changes** stops and names it instead of discarding your change: restart Unreal, make the change again, save it and choose **Upload changes** again. Before, **Upload changes** discarded such changes without a prompt or, while the files were still in use, asked you to restart Unreal without saving them.
- A downloaded source that cannot register in this project, for example because its Blueprint does not compile with this project's engine and plugins, is now discarded after its first failed registration. The avatar's previous version comes back (a first download is removed), the message says why, and **Get latest** or **Download to project** then downloads the latest uploaded source. If the avatar is in use, for example placed in the open level, it is put back at the next editor start, before the level loads. Before, the rejected source was installed again at every editor start, which moved the working avatar into its backup; **Refresh** did not put it back, and **Get latest** retried the same files instead of downloading a fixed source.
- An avatar in your library whose download is pending in this project now offers **Discard downloaded update...** in its details. It stops the install and puts back the avatar's previous version (a first download is removed). The downloaded files are kept, and **Get latest** or **Download to project** then downloads the latest uploaded source. A discard that has to wait, because the avatar or a file the install added is still in use, finishes at the next editor start, and the message says so; choosing it again while a file is in use stops waiting and leaves what could not be undone. Before, nothing in Cloud Avatars stopped a download that kept failing because of this project, such as one blocked by unsaved changes to a shared asset it adds, or a restore whose backup is missing.
- A download that fails because of this project rather than its source, such as read-only configuration, a locked workspace record or unsaved changes to the avatar, even ones that stop its Blueprint compiling, still retries without downloading again, and each retry now restores the avatar's workspace record and cook setting as they were just before that attempt if it fails. Before, every retry restored them as they were before the first attempt, undoing changes saved since.
- When **Get latest** cannot register a downloaded source because of this project and the avatar has unsaved changes, the prompt to discard them now matches what happens: **Yes** puts back the avatar's previous version (a first download is removed), and **No** keeps your changes and the downloaded version for the next **Refresh**. Before, the prompt offered to apply the downloaded source, although **Yes** put the previous version back.
- The Cloud Avatars notice now keeps what it already showed when a refresh reports more: the result of a finished download, upload or delete, and the editor start's notice, stay next to problems found in local avatar folders. Each refresh replaces what the previous refresh or the editor start reported about pending downloads, local avatar folders and cloud avatars not yet linked to their local drafts, so a problem that was fixed or a download that finished leaves the notice. Before, any such problem replaced the whole notice, a pending download's message replaced the earlier ones too, and a refresh that found nothing to report left a fixed problem on screen.
- Cancelling a download while it shows "Validating the source and checking required plugins" no longer makes the next download or upload wait while the partly extracted files are deleted: they are now deleted in the background. Before, they were deleted one by one first, which took seconds for a large avatar.
- A download that fails while its source is validated or staged now deletes its partly extracted files, and folders that a crash or a forced exit left in `Saved/ConvaiAvatarStudio/PendingDownloads` are deleted a day later. Before, only a cancelled download cleaned up, and nothing removed the rest.
- A download you cancel just after its source was validated is no longer installed at the next editor start when Unreal closes or crashes before the cancel finishes; a download Unreal closes on at that moment is not installed either and must be downloaded again. Before, the next start installed it, and for **Get latest** moved the working avatar into its backup.
- A thumbnail you choose or capture in the create form now shows while the library's thumbnails are still downloading. Before, it could show "No image" for up to 20 seconds on a slow connection.
- Downloading an avatar again after its folder was deleted outside Unreal, in the same session, now reads the new files. Before, Unreal kept the deleted folder's asset data, so registration could fail or record an incomplete dependency list.
- When a downloaded avatar's install record (`ConvaiAvatarPendingInstall.json`) cannot be removed, for example because another program has it open, the download now stays pending with a message naming the file, and the next refresh finishes it. Before, the download was marked finished with the record left behind, so the avatar showed **Source available** instead of **In this project** for good, and **Upload changes** stayed disabled.
- A download that cannot finish because its avatar folder is gone is now stopped by **Refresh**, which reports it once and keeps every file. Before, every editor start and every **Refresh** reported it again, and only **Download to project** or **Get latest** could stop it, which an avatar that is no longer listed or has no uploaded source does not offer.
- A crash or power loss while Cloud Avatars saves an avatar's local record no longer brings back the previous record. Before, the older copy could win, so the avatar reverted to its previous record and the next **Upload changes** asked you to review files Cloud Avatars had written itself.
- When an **Upload changes** that updates the avatar's prepared copy in place fails after writing some of its files (a downloaded avatar, an avatar whose project Blueprint is unchanged, or adding Convai content), the retry no longer lists those files as your changes. Before, it asked you to confirm them as edits, and refused the automatic rebuild of a copy that lost its component overrides.

## Cloud Projects

- On UE 5.3, **Upload changes** and **Create and upload** now wait for another Unreal Automation Tool task that is already running, such as the editor's platform SDK check after startup or a **Package Project**, instead of failing with "Packaging failed".
- When packaging fails, the panel now gives the Unreal Automation Tool exit code and quotes the first errors, and the Message Log's **Packaging Results** page lists them. Before, it only pointed to the Output Log.
- A Blueprint that you give a Convai player component and upload without saving first now gets the Pixel Streaming capture on that upload, so the build has a microphone in the browser. Before, only the next upload added it.
- **Upload changes** and **Create and upload** now list the project's unsaved files in a **Save before uploading** dialog before they cook, and **Cancel** stops the upload without saving anything. Before, they saved every unsaved map and asset without asking, engine content included.
- Signing out or changing the API key during an upload now takes effect at the first action after the upload fails or you press **Cancel**. Before, later actions went on using the old credentials until a **Refresh**.
- A build log still loading when you press **Refresh** or **Cancel**, or when an upload, **Make active**, **Save and redeploy** or **Save only** finishes, now closes, and **Logs** opens it again. Before, it said "The cloud recorded no log lines for this build."

## MetaHuman animations

- On UE 5.6-5.8, the editor's startup check for out-of-date Convai MetaHuman animations now covers all of them over several startups: each startup checks the next even spread of at most 20, and the project is recorded as up to date only once every animation has been checked or **Yes** has fixed them all. Each startup's check takes about as long as before; about 400 animations are all checked after about 20 startups. Before, one clean spread of 20 recorded every animation as up to date, so out-of-date animations outside it were not offered for fixing until the MetaHuman skeleton or the plugin version changed. This replaces part of the 4.0.0-beta.33 note.

## Scene Auto Tagger

- Windows packaging no longer fails with "Failed to copy ... convai_scene_auto_tagger_core.dll" (or `convai_http_helper.dll`) when the plugin is installed in the engine, for example under `Engine/Plugins/Marketplace`. Packaged games now take the Scene Auto Tagger's native DLL and its HTTP helper from the plugin's `Source/ThirdParty/ConvaiSceneAutoTaggerCore/Win64/bin` folder, where the plugin ships them. Before, packaging expected copies in the plugin's `Binaries/ThirdParty` folder, which Unreal does not create for a plugin installed in the engine.

## Editor

- Quitting the editor while the Convai announcements or changelog are still downloading no longer uses memory the editor has already freed, which could crash it.

## Android

- The arm64-v8a and x86_64 builds of `libconvai_client.so` and `libconvai_http_helper.so` are now 16 KB page-aligned, so an app that includes the plugin passes Google Play's 16 KB page-size check for apps targeting Android 15 or later, and the libraries load on devices that use 16 KB memory pages. Before, both were 4 KB-aligned: Google Play rejected the app, and on a 16 KB device the app could not load them. The 32-bit armeabi-v7a libraries are unchanged, because the requirement covers 64-bit libraries only.
- Google Play's check also covers the engine's own libraries, which the plugin cannot fix: on UE 5.5 it flags `libswappy.so`, and on UE 5.3 `libUnreal.so`. UE 5.4 and UE 5.6 or later pass with their default settings.

## Known issues

- Cloud Avatars: after a failed **Upload changes** leaves some of an avatar's files in use, changing one of those files anyway stops the editor's autosave at that file each time it runs, so the files after it in that autosave are not saved until you restart Unreal. **Save All** still saves everything else.
- Cloud Projects: while **Upload changes** or **Create and upload** is packaging, Unreal's own **Package Project**, **Cook Content** and **Zip Project** stop at once with "A conflicting instance of AutomationTool is already running". So can one you queued behind another Unreal task before you started the upload, because the waiting upload starts first; on UE 5.3 that case is new with this release. Start them again after the upload has moved past Package.

# Release 4.0.0-beta.33

## Highlights

- The bundled native client is updated from 0.2.15 to 0.2.16. Typed text, narrative triggers and context updates are queued instead of making the game thread wait for each send.
- CC4 Extended lip sync: face frames now carry CC4 curve names, such as `Mouth_Drop_Lower`, so an anim graph that reads curves by name can drive a CC4 rig. Since the mode was added, they carried MetaHuman (or ARKit) curve names.
- The character's voice is pulled by the audio engine as it plays, so a game-thread hitch or a very low frame rate no longer leaves gaps in it.
- **Update Context** text and **Dynamic Environment Info** are no longer erased by a later **Set Context State**, spatial fact or action change, and are sent again after a reconnect.
- A player carrying the Pixel Streaming capture keeps its microphone when the game runs without Pixel Streaming, such as a Cloud Projects project played in PIE.
- C++ projects on Windows that use an Unreal Engine installed from the Epic Games Launcher no longer recompile the plugin for Development Editor builds when the release is extracted fresh into the project's `Plugins` folder; they use the editor binaries it ships with.
- Cloud Avatars can upload grooms with Unreal's built-in hair physics (UE 5.8, Windows uploads), and source uploads and downloads work on an SDK unzipped from a browser download.
- The Scene Auto Tagger now ships for UE 5.3, 5.4 and 5.5.

## Upgrade notes

- CC4 Extended lip sync: with the project's **Lip Sync Mode** on **Auto** (the default) and a **Convai Face Sync** set to **CC4 Extended Blendshapes**, face frames now carry CC4 curve names. An anim graph or **Blendshape Mapping** keyed on the MetaHuman `CTRL_expressions_*` (or ARKit) names those frames used to carry must switch to the CC4 names; entries keyed by the old names no longer match anything. Characters driven through other lip sync modes are not affected.
- Tracked properties: a Blueprint-enum property now reaches characters as the entry name (`Open`, not `NewEnumerator1`), and an un-aliased Blueprint struct member by its member name (`Door.Stats.Health`, not `Door.Stats.Health_12_<GUID>`). Update **State Value Descriptions**, Watch Property targets and **Get Context State Value** keys that use the old names, and any Blueprint that compares **Get Context State Value**'s result with an old enum name. Set an **Alias** for a key that must not change.
- Action handlers: a Custom Event or function named after an action that takes an `FConvaiResultAction` plus another input, an output or a return value is now skipped with a warning. Give it the single `FConvaiResultAction` input only.
- **Auto Initialize Session** on a player component on a pawn now waits until a local player controller possesses the pawn. Call **Start Session** yourself for a pawn that must start a session without being possessed.
- Call **Set API Key**, **Set Auth Token**, **Set Custom Param**, **Set VAD Settings** and the C++ `UConvaiSubsystem::SendAudio` only on the game thread. From another thread they now raise an ensure.
- LTM and Knowledge Bank nodes now fire **On Failure** after 60 seconds in total, retries and uploads included, and when the world that started them is cleaned up. Make sure Blueprints handle that pin, especially for large Knowledge Bank uploads.
- Native C++ subclasses of `UConvaiAPIBaseProxy` must be rebuilt, and some need a source change (see API requests).
- C++ projects: delete the old Convai plugin folder before extracting this release, because object files from an earlier local build keep the plugin compiling from source. To build the plugin from source on purpose, delete `"Installed": true` from `ConvAI.uplugin`.
- **Lower Face Smoothing Speed** and **Upper Face Smoothing Speed** on the **Convai Face Sync** anim graph node take 0 to 1, where 1 means no smoothing; the old tooltip's 5-15 was wrong. A value above 1 set through a pin, a `-LipSyncAnim*` switch or **Set Custom Param** now acts as 1, so set it below 1 if you want smoothing.
- A custom `IConvaiAudioProcessingInterface` now receives `SetProcessedAudioReceiver(nullptr)` when the player lets it go, and must then stop calling the old receiver.
- Cloud Projects: **Upload changes** and **Create and upload** now save every unsaved map and asset without asking which, so discard edits you do not want saved first.

## Connection

- The bundled native client (`convai_client`) is updated from 0.2.15 to 0.2.16. Messages the game thread sends (typed text, narrative triggers, context and scene updates, template keys, speech-to-text toggles and idle-timer renewals) are now queued and sent in the background, in order. Before, the game thread waited for each send.
- A queued message counts as sent: typed text interrupts the character and counts as player activity as soon as it is queued. Up to 64 messages can wait. Messages still waiting when the session disconnects are dropped, so text sent in the same frame as **Stop Session** may not arrive. A refused text, narrative trigger, speech-to-text toggle or idle-timer renewal logs "The message queue did not accept ...", and **Reset Idle Timer** returns false when its renewal is refused.
- A missing `convai_client.dll`, or one that fails to load (for example because antivirus blocked it), no longer crashes the game at **Start Session**. The plugin logs an Error naming the library, and **Start Session** refuses to connect.
- Fixed rare crashes when a session ends or restarts while the native client is still calling back. Ending a session and draining a push-to-talk release no longer tie up the engine's worker threads for seconds.
- The plugin no longer reconnects a session the server ends with `session-ending` and `reconnectable: false`, even with **Session Reconnect Attempts** above 0. If the server closed such a session for inactivity, **Idle Behavior** still applies, except that **Keep Session Open** acts like **Reopen When Player Returns**.
- A chatbot refuses **Start Session** while it is ending play or being destroyed, until its next Begin Play, so a game that restarts its session on Disconnected while a level unloads no longer opens a new one. **Stop Session** called from **Gather Environment Extras** during a reconnect now stops the reconnect.
- A narrative trigger or context update sent from **On Attendee Connection State Changed Event** when it reports Connected after a reconnect now belongs to the restored session. Before, after a long outage, the trigger was dropped and the context was sent without asking for a reply.
- Custom params the plugin cannot use are replaced where it uses them, with one warning per key: a `ClientReadyRetrySecs` below 0.1 s becomes 0.1 s, a `ClientReadyTimeoutSecs` below 5 s becomes 5 s, a negative `ObjectPollIntervalSeconds` becomes 0.25 s, and a `ChunkSize` or `OutputFPS` of 0 or less becomes 10 or 90. **Get Custom Param** still returns the value as set.
- `-ConvaiProdURL=""` or `-ConvaiBetaURL=""`, or either flag left empty at the end of the command line, no longer clears a URL set in Project Settings.
- Vision refuses frames the native client cannot read (an odd side, a side under 16, or a short buffer), with one warning per connection, and the first frame is now sent. Only the connected character's chatbot can send frames, so a chatbot another character displaced no longer sends its camera to the new character.
- Also fixed: a Blueprint that starts or stops a session from inside a getter the connect reads can no longer start a connection on a client being destroyed; ending play during a reconnect backoff no longer logs "DisconnectSession: Character session is not the current active session"; the editor no longer reads memory it had just released on its first frame, which could crash it under memory checkers such as `-stompmalloc`.

## Push-to-talk

- A timed-out release, or one that fails for any reason other than the three below, no longer turns push-to-talk off until the next connection; the next press works. This replaces the 4.0.0-beta.32 note.
- A release the server answers with `user_input_commit_timeout`, `stt_reset_failed` or `stt_reconnect_failed` still turns push-to-talk off for that session, because the session takes no more voice, and the first refused press logs a warning. With **Session Reconnect Attempts** above 0, the plugin now ends such a session and starts reconnecting it within a second, and push-to-talk works again once it is back. At 0, push-to-talk stays off until the game starts a new session.

## Microphone

- A player carrying the **Convai Pixel Streaming Audio Capture** component now keeps the built-in microphone when the game runs without Pixel Streaming (no `-PixelStreaming`, `-PixelStreamingURL=` or `-PixelStreamingIP=`), as in PIE. In beta.32 such a player, including any that Cloud Projects' Prepare set up, had no microphone in those runs.
- With **Auto Initialize Session** on, a player component on a pawn starts its session only once a local player controller possesses the pawn. Unpossessed pawns, other players' pawns and AI-possessed pawns no longer start one. Possessing a pawn again gives the microphone back to that pawn's player component, so push-to-talk bound to it keeps working.
- A player component that a newer player session displaced now stops capturing, and its push-to-talk messages no longer reach the server (beta.32 already refused its audio). When the newer owner leaves, the microphone goes back to a displaced component, which streams again if it was streaming before. **Unmute Streaming Audio** on a displaced component returns false with a warning.
- After a game-thread stall, an open microphone sends only the newest 0.2 s of the audio that piled up, and a push-to-talk hold up to 1.5 s, so a stall no longer leaves the player's voice permanently delayed; a game drawing steadily at a low frame rate still sends all its audio. Time dilation no longer delays the microphone. Both apply unless the `MicCaptureTap` custom param is `Recorder`. Speech the built-in microphone captures while the world is paused is dropped instead of reaching the character late.
- When only partial transcripts show the player talking, **Is Speaking** turns off and **On Finished Talking Delegate** fires 10 s after the last one. Talking the server started still waits for the server's stop or a final transcript, as before, unless the player mutes, stops streaming or starts a mic test during it; the same 10 s limit then applies. A session that ends while the player is speaking now fires **On Finished Talking Delegate** once.
- The microphone health check no longer gives up after 10 restarts. It keeps retrying, waiting longer each time up to 30 s, so a microphone connected later is picked up; on Android it waits for the microphone permission first. On a machine with no working microphone it warns once per run of failures instead of on every restart.
- A gain set with **Set Microphone Volume Multiplier** before the player adopts a capture component, or a saved gain other than 1.0, now carries over to the adopted component. The Pixel Streaming capture doubles the gain it is given, so a saved gain of 1.5 streams at 3.0. If an adopted component is destroyed while in use, the player switches back to the built-in microphone.
- **Save Microphone Settings** and **Save Microphone Settings From Player Component** clamp the gain to 0-2 (a non-finite gain becomes 1.0). When the player follows the system default, **Save Microphone Settings From Player Component** saves no device name, which means the system default, instead of the current default device's name; it does the same when you save while the chosen microphone is unplugged.
- The chosen microphone is remembered by its device ID, so plugging in another device no longer makes the player open a different microphone. If the chosen one is unplugged, the player uses the system default and goes back to the chosen one the next time it opens the microphone after the device returns. **Get Available Microphone Names** now leaves out output-only devices, so its indices can change.
- Shipping builds no longer write `Saved/AudioDebug/recorded_audio.wav`, and ending play during a mic test no longer writes it in any build. During a live stream, **Start Recording** is refused while the push-to-talk key is held, and a push-to-talk press is ignored with a warning while a mic test runs. **Mute Streaming Audio** no longer silences a running mic test.
- The Convai Player no longer changes the engine's `voice.MicNoiseGateThreshold` and `voice.SilenceDetectionThreshold`. Set them yourself if you relied on the values it used to set (0.01 and 0.001).
- An audio processor's processed-audio callback may now run on any thread; the player copies the chunk and handles it on the game thread.

## Voice playback

- The chatbot's voice is now pulled by the audio engine as it plays, instead of being handed over by the game thread at most 150 ms per frame, so a game-thread hitch or a very low frame rate no longer leaves gaps in it.
- A stop that comes from the network, such as the player interrupting the character, no longer races the game thread, so no part of the cut reply can play after the stop, the character cannot start talking again for the old reply, and the next reply keeps its start buffer (`MinBufferDuration`, 0.2 s by default).
- Stopping the character's voice while it plays no longer races the audio render thread for the queued audio, which could corrupt memory and crash later.

## Lip Sync

- Fixed: CC4 Extended face frames now carry CC4 curve names, such as `Mouth_Drop_Lower`, so an anim graph that reads curves by name can drive a CC4 rig. Since the mode was added, the plugin picked each face frame's curve names by the frame's size, so CC4 frames carried MetaHuman `CTRL_expressions_*` names and did not move a CC4 rig.
- The CC4 fix applies when the project's **Lip Sync Mode** (Project Settings ▸ Plugins ▸ Convai) is **Auto**, the default, and the character's **Convai Face Sync** component has **Lip Sync Mode** set to **CC4 Extended Blendshapes**. In CC4 Extended mode the component's rest pose is now every CC4 curve at 0; it was the viseme rest pose.
- A face frame with more values than its format has curve names no longer crashes a Development build or reads past the end of the name list in Shipping. The extra values are dropped, and the log warns once per connection when a frame's size does not match its format.
- A bound **Convai Face Sync** or vision component that is destroyed while the chatbot keeps running is dropped on the chatbot's next tick. Before, the chatbot kept calling into it after garbage collection, which could crash. A lip-sync provider that is not a UObject, assigned to `ConvaiLipSync` in C++, stays bound but must stay alive until the tick after it is cleared.
- A chatbot without a lip-sync component, and a **Convai Face Sync** anim graph node without a chatbot, now repeat their search on the game thread at most once a second. Before, the chatbot searched its actor's components every frame and again on a network thread for every audio chunk, and the node searched on an animation thread every frame. A lip-sync component or chatbot added later can take up to a second to be found.
- **Lower Face Smoothing Speed** and **Upper Face Smoothing Speed** are now clamped to 0-1 at runtime too, where 1 means no smoothing; the Details panel already limited them. A value above 1 from a pin or a `LipSyncAnim*` override, such as the 5-15 the old tooltip suggested, made face curves overshoot or turn to NaN while that smoothing was on. The tooltips now describe the 0-1 range.
- The **Convai Face Sync** anim graph node reads its `LipSyncAnim*` overrides on the game thread when its anim instance initializes, and ignores a value that reads as NaN or infinity. An override set later with **Set Custom Param** applies the next time an anim instance initializes.
- Stop, pause, resume and end-of-audio calls reach a lip-sync component only on the game thread; one that starts on a network thread, such as the stop when the player interrupts, applies on the chatbot's next tick. With **Enable Interpolation** on, the component reads `ConvaiGetFacialDataNames` once per Lip Sync Mode, so a C++ override that returns a different list without a mode change is not picked up.

## Actions

- C++ implementations of `IConvaiConnectionInterface::OnActionSequenceReceived` are now called on the game thread, and object and character references in an action's parameters (`RefValue`) are resolved when the action arrives, so `ResolvedComponent` and the position are filled in as documented.
- An action handler that takes an `FConvaiResultAction` as its first input but also has another input, an output or a return value is now skipped with a warning, and the action goes to the next candidate (owning actor, objects added with **Add Action Handler**, the chatbot component, then the built-in actions). Before, such a handler was called anyway, and its extra pins read and wrote memory outside the action's data. A handler whose first input is something else was already skipped.
- Packaged builds no longer offer characters action Enum entries marked Hidden, and native C++ enums offer their display names there. The editor saves each Enum parameter's labels (`FConvaiActionParam.SavedEnumLabels`) when it saves or cooks an asset; Enum parameters built at runtime keep the old behaviour.
- **Get Param As Number** and an action parameter's `NumberValue` read 0 for text that reads as infinite or NaN; the deprecated **Get Action Param As Number** also logs a warning. **Get Param As Byte** returns the value of the entry the character picked even when an earlier entry is Hidden, and its tooltip now says it returns the entry's value, not its index.
- **Convai Move To** now ends with **Missing AI Controller** and "You cannot move right now." when its pawn is unpossessed or destroyed, including when play stops mid-move. Before, the move never ended. A **Convai Escort** whose pawn is lost while it is walking passes the same note on, with **Invalid Escort Setup**.
- **Convai Move To** finishes with **Reached** when Unreal reports success but stops the pawn at the end of a partial path, outside Convai's arrival tolerance, for example beside a goal just off the navmesh. Before, it retried once and then finished with **Move Failed**.
- When a disconnect cuts off a running action, the next action plan, including the first after a reconnect, waits until the cut action's handler reports back with **Handle Action Completion** or **Handle Action Cancellation**, or 10 s pass. Before, that late report completed or failed the next plan's action. A failure reported with a **Delay** now reaches the character even when a new plan starts during the delay; it then arrives silently, without asking for a reply.

## Objects

- Spawning actors with Convai Object Components or chatbot components in quick succession, or turning Convai Objects on with **Set Convai Object Enabled** more often than the poll interval, no longer stops tracked properties, movement and spatial facts from updating.
- Blueprint-enum tracked properties reach characters by entry name and un-aliased Blueprint struct members by member name, in the editor and in packaged builds alike (see Upgrade notes). Two members that would share a name keep their full stored names.
- Destroying or disabling one member of a merged object set (**Merge With Same-Named Objects**) no longer removes the set's tracked-property values from every character. A value a remaining member also tracks is kept and sent again from that member, without firing a Watch Property.
- Tracked-property values an object sends when it registers no longer fire a Watch Property the character armed. A Convai Object re-enabled after its name came free takes that name instead of keeping a numbered suffix ("Crate 2").
- **Add Object** and **Add Objects** add Convai Object Components only for the entries they add. An entry that C++ writes into the chatbot's `EnvironmentData.Objects` directly gets its component at Begin Play or from **Ensure Object Components For Environment Objects**, no longer from the next Add Object call.
- Spatial awareness runs up to 16 reachability pathfinds per poll across all characters (64 a second at the default 0.25 s poll); a single check that needs more runs on its own. Checks that do not fit wait for later polls, oldest first. Until a subject's first check runs, its fact says where it is but not whether the character can walk there, and a subject that goes out of sight keeps its last walking verdict.
- With **Enable Line of Sight** on, a merged object set counts as seen when the trace reaches any of its members.
- A tracked Blueprint function that destroys actors with Convai Object Components no longer crashes the object poll.

## Dynamic context

- **Update Context** text is now kept and sent again with every managed context update. A later **Set Context State**, spatial fact or action change no longer erases it, a reconnect sends it again, and text sent before the session is ready reaches the character once it is. Append, Replace and Reset work on the kept lines, and **Reset Dynamic Context** clears them. Each line is kept once, so a line sent again moves to the end, and only the newest 16,000 bytes are kept, with a warning.
- **Dynamic Environment Info** now travels in every managed context update (`update-dynamic-info` is no longer sent), so a later **Set Context State**, spatial fact or action change no longer erases it; an **Update Context** with Replace or Reset still replaces it until the next managed update. A change made mid-session goes out with the next debounced update. It is cut at 15,999 bytes with one warning; the property keeps its full value.
- The first context update of a session is sent on the character's next tick after the session becomes ready, instead of about 0.5 s later. Narrative triggers queued before ready also fire then.
- A context update that comes near the transport's 64,000-byte limit (past about 60,000 bytes of text) is trimmed as it is built: oldest events first, then the end of **Dynamic Environment Info**, then the oldest Update Context lines. States, facts and the action block are kept, and one warning says what was left out. The event history keeps its newest 32,000 bytes.
- **Set Context Fact** with the sentence the fact already has sends nothing. Before, it sent a context update, which also asked the character to react when **Should Respond** was Auto or Always.
- Tracked-property and movement changes with **Flush Immediately** on that one object poll finds now go out as one context update per character. Before, each went out separately, so the second could cut off the answer to the first.

## API requests

- Added **Max API Retries** (Project Settings ▸ Plugins ▸ Convai ▸ Connection, default 2; 0 turns retries off). **Convai List Speaker IDs**, **Convai Get LTM Status** and **Convai List Knowledge Bank Documents** retry after a connection error, a timeout, or an HTTP 408, 429, 500, 502, 503 or 504 reply, with growing waits that respect `Retry-After`. No other REST node retries, and **Session Reconnect Attempts** remains a separate setting.
- LTM and Knowledge Bank nodes now fail with **On Failure** after 60 seconds in total; beta.32 set no limit, so a large Knowledge Bank upload over a slow connection can now fail where it used to finish. They also fail when the world that started them is cleaned up (a level change, or Play In Editor stopping) or the API key or auth token changes; the server may already have carried out the request.
- Native C++ subclasses of `UConvaiAPIBaseProxy` must be rebuilt. To reject input, return false from `ConfigureRequest`, or call `MarkAPIRequestConfigurationFailure()` and return false from `AddContentToRequest` or `AddContentToRequestAsString`, instead of calling `HandleFailure()`. A subclass that handles the HTTP response itself must finish it with `CompleteAPIRequest()`.
- Every REST node fires exactly one outcome. The LTM nodes fire **On Failure** once, not twice, without an API key or auth token; **Convai Create Speaker ID**, **Convai Delete Speaker ID**, **Convai Get LTM Status** and **Convai Set LTM Status** no longer send a request after rejecting empty input; **Convai Set Knowledge Bank Document Status** fails instead of posting an empty body when none of its documents has an ID. **Convai Delete Knowledge Bank Document**, **Convai Set Knowledge Bank Document Status** and **Convai Set LTM Status** pass the server's error body to **On Failure**.
- Fixed crashes: the Convai Speech To Text nodes and **Convai Text To Speech** when a request fails without an HTTP response; **Convai Speech To Text From SoundWave**, **DEPRECEATED Convai Chatbot Get Response From SoundWav** and **Write Sound Wave to Wav File** on a Sound Wave that reports PCM data it does not hold, such as a recorded wave. **DEPRECEATED Convai Chatbot Get Response From SoundWav** also decodes imported Sound Waves, which used to crash it in the editor and Development builds.
- The Convai Speech To Text nodes read the transcript from the reply's `result` field, so quotes, backslashes and non-ASCII text arrive as themselves; a 2xx reply with no `result` field fires **On Failure** with the raw body. They and the deprecated Get Response From Audio file and From SoundWav nodes fire **On Failure** for a missing file or a null Sound Wave; before, neither pin fired.
- **Convai Fetch Narrative Sections** and **Convai Fetch Narrative Triggers** fire **On Failure** with an empty list when a 2xx reply is not a JSON array; before, neither pin fired. They, **Convai Get All Characters IDs** and **Convai List Speaker IDs** skip a list entry that is not a JSON object, with a warning.
- The three deprecated Get Response nodes fire **On Failure** for a 2xx reply that is not a JSON object, has no `text` field, or has `"text": null`; an empty `text` still succeeds with a warning. **DEPRECEATED Convai Chatbot Get Response** percent-encodes its form values, so typed text containing `&`, `%` or `+` reaches the server as typed. The From Audio file and From SoundWav nodes send non-ASCII character IDs, session IDs and class labels in full and deliver the classified tag with **Voice Response** off.
- **Convai Download Image** and **Convai Download Image using RPM Link** pick the decoder from the downloaded bytes, so a JPEG, PNG or BMP decodes whatever the URL ends in.
- **Stereo to Mono** reads the WAV's chunks instead of assuming a 44-byte header, so WAVs with extra chunks or an extensible format header convert correctly and short input no longer reads past the end of the array. A data chunk that claims more bytes than the file holds is converted up to the end of the file, and input that is not 16-bit PCM with two or more channels comes back unchanged.

## Logging

- `ConvaiLogs` now lives in the project's Saved folder as the engine resolves it, so a Shipping game writes it to the user's saved folder (on Windows `%LOCALAPPDATA%\<Project>\Saved\ConvaiLogs`) instead of its install folder. Packaged builds keep the newest 20 log files, and a file rolls over past 50 MB.
- Streamed packets such as `bot-llm-text` and the plugin's other Verbose lines reach `ConvaiLogs` only when their category is raised, for example with `-LogCmds="ConvaiSubsystemLog VeryVerbose"`. A Shipping build never writes them unless the project turns on logging in Shipping (`bUseLoggingInShipping`). A streamed packet logs a size line at Verbose and its full JSON at VeryVerbose. The end-user metadata and action config sent when connecting are logged by size, and in full only at Verbose.
- A typed prompt's reply is written as one Log line with the whole reply text instead of one line per token. A Blueprint **ConvaiLog** call writes one line to `ConvaiLogs` instead of two, and each unknown server message type is warned about once per run. The plugin's `[BlendshapeTurnStats]` summary lines appear only in a `ConvaiDebugMode` build. The server's `blendshape-turn-stats` packet, which beta.32 never logged, is now logged at Log like other packets.
- LTM and Knowledge Bank nodes log only the HTTP status code when a request fails, not the server's reply. **Convai Create Character**, **Convai Update Character**, **Convai Get Character Details**, **Convai Get All Characters IDs** and the deprecated Get Response nodes log a reply longer than 512 characters as its length plus at most its first 512 characters, stopping before any audio data.

## Build

- C++ projects no longer recompile the plugin in Development Editor builds on Windows. They use the editor binaries the compiled release ships with when the release matches your Unreal Engine version, sits in the project's `Plugins` folder and holds no object files from an earlier local build of the plugin (see Upgrade notes), and UnrealBuildTool then logs `Convai: using the compiled package's editor binaries`. Packaging, DebugGame Editor and source-built engines still compile the plugin from source.
- While the shipped binaries are in use, the debugger may not find the plugin's source files, because the shipped symbols point at the build machine's paths.

## Cloud Avatars

- Avatars whose grooms use Unreal's built-in hair physics can upload to Avatar Studio with simulation on, in Unreal Engine 5.8 with a Windows-only upload. The shared `StableSpringsSystem` and `StableRodsSystem` solvers are referenced instead of copied. In other engine versions the upload stops and asks you to keep groom simulation disabled, and an upload with **Linux package** selected is refused.
- When an avatar's mesh uses a custom animation Blueprint rather than Convai's stock MetaHuman body or face animation, **Include Convai content** is turned on automatically and cannot be turned off, so its animation behaves the same in Avatar Studio as in your project. Uploads may be larger.
- Uploads with **Include editable source**, **Download to project** and **Get latest** now work when the SDK was unzipped from a ZIP downloaded with a browser. Before, Windows refused to run the plugin's archive script from such a copy and these steps stopped with "The source archive helper could not finish".
- A referenced file missing from disk no longer gets stuck in the save prompt, or blocks the upload as unsaved, when Unreal keeps an empty placeholder for it; it goes to the missing-files review instead. New assets you have not saved still have to be saved before uploading.
- The uploader's cooking project enables Unreal's Interchange plugin, fixing a packaging error with newer assets whose saved import information names its importer classes. Interchange is not added to the avatar's plugin requirements or to your project.
- When an upload stops on a reference that still points outside the avatar's copy, the error names the asset that holds the reference and the asset it points to, and the log lists up to six such references, soft references included.
- **Upload changes** no longer asks you to confirm generated files the tool itself rewrote in an earlier successful upload, and it now removes plugins the avatar no longer uses from its plugin requirements on every update, not only when it rebuilds the avatar's copy. An environment whose instanced triangle count overflows is now flagged by the triangle limit instead of passing it.
- Environment updates: the tool deletes the old copied scene's files directly instead of loading the scene to delete it, so an update no longer gets stuck on old scene dependencies that loading it brought back into memory. Tracked files, writable Perforce files included, are checked out before they are replaced, files the new copy no longer uses are marked for delete, and if source control cannot record a change, the next **Upload changes** finishes it once source control is available. If a status check or checkout fails, the update stops with instructions and replaces nothing.
- An environment upload is no longer blocked by editor-only engine assets, such as the builder brush material, that Unreal adds when it saves the copied level. Engine assets the scene needs at runtime or for cooking are still checked and copied.
- When the copied scene is created or updated, its editor animation is rebuilt from the copied Blueprint classes instead of staying tied to older classes. The original scene and the avatar's animation settings are not changed, and if a rebuilt Blueprint still calls an outdated class, preparation stops and the log names the call.
- **Download to project** and **Get latest** no longer ask you to install plugins the avatar's plugins need only on other platforms, build configurations or targets; on Windows such a request could never be met. Fixed an editor crash when a download replaced an avatar while one of its materials was open in the Material editor, and a pending download that cannot be read or finished no longer stops **Refresh** from listing the library.
- **Cancel** works while an avatar's source is archived for upload and while a downloaded source is validated, and closing the editor during either no longer waits for the archive helper.
- Retrying a failed **Create and upload** or **Upload changes** no longer asks you to restart Unreal because the failed attempt's Control Rig or Niagara-registered enum copies stayed loaded, and **Include Convai content** now finishes for avatars whose enums Niagara has registered. Every failed **Upload changes** that had already modified files now unloads that avatar's content and closes its asset editors, not only one that was rebuilding the avatar's copy; files still in use, such as ones placed in the open level, stay loaded until you release them.
- A draft whose cloud avatar could not be linked is remembered across restarts, and the link is retried on each refresh; until it links, the draft is not offered for **Resume draft**, which could create a second cloud avatar. An upload whose transfer reported neither success nor an error is now reported as failed.
- Avatar records are saved atomically, so a crash or a full disk no longer leaves one truncated. The local avatar list names each folder whose record it rejects, including every folder that claims the same asset ID as another, and still lists the rest; before, a damaged record was skipped silently and a duplicate ID stopped the list. A record with a path too long for Unreal no longer crashes the editor at startup, and deleting an avatar's folder no longer blocks uploads of every other avatar.
- Packaging deletes each platform's cooked files after its step and older runs' output, keeping only the newest earlier run; the first run after updating can free several GB. Downloaded source zips, archive helper job folders, staged copies and partial transfer files left by a crash are deleted too.
- Thumbnails decode off the game thread, progress updates no longer rebuild the library and details panel (which keeps its scroll position and expanded **Advanced upload options**), hashing avatar files shows progress and can be cancelled, and portrait capture and Blueprint setup no longer freeze the editor with a full garbage collection.

## Cloud Projects

- **Cancel** during the Package stage stops the cook: the panel runs Unreal Automation Tool itself, ends its whole process tree, deletes the partial output, and a new upload can start at once. The cook's output, including why it failed, is in the **Output Log** under `LogConvaiCloudPackaging`. While an upload is packaging, Unreal's own **Package Project** and **Cook Content** fail at once instead of waiting, so start them after it finishes.
- **Upload changes** and **Create and upload** first save every unsaved map and asset, as Unreal's **Package Project** does. Project content (`/Game` or the project's plugins) that cannot be saved stops the upload and is named; other content is logged as a warning and its copy on disk is used.
- Prepare also adds the Pixel Streaming capture to project (`/Game`) Blueprints that inherit the Convai player from an SDK or plugin Blueprint or a C++ class, never a second one where a Blueprint or its parent already has it. When a project C++ class creates the Convai player and no `/Game` Blueprint inherits from it, Prepare warns, and you add a `UConvaiPSAudioCaptureComponent` to it in C++.
- On Windows, an archive of 2 GiB or more is refused before a version is reserved, and compressing first checks that the drive has room. On Unreal Engine 5.4 and later the archive is read back after writing, so a truncated one fails instead of uploading. On 5.6 and later, archive entries are stored uncompressed, so uploads are somewhat larger.
- A failed archive upload is sent again after 10 s and after 60 s before the run fails, and the panel shows each failed attempt.
- After packaging, **Upload changes** reads the project again and numbers the upload after its newest version, including one uploaded during the cook, for example by a teammate.
- **Make active** and an upload's activation read the project's settings from the server just before activating, so changes made on the Convai dashboard since the last refresh are kept and unsaved browser options in **Runtime settings** are never sent. An upload no longer applies the settings of whichever project is selected when its build finishes.
- Progress updates no longer rebuild the project list and details, so scrolling, typing and **Cancel** are no longer lost during an upload. Reopening the Cloud Projects tab during an upload no longer leaves it stuck, or crashes the editor if you had signed out, and **Cancel** or quitting the editor during the Compress stage no longer risks a crash.
- Fixed: the selected project losing its highlight after a search, and **Publish** and **Save draft** acting on the previously selected project's page right after you switch projects; they now wait for the new page to load.

## Scene Auto Tagger

- The UE 5.3, 5.4 and 5.5 packages now include the Scene Auto Tagger, as UE 5.6-5.8 already did; this replaces the 4.0.0-beta.30 and beta.31 notes that limited it to UE 5.6-5.8. Win64 games packaged with these engines also include its runtime module and native DLL.
- On UE 5.3-5.5 the Tagger captures objects under the level's own lighting and post-processing; the neutral capture lighting and the fill-light recovery for objects that stay too dark need UE 5.6 or later. On UE 5.3 it also does not wait for compiling materials or prestream mesh LODs before a capture.

## MetaHuman animations

- On UE 5.6-5.8, the editor's startup check for out-of-date Convai MetaHuman animations loads an even spread of at most 20 of them and stops at the first one that needs retargeting, so it finishes sooner. The prompt no longer says how many animations are affected, and **Yes** checks every animation before fixing and saving the out-of-date ones.
- When every sampled animation is up to date, the project is recorded as up to date until its MetaHuman skeleton or the plugin version changes, so out-of-date animations outside the sample are not offered for fixing in the editor. Cooking still rebuilds every out-of-date pose, so packaged builds are not affected.

## Known issues

- Cloud Projects on UE 5.3: **Upload changes** and **Create and upload** fail in the Package stage with "Packaging failed" when another Unreal Automation Tool task is already running, such as the editor's platform SDK check after startup or a **Package Project**. Wait for the other task to finish and upload again; UE 5.4 and later wait automatically.
- Cloud Projects: if you add a Convai player component to a Blueprint and upload without saving it first, Prepare may not add the Pixel Streaming capture to it, so that build has no microphone in the browser. Save the Blueprint before uploading, or upload again.
- After a failed Cloud Avatars upload that leaves some of the avatar's files in use, a later Cloud Projects upload saves those partly updated files without asking, and the next avatar upload can keep them as if they were your own edits. Restart Unreal without saving the avatar's files before a Cloud Projects upload.
- After a failed **Upload changes**, unsaved edits you make to avatar files that Unreal kept loaded are lost: the next **Upload changes** discards them without a prompt or, while the files are still in use, stops and asks you to restart Unreal without saving them. Make fixes in your project's original Blueprints, or restart Unreal without saving those files before editing them.
- If the server reports the account's usage limit in the first second or so of a session, the plugin can treat the end of the session as an ordinary drop. With **Session Reconnect Attempts** above 0 it then retries until the attempts run out. Keep the default of 0 if the account may be near its usage limit.

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
