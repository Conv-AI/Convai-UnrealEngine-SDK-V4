# Working with Convai in this Unreal project

This project uses the **Convai** Unreal SDK (conversational AI characters: speech, lip-sync, actions, scene awareness). You are connected to the Unreal Editor through its **MCP** server, so you can drive the editor with tools.

## Start here
- **List the editor tools first.** Run the MCP `list_toolsets` then `describe_toolset` to see what you can do (actor/blueprint/object/asset/scene editing, etc.).
- **Convai ships AgentSkills** — task playbooks surfaced through the editor. List and read them before building anything Convai-related:
  - Toolset `ToolsetRegistry.AgentSkillToolset` → `ListSkills`, then `GetSkills` for the relevant ones.
  - Read **ConvaiQuickStart** first for the fast path (talk + follow/move + custom actions), then **ConvaiActions** for actions, **ConvaiSceneObjects** for tracked properties and movement awareness, and **ConvaiDynamicContext** for live events, response policy, delivery, and flushing as needed.
  - Read **ConvaiPlayerAndInput** for microphone/session setup and the normalized player speaking lifecycle used by `Is Speaking` and the Started/Finished Talking events.

## Key gotchas (so you don't relearn them)
- Use the **Blueprint convenience components** (BP_ConvaiChatbotComponent / BP_ConvaiPlayerComponent), not the raw native C++ ones — the action dispatch, completion, and movement helpers live on the BP versions.
- A custom action is handled by a **Custom Event named EXACTLY the action name**, taking one `ConvaiResultAction` input. Always call **Handle Action Completion** on every path or the character locks up.
- A valid Convai **API key** must be set (Project Settings ▸ Plugins ▸ Convai) for conversations to work at runtime.

## How to work
Prefer the Convai AgentSkills and the editor toolsets over guessing. When a task matches a skill, follow that skill's steps. Keep changes minimal and compile/save Blueprints after editing them.

Repository contributors must also follow the [AgentSkill source-control and binary-conflict policy](../../Docs/AgentSkillsSourceControl.md).
