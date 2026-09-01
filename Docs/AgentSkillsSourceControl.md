# AgentSkills in Source Control

`Content/Skills/**` is the sole intentional exception to this plugin's normal
ignored `Content/` policy. Convai AgentSkills are shipped guidance: they teach
editor agents how the SDK behaves and must evolve atomically with the APIs and
documentation they describe. Other plugin content remains ignored.

## Authoring and review

- Update from `WebRTC-Video` before editing a skill and assign one active owner
  to each skill asset.
- Author and save skills in the team's agreed Unreal Engine version. Avoid bulk
  resaves and stage only the assets intentionally changed.
- A feature PR must name every changed `Content/Skills/*.uasset` and summarize
  its instruction changes in plain text. Add, modify, or remove the relevant
  skills in the same PR as the behavior they document.
- Concurrent PRs can safely edit different skill assets. Coordinate and merge
  serially when they need the same asset.
- Before merging, load every changed skill and inspect both its Description and
  Instructions. Confirm that it teaches the final API and behavior, then
  compile/save it and check `git status` for unintended asset changes.

## Resolving a same-skill conflict

Unreal `.uasset` files are binary. Git cannot semantically combine two edits to
the same skill, and `.gitattributes` deliberately prevents a text merge.

1. Preserve both branches' Description and Instructions as readable text.
2. Keep the asset from the newly updated target branch as the starting point.
3. Open that asset in Unreal and manually replay and semantically combine the
   other branch's instruction change.
4. Compile and save the asset.
5. Inspect the registered skill again and confirm both intended changes are
   present in its Description and Instructions.
6. Build or load-test the plugin and verify that only the intended skill asset
   changed.

Never hex-edit a skill or resolve a conflict by blindly choosing `ours`,
`theirs`, `merge=ours`, or a union merge. Each of those can silently discard
another PR's guidance. If two PRs touched the same asset, merge one first and
reapply the later PR's semantic change on the new target-branch asset.
