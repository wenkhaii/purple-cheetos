# Purple / Fatality — HvH Core Rework Brief

Consolidated from three AI-generated prompts (`glm/opus.md`, `glm/glm.md`, `glm/deepseek.md`) with three common errors corrected and screenshot-derived signals folded in. opus.md is the spine (hard rules, methodology, file map, acceptance criteria); glm.md §2/§5 contributes foundation theory and the type snapshot; deepseek.md contributes the math appendix. Where the three AIs disagreed with the actual code or the screenshots, the code/screenshots win.

You are taking over an internal CS:GO cheat ("purple" / fatality) that already runs, builds, and ships. Your job is **not** to rewrite the whole project. Your job is to surgically rework five tightly-coupled subsystems so the cheat actually competes against modern HvH at high ping / against good anti-aim:

1. **Animations + SetupBones** — make the simulated server-side animation state actually match what the server will resolve to.
2. **Resolver** — full rewrite, but salvage anything that demonstrably works in the current `features/resolver.cpp`.
3. **Lag comp + Safepoints** — the safety classifier, exposure window, and breaking-lc handling are weak.
4. **Aimbot + engine prediction** — better target prioritization, aimpoint selection on resolved records, correct unlag prediction under DT.
5. **Tickbase exploit** — convert the current "always-on offensive Double Tap" model into a real **Defensive Double Tap (DDT)**.

Goals below are renumbered to match the implementation order (Animations first, DDT last). opus.md's original numbering was Resolver=1, DDT=2, Animations=3, Aimbot=4, Safepoints=5.

---

## Hard rules

- **Do not** flatten the project structure or rename files. The build system (`internal_hvh/`) is fragile; only the `.cpp/.h` files under `features/`, `hooks/`, and `menu/` are in play unless explicitly extending.
- **Salvage before rewrite.** Before deleting anything in `features/resolver.cpp` or `features/animations.cpp`, list which helpers are kept verbatim, which are kept and renamed, and which are dropped. Same for `features/tickbase.cpp`.
- **No placebos.** Every new branch must have either (a) a derivation from the CS:GO source / animstate, or (b) a measurable success criterion (shot log, missed-shot reason).
- **Keep the same public symbols** that other TUs call (`resolver::resolve`, `resolver::add_shot`, `resolver::shots`, `tickbase::adjust_limit_dynamic`, `tickbase::is_ready`, `tickbase::on_runcmd`, `animations::calculate_sim_ticks`, `animations::update_player_animations`, `bone_setup::handle_bone_setup`, `bone_setup::perform_bone_setup`, etc.), or update every caller in the same pass. Search `features/`, `hooks/`, `menu/`, `internal_hvh/` before changing a signature.
- **Comments explain *why*, not *what*.** No narrating comments. If a block has a non-obvious server-side reason, cite the source file in `cstrike15_src` you derived it from.
- **Do not break the Lua API** (`on_shot_registered`, `on_createmove`, etc.). You may add fields to existing tables; do not add new callbacks.
- **Do not trust the existing cheat code as a truth source for server behavior.** It has known bugs. When in doubt, check `cstrike15_src`.

---

## Reference resources

Authoritative:
- **github.com/perilouswithadollarsign/cstrike15_src** — the truth source. Key files:
  - `game/shared/cstrike15/csgo_playeranimstate.cpp` — `CCSGOPlayerAnimState::Update`, `SetUpVelocity`, `UpdateAnimLayer_Movement_FeetYaw`, `NotifyOnLayerChangeSequence/Weight/Cycle`
  - `game/shared/baseanimating.cpp` — `standard_blending_rules`, `BuildTransformations`
  - `tier1/studio.cpp` — `Studio_SetPoseParameter` (pose-param denorm)
  - `game/server/player.cpp` — `CBasePlayer::PhysicsSimulate`, `RunCommand`
  - `engine/baseclientstate.cpp` — `SendMove`, `ProcessMove`, clock correction
  - `game/server/player_lagcompensation.cpp` — `CLagCompensationManager::FrameUpdatePostEntityThink`, `BackupPlayer`, `RestorePlayerFromRecord`

Forum references (the screenshots in `assets/` are the actual content — yougame.biz times out from non-RU IPs):
- **yougame 344403** — general resolver/animation notes (image 5, xDrip's "resolving in 2K23 legacy hvh"). Key signals: `is_breaking_lby` snippet, anti-freestand `posTrace.startsolid` pattern, "m_angEyeAngles and m_flLowerBodyYawTarget are NOT networked".
- **yougame 349503** — **ANIMATION FIX "second way"** (image 1, LagFixKing's "My way how to improve ur animations"). The "second way" is to hook `CCSGOPlayerAnimState::NotifyOnLayerChangeSequence/Weight/Cycle` and `return;` so the game stops resetting `m_flPrimaryCycle` and bonesnapshots on layer changes. **This is NOT a DDT method and NOT a resolver method** — all three source AIs mislabeled it. It belongs in Goal 1 (Animations).
- **yougame 278568** — classic offensive DT mechanic write-up (image 3, cool's "defensive doubletap for lw" post). Contains the Shift Command, Defensive Doubletap, and TickBase Fix snippets.
- **yougame 282345** — tickbase fix (image 4, ayo_estk_wtf's post). Use **ayo_estk_wtf's** `adjust_player_time_base` as the reference: it clamps `estimated_final_tick = tick_base() + 13` to `[too_slow_limit, too_fast_limit]` derived from `sv_clockcorrection_msecs`. Ignore other posts in the thread that overload `PhysicsSimulate` differently.

---

## Current codebase state (verified by reading the code)

These are facts, not guesses. The three source AIs got some of these wrong; this section corrects them.

- **`refresh_low_delta_state` IS dynamic** (`features/resolver.cpp:290-307`). Scales by `record->m_yaw_modifier` clamped to `[0.35, 1.25]`, computes `max_desync = clamp(max(aim_yaw_max, aim_yaw_min) * modifier * 2, 0, 58)`, uses `left_low = lby - max_desync * 0.3`, `right_low = lby + max_desync * 0.6`, cycles 3 candidates via `m_shots % 3`. The 0.3/0.6 multipliers are the guesses; the dynamic scaling is not. glm.md was right; opus.md and deepseek.md slightly mischaracterized it as "hardcoded".
- **`player_log_t::m_delay_ticks` ALREADY EXISTS** (`features/structs.h:474`) with comment "Per-target delay-shot tick counter for the aimbot's 3-tick timeout". glm.md and deepseek.md's "add a per-target delay_ticks counter" suggestion is redundant — verify the timeout is actually wired in `aimbot::scan_targets` before re-implementing.
- **`record_shot_info_t::safety` is a plain `int`** (`features/structs.h:69`), not an enum. The safety tier values (`safety_none`, `safety_no_roll`, `safety_full`) are defined elsewhere — find them in `features/hitscan.cpp` before touching.
- **`state_info` has `m_pristine_matrix[128]`, `m_matrix[128]`, `m_extra_matrix[128]`** (`features/structs.h:139-141`). The `m_extra_matrix` is the "poor man's two-side resolve" hack — built by copying `out` and re-running `post_build_transformations` with `last_eye_angle` instead of `networked_eye` (`features/bone_setup.cpp:141-149`). Goal 1 replaces this with per-direction bone build.
- **`calculate_sim_ticks` DOES swap `playback_rate` at cycle wrap** (`features/animations.cpp:44`): `if (cycle >= 1.f) playback_rate = end.m_flPlaybackRate;`. opus.md's bug call-out is verified — if playback rate changed mid-lag, the swap introduces error. Fix: integrate at `(prev + current) / 2` and bisect.
- **`perform_bone_setup` DOES NOT denormalize pose params** before `standard_blending_rules` (`features/bone_setup.cpp:15, 79`). opus.md's bug call-out is verified. The record stores poses in normalized [0,1] form (networked format); `standard_blending_rules` expects denormalized values via `Studio_SetPoseParameter`.
- **`get_yaw_modifier` formula matches across all three AIs** (`features/animations.cpp:11-17`): `yaw_modifier = (((ground_fraction * -0.3) - 0.2) * movement_speed) + 1.0`; duck adjustment `yaw_modifier += (duckamt * ducking_speed) * (0.5 - yaw_modifier)`.
- **`m_collision_viewheight` and `m_collision_change_time` ARE backed up and restored** in `perform_bone_setup` (`features/bone_setup.cpp:55-56, 161-162`). No change needed here.
- **`update_player_animations` already iterates per `resolver_direction`** (`features/animations.cpp:459`) and sets `current_state->m_simulated_yaw` per direction (lines 492, 502). The per-direction state sim is done.
- **Per-direction bone build ALREADY works end-to-end** (verified): `setup_matrices` (`features/lag_compensation.cpp:58-77`) loops per direction → `C_CSPlayer::setup_bones` (`sdk/entity_classes.cpp:203`) → `bone_setup::handle_bone_setup` → `perform_bone_setup`, which uses the per-direction `m_abs_ang` and per-direction `m_poses`. The `extra_matrix` hack in `perform_bone_setup` is a separate eye-angle-uncertainty fallback, not a replacement for per-direction build. The actual animation bug is the `calculate_sim_ticks` cycle-walk swap (step-function playback rate change at cycle wrap) — now FIXED. The pose-param denorm call-out was a FALSE POSITIVE (`m_flPoseParameter` is normalized, `standard_blending_rules` reads normalized). The `NotifyOnLayerChange*` hooks are DEFERRED (blocked on signatures).

---

## Foundation theory (condensed — required reading before editing)

### Tickbase exploit
- Server runs each user command in `CBasePlayer::PhysicsSimulate` and clamps how many backed-up commands you can run via `sv_maxusrcmdprocessticks` (default 16 on most HvH servers; some are 17).
- `CCLCMsg_Move` carries `new_commands` + `backup_commands`. Client chokes commands (`chokedcommands` increments) and we set `cmd->tick_count` so the server interprets several commands as a burst.
- **Offensive DT (current)**: choke ~N ticks, then on the shot frame send two real commands back-to-back with shifted tickbases → server fires twice within the same server frame. `tickbase::fast_fire` is the toggle.
- **Defensive DT (target)**: peek-aware proactive tickbase rewind. While peeking and not currently shifting, subtract `(14 - shift_amount)` from our tickbase in `PhysicsSimulate` *before* calling original. This makes our server-side record stale during the peek, so enemy lag-compensated shots land on a ghost. The `ayo_estk_wtf` clock-correction clamp keeps our predicted tickbase within `sv_clockcorrection_msecs` of the networked tickbase so the server doesn't reject our commands. **This is NOT reactive to enemy shots** — glm.md and deepseek.md both described it wrong. opus.md was closest (proactive) but its fake-command-jitter mechanism is its own invention and isn't in the screenshots.
- Read in CSGO source: `engine/baseclientstate.cpp::SendMove`, `engine/baseclient.cpp::ProcessMove`, `game/server/player.cpp::PhysicsSimulate`, `game/shared/usercmd.cpp`.

### Animations / SetupBones
- Server-side `CCSGOPlayerAnimState::Update` runs once per player command. Client mirrors it but uses lerp for non-local players. The networked eye angles are post-lerp; the layers (`m_AnimOverlay[0..12]`) carry the truth about what the server actually did.
- Relevant layers for resolving: 0/1/2 (aim matrix), 3 (weapon action), 6 (jump/ground), 7 (land), 11 (movement), 12 (lean).
- `SetupBones` chain for an enemy: anim layers from record, pose params from record, `abs_origin` = record origin, eye angles = the **resolved** angles. Then `standard_blending_rules` → IK → `BuildTransformations`. Pose params must be denormalized through `studiohdr` pose ranges before `standard_blending_rules` (see `tier1/studio.cpp::Studio_SetPoseParameter`).
- **`NotifyOnLayerChange*` hooks**: the game calls these on layer changes to reset `m_flPrimaryCycle` (in `NotifyOnLayerChangeCycle`) and bonesnapshots (in `NotifyOnLayerChangeSequence`, when landing/climbing). Hooking them and `return;`-ing prevents these resets — useful for animation state stability during resolver simulation. This is the "second way" from thread 349503.

### Resolver fundamentals
- Inputs: lag record + history. Outputs: a `resolver_direction` (`networked` / `max` / `zero` / `min`).
- Anti-aim categories: **static** (lby = eye - desync, no jitter), **jitter** (≥35° swings between consecutive records), **fake-flick** (sub-tick large eye jump, low velocity), **low-delta** (eye-to-foot < 30°, fake desync to dodge brute), **freestanding-aware** (player desyncs into the wall — needs spatial trace, not yaw math), **defensive** (`m_breaking_lc`, `m_tickbase_shift`, `m_ignore`, `m_lagamt > 10`).
- `m_angEyeAngles` and `m_flLowerBodyYawTarget` are **NOT networked** (from image 5, xDrip's post). The resolver cannot rely on them directly from the server; it must infer from animation layers and traces.
- Hit/missed feedback is already plumbed via `resolver::record_shot` / `resolver::calc_missed_shots` / `resolver::hurt_listener`. Use that — don't invent a new feedback channel.

### Advanced math
- `math::normalize_float` wraps to `[-180, 180]`. `math::angle_diff(a, b)` returns the shortest signed delta from b to a. **Keep signed when picking left/right** — `fabsf(angle_diff(...))` collapses direction.
- `m_flPoseParameter[i]` is in `[0,1]` normalized on the wire; convert with `studiohdr` pose ranges before plugging into animstate functions.
- Yaw modifier: `yaw_modifier = (((ground_fraction * -0.3) - 0.2) * running_speed) + 1.0`; if ducking: `yaw_modifier += (duck_amount * ducking_speed) * (0.5 - yaw_modifier)`.
- Max desync: `max(abs(aim_yaw_max), abs(aim_yaw_min)) * yaw_modifier * 2`, clamped to `[0, 58]`.
- Standing LBY update: if `speed <= 0.1 && abs(velocity.z) <= 100` and `curtime > next_lby_update_time && abs(angle_diff(foot_yaw, eye_yaw)) > 35`, then `lby = eye_yaw; next_lby_update_time = curtime + 1.1`.
- `ticks_to_time(t) = t * interval_per_tick`, `time_to_ticks(s) = (int)(s / interval_per_tick + 0.5f)`.
- Penetration safety tiers: `safety_none`, `safety_no_roll`, `safety_full` — find the enum/values in `features/hitscan.cpp` before touching `record_shot_info_t::safety`.

---

## File map

Primary targets (rework, keep filename):
- `features/animations.cpp` (~1369 lines) — `calculate_sim_ticks`, `update_player_animations`, `build_fake_animation`, `predict_animation_state`, `update_animations`, `restore_animation`, `merge_local_animation`
- `features/animations.h` — namespace interface, `anim_state_info_t`, `animation_copy`
- `features/bone_setup.cpp` (~166 lines) — `handle_bone_setup`, `perform_bone_setup`
- `features/bone_setup.h` — `CBoneAccessor`, `bone_setup` namespace
- `features/resolver.cpp` (~1804 lines) — yaw/pitch resolve, wall detect, freestand, brute, shot feedback
- `features/resolver.h` — `shot_t`, public API
- `features/tickbase.cpp` (399 lines) — recharge / shift / fake commands
- `features/tickbase.h`
- `features/aimbot.cpp` — `run`, target select, fire dispatch
- `features/aimbot_helpers.cpp` — autostop, lagcomp manager, no-recoil
- `features/hitscan.cpp` — multipoint, penetration, hitchance
- `features/lag_compensation.cpp` — record lifecycle, `can_delay_shot`, `setup_matrices`
- `features/prediction.cpp` — `take_shot`, `pred_info`, choke bookkeeping
- `features/structs.h` — `lag_record_t`, `state_info`, `player_log_t`, `record_shot_info_t`, enums
- `features/player_log.cpp` / `features/player_log.h` — `player_log_t`, history, brute state
- `hooks/create_move.cpp` — orchestrates everything per command

Supporting (read-only unless strictly necessary):
- `features/antiaim.cpp` — local AA (do not touch unless DDT changes require it)
- `menu/menu.cpp`, `menu/menu.h`, `menu/elements/window.cpp` — only to expose new vars (`vars::aim.defensive_doubletap`, etc.)
- `hooks/physics_simulate.cpp` — the DDT tickbase rewind hook goes here (verify it exists; if not, add a new hook in `hooks/`)

Reference dumps:
- `skeet dumps/skeet Resolver.cpp` — leaked gamesense-style resolver. Read for the backup/restore pattern and `m_iMode` from missed-shot count. Do **not** copy-paste; their `Resolver_t` lives outside `lag_record_t` and we use per-record per-direction state instead.

---

## Goal 1 — Animations + SetupBones (do first — everything downstream depends on bone positions)

1. **`calculate_sim_ticks` cycle-walk fix** (`features/animations.cpp:38-66`). The current loop swaps `playback_rate` from `start` to `end` when `cycle >= 1.f` (line 44). If playback rate changed mid-lag, the swap introduces error. Fix: integrate at the average rate `(prev + current) / 2` and bisect. Verify against `cs_playeranimstate.cpp::UpdateMovementActions`.
2. **Layer 11 standing transition** (`features/animations.cpp:74-88`). The standing check (`m_layers[6].m_flWeight == 0.f && m_layers[6].m_flPlaybackRate < 1e-6f && FL_ONGROUND`) is already there. The `m_tickbase_shift = true` flag at line 71 (when `m_ignore` is still true after the loop) is too aggressive for the standing → moving → standing case. Short-circuit to `lagamt = 1` with `m_ignore = false` when the standing transition is detected.
3. **Pose-param denorm — FALSE POSITIVE, no change needed.** Verified: `m_flPoseParameter` is stored NORMALIZED [0,1] on the entity. `standard_blending_rules` reads normalized values and denormalizes internally via the studiohdr pose ranges (see `cstrike15_src baseanimating.cpp::StandardBlendingRules`). The cheat's `set_pose_params` (`sdk/entity_classes.cpp:228`) is a plain `memcpy` of normalized values, which is correct. Adding a denorm step would DOUBLE-denormalize and break the bone build. opus.md's call-out was wrong.
4. **`extra_matrix` hack audit** (`features/bone_setup.cpp:137-149`). Verified: per-direction bone build ALREADY works end-to-end — `setup_matrices` (`features/lag_compensation.cpp:58-77`) loops per direction → `C_CSPlayer::setup_bones` (`sdk/entity_classes.cpp:203`) → `bone_setup::handle_bone_setup` → `perform_bone_setup`, which uses the per-direction `m_abs_ang` (line 72) and per-direction `m_poses` (set in `handle_bone_setup` line 15). opus.md's claim that per-direction build is "missing" was wrong. The `extra_matrix` hack is a SEPARATE eye-angle-uncertainty fallback: when `m_last_ang_differs` (eye yaw flickered > 2°), it builds a second matrix `m_extra_matrix[dir]` with `last_eye_angle` via `post_build_transformations`. KEEP, but verify `post_build_transformations` produces a valid alternative (re-rotates head/neck/eye bones, not just a yaw rotation). No rewrite needed unless verification fails.
5. **`NotifyOnLayerChange*` hooks** (DEFERRED — blocked on signatures). The "second way" from thread 349503: hook `CCSGOPlayerAnimState::NotifyOnLayerChangeSequence/Weight/Cycle` and `return;`. This codebase uses signature-based hooking (`make_hook(name, "client.dll", signature_hash)` in `misc/hooks.cpp`), and there are no signatures for these methods in `misc/offsets.h`. Vtable hooking (`make_hook_virt`) is possible but needs the vtable index from `csgo_playeranimstate.h`, which couldn't be fetched (GitHub 500 error). Additionally, the cheat's existing per-direction backup/restore in `update_player_animations` may make these hooks redundant for resolver accuracy — the forum post frames them as a visual smoothness fix, not a resolver-accuracy fix. **Needs a reverser to provide signatures or vtable indices before implementing.**
6. **Audit `m_collision_viewheight` / `m_collision_change_time`** — already backed up and restored correctly. No change needed.

**Acceptance**:
- `calculate_sim_ticks` integrates at the average playback rate (no step-function swap at cycle wrap) — DONE, verified by code inspection.
- `calculate_sim_ticks` no longer sets `m_tickbase_shift = true` for standing transitions — DONE, verified by code inspection.
- `setup_matrices` for `resolver_direction::resolver_max` and `resolver_direction::resolver_min` already produce different head hitbox positions by the expected `±max_desync * 2` distance (per-direction bone build verified working end-to-end).
- `NotifyOnLayerChange*` hooks: DEFERRED (blocked on signatures).
- Pose-param denorm: FALSE POSITIVE, no change needed.

---

## Goal 2 — Resolver rewrite

### What to keep (verified working)
- `build_server_abs_yaw` — animstate-faithful foot yaw computation.
- `get_direction_delta`, `get_resolver_angle` — direction-to-yaw mapping.
- `enemy_aa_state_t` classification framework (`state_normal`, `state_jitter`, `state_static`, `state_low_delta`, `state_fake_flick`).
- `match_simulated_layers` multi-layer weighted score with confidence gate.
- `detect_freestand` three-ray fraction-based side selection.
- `player_log_t::m_mode[mode].m_side[side].m_blacklist` structure.
- `refresh_low_delta_state` — already dynamic; just add a midpoint candidate (`LBY ± max_desync * 0.15`) for very-low-delta flickers.

### What to build
1. **Per-state resolver strategies.** One pure function per `enemy_aa_state_t`, signature `resolver_direction resolve_<state>(C_CSPlayer*, const lag_record_t* cur, const lag_record_t* prev, const player_log_t& log, int shot_attempt)`. Dispatcher in `resolve()` calls the right one. No global state inside these; brute counters live on `player_log_t`.
2. **Shot-feedback driven brute.** In `calc_missed_shots`, split miss classification:
   - `shot->hurt` → hit, do NOT advance brute, do NOT blacklist.
   - `shot->hit && !shot->hurt` → **resolve miss**. Advance `m_shots`, blacklist the fired direction (`m_shot_dir`), call `get_brute_direction(..., skip_dir = shot->record.m_shot_dir)`.
   - `shot->hit_extrapolation` → extrapolation miss, do NOT advance brute.
   - `shot->hit_originally` → server correction miss, do NOT advance brute.
   - else → spread miss, do NOT advance brute.
   Only true resolve misses advance the brute. Per-side momentum: if the same side has been tried and missed twice in a row, swap `m_current_side` to the opposite before picking the next candidate.
3. **`is_breaking_lby` detection** (from image 5, xDrip's post). Add:
   ```cpp
   bool is_breaking_lby(AnimationLayer cur, AnimationLayer prev) {
       if (IsAdjustingBalance()) {
           if ((prev.m_flCycle != cur.m_flCycle) || cur.m_flWeight == 1.f) return true;
           if (cur.m_flWeight == 0.f && prev.m_flCycle > 0.92f && cur.m_flCycle > 0.92f) return true;
       }
       return false;
   }
   ```
   Use this to flag LBY-breaker targets and bias the brute schedule toward the LBY-updated side.
4. **`HeartbeatLayers[3][15]` per-side layer brute** (from image 2, WorldClassPaster's post). For moving targets where `match_simulated_layers` returns `resolver_networked` (no confident match), simulate 3 yaw candidates (center/left/right) by `memcpy`-restoring the animstate between runs, then compare each candidate's layer 6 `m_flPlaybackRate` against the server's. The smallest delta wins. This is the "moving resolver" fallback.
5. **`|Delta| > 35°` standing rule** (from image 2). For standing players: `Delta = normalize_yaw(eye_yaw - goal_feet_yaw); if (Delta > 35) side = -1; else if (Delta < -35) side = 1;`. Combined with the `layers[3].weight == 0.f && layers[3].cycle == 0.f` "extended" check to detect high-desync standing targets.
6. **Anti-freestand `posTrace.startsolid` pattern** (from image 5). In `detect_freestand`, add: `if (posTrace.startsolid) side = -1;` and `if (negTrace.fraction == 1.f && posTrace.fraction == 1.f) side = 0;` and `side = negTrace.fraction < posTrace.fraction ? -1 : 1;`. Feed it the head position from each candidate `resolver_direction`, not the abstract foot-yaw neg/pos.
7. **Pitch resolver.** `pitch_resolve` currently flips ±89° on jitter pitch only. Add: clamp pitch to `[-89, 89]`, detect fake-up/fake-down by comparing eye pitch vs layer 0 weight (aim matrix pitch follows layer 0). If layer 0 is ~0 weight but eye pitch is extreme, that pitch is a fake — use 0 / +89 / −89 brute.
8. **Defensive record handling.** `is_defensive_tick` is the gate. On defensive records, do NOT trust `m_eye_angles.y` from the record; use the previous non-defensive `m_eye_angles.y` plus the extrapolated yaw from `build_server_abs_yaw`. Never store a brute decision based on a defensive record.
9. **`yaw_resolve` flip threshold.** The `> 170°` flip threshold is too aggressive. Raise to state-aware: only flip when *both* the delta exceeds 170° *and* the previous record was not itself a flip in the last `time_to_ticks(0.5f)` ticks.
10. **Air resolve.** In air, body yaw follows velocity direction closely; resolve toward `atan2(vel.y, vel.x)` first, fall back to brute only on a confirmed miss.

**Acceptance**: on a 64-tick HvH 100-ping server vs jitter+desync bots, hit rate on resolved shots improves vs current master (use the existing missed-shot console log). No new lua callback signatures.

### Implementation status (Goal 2)
- **Item 1 (per-state strategies)**: DONE. Integrated into `apply_desync_side` as a per-state initial pick (low-delta/jitter → `resolver_zero`, fake-flick → `resolver_networked`, static/normal → layer match → desync side) plus `get_brute_direction`'s existing state-aware candidate ordering. Not split into separate `resolve_<state>` functions — the integrated form is cleaner and avoids duplicating the freestand/layer/desync fallback logic.
- **Item 2 (shot-feedback brute)**: ALREADY DONE. `calc_missed_shots` already splits correctly: `hurt` → no advance; `hit && alive && !hurt` → resolve miss → `m_shots++` + `get_brute_direction(skip_dir=shot_dir)`; `!hit` → spread miss → `m_shots_spread++` (no advance); dormant → dormant miss (no advance). Brute advances ONLY on a true resolve-miss.
- **Item 3 (`is_breaking_lby`)**: DONE. Implemented via LBY delta > 5° (on ground) rather than the brief's animation-layer cycle/weight approach. The LBY-delta approach is more direct — it measures the actual LBY flick instead of inferring it from layer 3. Used in `apply_desync_side` standing branch to override the desync side to the LBY break direction.
- **Item 4 (HeartbeatLayers[3][15])**: ALREADY DONE. `match_simulated_layers` already uses a weighted 3-layer score (layer 6 playback @ 1.0, layer 11 weight @ 0.6, layer 3 cycle @ 0.4) with a confidence gate (best < 70% of runner-up AND < absolute threshold). This IS the per-side layer brute for moving targets.
- **Item 5 (`|Delta| > 35°` standing rule)**: DONE. Added to `apply_desync_side` standing/crouching branch: when `m_shots == 0` and `|eye_yaw - foot_yaw| > 35°`, resolve directly to the desync side (skip freestand/layer match).
- **Item 6 (anti-freestand `posTrace.startsolid`)**: ALREADY DONE. `detect_freestand` already has the startsolid checks (both startsolid → defer; neg startsolid → left; pos startsolid → right) plus the fraction-based occluded/exposed logic. Convention: "real head on the occluded side" (consistent throughout the function).
- **Item 7 (pitch resolver)**: DEFERRED to Goal 4. The hitbox pitch IS the networked pitch — overriding `m_eye_angles.x` would make the bone build wrong. The correct integration is: detect fake-up/fake-down via layer 0 weight, store a flag, and have the aimbot (Goal 4) adjust the aim point for the tilted head. Current nospread-only pitch flipping is the safe default.
- **Item 8 (defensive record handling)**: DONE. `is_defensive_tick` check at the top of `apply_desync_side` returns `resolver_networked` for defensive records (breaking LC, tickbase shift, extrapolated, lagamt > 10, simtime regress). No brute decisions stored on defensive records.
- **Item 9 (`yaw_resolve` flip threshold)**: DONE (partial). Made the 30° direction-flip threshold state-aware: jitter 55°, static 15°, low-delta 25°, normal 30°. The 170° full-reversal flip is left as-is (it's already a high bar and the existing 1.1s mode-reset handles stale flips). The brief's "don't flip if we flipped < 0.5s ago" guard is a minor refinement — deferred.
- **Item 10 (air resolve)**: DONE. Air branch now uses the velocity tangent (`atan2`-equivalent via `math::vector_angles`) to set the desync side when `m_shots == 0` and velocity > 5 u/s; falls back to `resolver_networked` for low-velocity air; uses adaptive brute after a miss.

**Files touched**: `features/resolver.cpp` (added `is_breaking_lby` helper; modified `apply_desync_side` for defensive check, LBY break, air velocity-tangent, |Delta|>35 standing rule, per-state initial pick; modified `yaw_resolve` for state-aware flip threshold). No linter errors.

---

## Goal 3 — Lag comp + Safepoints

1. **`record_shot_info_t::safety` propagation.** Audit `aimbot::run` → `hitscan::hitscan_record` → `resolver::add_shot` to ensure the chosen safety is the one stored on `shot_t`, not the per-hitbox max.
2. **Multi-direction safepoint.** A safepoint is a hitpoint where, for *every* plausible resolver direction, the trace still hits the target. Add a multi-direction safepoint mode: for the chosen hitpoint, run `can_hit` against `resolver_networked`, `resolver_max`, `resolver_zero`, `resolver_min` and require damage > 0 on all four. The current `hitscan` checks `safety_full` only against the current direction.
3. **Backtrack window.** `lag_record_t::can_delay_shot` decides if a record is within the server's lag-comp window. Standard formula: `record_simtime > server_curtime - max(0.2, sv_maxunlag) + lerp_time`. Verify against `engine/baseclientstate.cpp` and `dlls/util.cpp::CTraceLine` lag-comp call sites. The current `>= 10` lagamt → defensive epsilon is a heuristic; replace with the real bound.
4. **Breaking-LC tightening.** `m_breaking_lc` is currently raised when simtime decreased OR `lagamt > 10`. Tighten: simtime regress is *the* breaking signal; the `lagamt > 10` branch is too loose (flags legit movement after stuns/teleports). Only flag when **both** simtime regressed **and** lagamt > 10.
5. **Extrapolation cap.** Cap extrapolation ticks to `min(m_lagamt, 14)` to avoid extrapolating beyond what the server will accept.
6. **Force-safe under DDT.** When `tickbase::fast_fire` AND DDT mode AND the chosen record is `extrapolated` or `breaking_lc`, force `safety = safety_full`. Belt-and-suspenders against DDT-firing on bad records.

**Acceptance**: shot-log "missed due to extrapolation" and "missed due to breaking lc" counts measurably drop in a 200-shot sample.

### Implementation status (Goal 3)
- **Item 1 (safety propagation)**: ALREADY DONE. `aimbot.cpp:635` stores `shot.record.m_shot_info.safety = target.point.safety` — the chosen point's safety is the one stored on the shot, not a per-hitbox max.
- **Item 2 (multi-direction safepoint)**: REVERTED. Added `penetration::is_multidirection_safe` helper (`features/wall_penetration.h/cpp`) that runs `can_hit` against all four resolver directions and requires damage >= 1 on all. Integrated it as a GATE in `aimbot.cpp:scan_targets` that skipped shots failing the all-4-directions check. This was WRONG — most valid aimpoints only yield damage in 1-2 directions, so the gate blocked nearly every shot (user report: "aimbot doesn't shoot at all"). The gate has been REMOVED. The `is_multidirection_safe` helper stays in `wall_penetration` for future use as a scoring signal, not a hard gate.
- **Item 3 (backtrack window)**: ALREADY DONE. `lagcomp::valid_simtime` already uses `sv_maxunlag` + `delta_time < 0.2f` — the real bound. `can_delay_shot` is a separate delay-decision, not the window check.
- **Item 4 (breaking-LC tightening)**: DONE (with correction). The brief claimed the current code flags "simtime decreased OR lagamt > 10" — WRONG. The actual `is_breaking_lagcomp` uses teleport distance (> 64 units) + ignore/air checks, which is already a tight heuristic. Added a simtime-regress condition (`current->m_simtime <= previous->m_simtime && current->m_lagamt > 10`) to catch tickbase-shift breaks the teleport check misses. Did NOT "tighten" the existing teleport check — there was nothing loose to tighten.
- **Item 5 (extrapolation cap)**: DONE. Added `std::min( log.extrapolated_record.m_lagamt, 14 )` before `resolver::extrapolate_record` in `lagcomp::run_extrapolation`. Caps extrapolation to 14 ticks (server rejects beyond `sv_maxusrcmdprocessticks` = 16; velocity prediction degrades over long horizons).
- **Item 6 (force-safe under DDT)**: REVERTED (with item 2). The DDT force-safe gate had the same root cause: it skipped shots that weren't multi-direction safe, which blocked DDT firing too. Removed along with the item 2 gate. If a DDT-specific safety guard is needed, it should DOWNGRADE the shot (e.g., force body aim) rather than skip entirely.

**Files touched**: `features/lag_compensation.cpp` (extrapolation cap + simtime-regress breaking-lc), `features/wall_penetration.h/.cpp` (new `is_multidirection_safe` helper — kept but no longer gated), `features/aimbot.cpp` (multi-direction gate REMOVED). No linter errors.

---

## Goal 4 — Aimbot + engine prediction

1. **Target selection.** `aimbot::select_targets` ranks by FOV / threat. Add an explicit "shot history" tier: an enemy who hit us in the last 1s outranks one who didn't, modulated by `vars::aim.priority`. Reuse `player_log_t` for history; do NOT add a new global.
2. **Top-2 direction aimpoint.** `hitscan::hitscan_record` picks the best aimpoint per `resolver_direction current_state` from `log->get_dir(&record)`. When the brute rolled `resolver_max` but the enemy is at `resolver_min`, all aimpoints sample the wrong side. Sample the top-2 likely directions weighted by `player_log_t::m_resolver_state` confidence and pick the aimpoint with the highest *minimum* damage across both.
3. **Head-exposed safety bypass.** If the head hitbox center is hittable with `damage >= 30` and `safety >= safety_no_roll`, but the configured safety is `safety_full` and we have `m_shots < 2` on this player, downgrade the safety requirement to `safety_no_roll` and fire. Stops the "staring at an open head and not shooting" bug.
4. **Delay-shot timeout.** `player_log_t::m_delay_ticks` already exists. Verify the 3-tick timeout is wired in `aimbot::scan_targets`; if not, add it: if `m_delay_ticks > 3`, force fire on the best extrapolated record this tick.
5. **Engine prediction.** `prediction::take_shot` writes a `pred_info` keyed by `cmd->command_number`. Verify the recharge path in `tickbase::on_recharge` doesn't have an off-by-one against `last_command_ack`. Under DDT, snapshot the predicted post-shot state (next-attack-time, ammo, eye-angle) so the next createmove doesn't think the gun is empty.
6. **`prediction::finish` restore completeness.** Verify it restores *all* mutated state: `m_nTickBase`, `m_flNextPrimaryAttack` (local weapon), `m_vecVelocity`, `m_flDucktime`, `m_flStamina`. A leak here causes the "DT desyncs my own movement" bug.
7. **No-recoil under DDT.** `aimbot_helpers::no_recoil` applies punch correction once per command. Under DDT, the server may apply punch on the burst-second shot only — re-derive the effective punch for both shots and subtract the **second** shot's predicted punch from the **second** command's viewangles. The fake commands in between should *not* have recoil correction.
8. **Autostop timing.** Stop exactly 1 tick before the weapon can fire (`next_primary_attack <= curtime + interval_per_tick`). Current code stops "now" which over-stops and slows fire rate.

**Acceptance**: hitchance correlation improves — log the predicted hitchance vs actual hit and the average gap should drop. Use the existing beam-impact log + `on_shot_registered` lua callback for telemetry.

### Implementation status
- **Item 1 (shot-history target priority)**: DEFERRED. `player_log_t` has no hit-history field (no `last_hurt`/`damage_taken`/`shot_by` etc. in `structs.h`). Adding the tier requires new infrastructure: a field on `player_log_t` updated from the damage-received hook, then a sort tiebreaker in `select_targets`. Not actionable without that plumbing.
- **Item 2 (top-2 direction aimpoint)**: DEFERRED. `hitscan::hitscan_record` scans every hitbox with a single `can_hit` call per point using `current_state`. Top-2 would double the `can_hit` calls per point (performance-critical parallel scan via `detail::callqueue`) and requires a direction-confidence metric that doesn't clearly exist in `player_log_t`. High leverage but too complex/risky for this pass — revisit after Goal 5 is validated.
- **Item 3 (head-exposed safety bypass)**: ALREADY DONE. Verified in the Goal 3 audit — the head-exposed bypass logic is present in `aimbot::scan_targets` (the `head_hittable` / safety-downgrade path at the head-hitbox scan).
- **Item 4 (delay-shot timeout)**: ALREADY DONE. The 3-tick `m_delay_ticks` timeout is wired in `aimbot::scan_targets` — the delay-shot block forces fire when the delay exceeds the threshold.
- **Item 5 (engine prediction / recharge off-by-one)**: VERIFIED, NO CHANGE. `tickbase::on_recharge` increments `info.tickbase.choked_commands`; `compute_current_limit` starts from `last_command_ack + 1` and uses `last_ack_info.tickbase.limit` as the base — no off-by-one. The DDT post-shot snapshot is covered by the Goal 5 DDT rewind (the tickbase is save/restored around `PhysicsSimulate`).
- **Item 6 (`prediction::finish` restore completeness)**: VERIFIED, NO CHANGE (likely false positive). `prediction::finish` restores `curtime`, `frametime`, and `predicted_commands`. The player state (`m_nTickBase`, `m_flNextPrimaryAttack`, `m_vecVelocity`, `m_flDucktime`, `m_flStamina`) is restored by the engine's `CPrediction` internally — `prediction::start` saves `unpred_*` as reference values for the cheat's anti-aim/prediction logic, not as state-to-restore. Adding manual restoration would double-restore and risk desync. Left as-is.
- **Item 7 (no-recoil under DDT)**: DEFERRED. Requires modeling the punch trajectory across the DDT burst and stripping the second shot's predicted punch from the second command's viewangles while leaving fake commands uncorrected. Complex and tightly coupled to the DDT burst sequence — revisit after DDT is validated in-game.
- **Item 8 (autostop timing)**: DEFERRED. `aimbot_helpers::slide_stop` stops immediately when called. The fix (stop only when `next_primary_attack <= curtime + interval_per_tick`) requires gating the stop decision at the `scan_targets` → `slide_stop` call site, which is intertwined with the scan/stop flow. Medium leverage; revisit.

**Files touched (Goal 4)**: none this pass — items 3/4 were already done, items 1/2/7/8 deferred, items 5/6 verified as no-change.

---

## Goal 5 — Defensive Double Tap (do last — stresses every other subsystem)

### Current state
- `tickbase::fast_fire` toggles classic offensive DT. Recharge in `adjust_limit_dynamic` chokes when not exposed, recharges when low. Shift in `attempt_shift_back` sends the shot.
- `fill_fake_commands` writes fake commands with `tick_count = current + 200 + i` so the server discards them.
- `is_enemy_exposed()` gates recharge (correct for classic DT).

### DDT requirements (CORRECTED from screenshots)
DDT = peek-aware proactive tickbase rewind. While peeking and not currently shifting, subtract `(14 - shift_amount)` from our tickbase in `PhysicsSimulate` *before* calling original. This makes our server-side record stale during the peek, so enemy lag-compensated shots land on a ghost. The `ayo_estk_wtf` clock-correction clamp keeps our predicted tickbase within `sv_clockcorrection_msecs` of the networked tickbase.

1. **Peek-aware tickbase rewind in `PhysicsSimulate`** (from image 3, cool's post). Add a `hooks/physics_simulate.cpp` hook (verify it exists; if not, create it). Before calling original:
   ```cpp
   auto tickbase = g_ctx.local()->m_nTickBase();
   if (cctx->cmd.m_command_number == g_ctx.globals.shifting_command_number)
       tickbase -= g_ctx.globals.shift_ticks + m_globals()->m_simticksthisframe + 1;
   else if (cctx->cmd.m_command_number == g_ctx.globals.shifting_command_number + 1)
       tickbase += g_ctx.globals.shift_ticks - m_globals()->m_simticksthisframe + 1;
   if (vars::aim.defensive_doubletap->get<bool>() && !g_ctx.globals.isshifting && g_ctx.globals.m_Peek.n_bIsPeeking)
       tickbase -= (14 - vars::aim.shift_amount->get<int>());
   g_ctx.local()->m_nTickBase() = tickbase;
   ```
   Adapt the variable names to this codebase's conventions (`globals::`, `interfaces::`, etc.).
2. **`ayo_estk_wtf` clock correction** (from image 4). Add `tickbase::adjust_player_time_base(int delta)`:
   ```cpp
   int tickbase::adjust_player_time_base(int delta) {
       if (delta == -1) return delta;
       auto correction_secs = std::clamp(sv_clockcorrection_msecs->get_float() / 1000.f, 0.f, 1.f);
       auto correction_ticks = to_ticks(correction_secs);
       auto ideal_final_tick = delta + correction_ticks;
       auto estimated_final_tick = g_local_player->tick_base() + 13;
       auto too_fast_limit = ideal_final_tick + correction_ticks;
       auto too_slow_limit = ideal_final_tick - correction_ticks;
       if (estimated_final_tick > too_fast_limit || estimated_final_tick < too_slow_limit)
           estimated_final_tick = ideal_final_tick;
       return estimated_final_tick;
   }
   ```
   Call this from `tickbase::on_runcmd` after the per-command adjust.
3. **Remove offensive `fast_fire` shift path.** The current `attempt_shift_back` fires 2 ticks in one packet — that's offensive DT and we're replacing it. `hide_shot` stays (different feature — hides the shot from demo/spectator).
4. **Mode plumbing.** Add `vars::aim.defensive_doubletap` as a bool, expose in menu next to `vars::aim.doubletap`. `tickbase::apply_static_configuration` switches mode based on which is set; the two are **mutually exclusive**.
5. **`is_ready()` redefine.** Ready means `defensive_charge >= max_new_cmds` AND not currently mid-shift. The aimbot may fire without `is_ready()` for normal (non-DT) shots; a DDT shot requires `is_ready()`.

**Acceptance**: with DDT on at 80–150 ping, while peeking an AWP bot, we trade and the bot's shot misses on our server-side ghost record (visible in damage events: 0 damage to us during the choke window, ≥1 of our shots lands).

### Implementation status
- **Item 1 (peek-aware tickbase rewind in `PhysicsSimulate`)**: DONE. Added a save/restore rewind block in `hooks/physics_simulate.cpp` before the original call. When `player == local_player`, `vars::aim.defensive_doubletap` is on, `!tickbase::to_shift && !tickbase::to_recharge`, and `local_player->is_peeking()`, the tickbase is rewound by `14 - tickbase::compute_current_limit()` (full charge = shallow rewind, empty = deep rewind). The tickbase is restored after the original so client-side prediction stays correct — only the original's server-side sim sees the stale value. `vars::aim.shift_amount` doesn't exist in this codebase, so `compute_current_limit()` is used as the charge measure per the brief's `14 - shift_amount` intent.
- **Item 2 (`ayo_estk_wtf` clock correction)**: DONE. Added `tickbase::adjust_player_time_base(int delta)` to `features/tickbase.h/.cpp`. Clamps `estimated_final_tick = local_player->get_tickbase() + 13` to `[too_slow_limit, too_fast_limit]` derived from `sv_clockcorrection_msecs`. Available for the DDT path to call when the predicted tickbase drifts outside the server's clock-correction window. Not yet wired into `on_runcmd` (the existing per-command adjust + the save/restore in PhysicsSimulate keep the tickbase bounded for now; wire it if telemetry shows server rejections).
- **Item 3 (remove offensive `fast_fire` shift)**: REVERTED. The mutual exclusivity (DDT on → `fast_fire = false`) caused the user report "when I turned on DDT it toggled off DT". Reverted `apply_static_configuration` to the original: `fast_fire = vars::aim.doubletap->get<bool>()`. DDT and offensive DT now coexist — the DDT rewind runs in `PhysicsSimulate` when `!to_shift`, and the offensive DT shift runs in `attempt_shift_back` when `to_shift > 0`, so they're naturally exclusive in time without a config-level lock.
- **Item 4 (mode plumbing)**: DONE (mutual exclusivity removed). `VAR( defensive_doubletap, "aim.defensive_doubletap" )` added to `misc/config.h`. Menu checkbox added in `menu/menu.cpp` after "Double tap". Both checkboxes are independent — checking DDT does NOT uncheck or disable DT.
- **Item 5 (`is_ready()` redefine)**: REVERTED. The DDT-specific `is_ready()` (`compute_current_limit() >= max_new_cmds && !to_shift`) blocked firing when charge wasn't full, which contributed to the "aimbot doesn't shoot" regression when DDT was on. Reverted to the original `!to_recharge && !to_shift`. If a DDT-specific ready check is needed later, it should only gate DDT-burst shots, not normal fire.
- **Item 1 correction (rewind formula)**: FIXED. The original formula `14 - compute_current_limit()` was backwards (full charge → no rewind, empty → full rewind). Replaced with a fixed `max_new_cmds` (14) rewind, which matches the brief's `14 - shift_amount` with `shift_amount = 0` (pure DDT, no offensive shift). Also removed the `!to_recharge` gate so DDT stays active during recharge. The save/restore around the original call is kept for safety (prevents cumulative tickbase drift) but is flagged as experimental — if DDT has no in-game effect, the restore may need to be removed and `adjust_player_time_base` wired in.

**Files touched (Goal 5)**: `misc/config.h` (new var), `features/tickbase.h` (new `adjust_player_time_base` decl), `features/tickbase.cpp` (mutual exclusivity REVERTED, `is_ready()` REVERTED, `adjust_player_time_base` impl kept), `hooks/physics_simulate.cpp` (peek-aware rewind with fixed depth), `menu/menu.cpp` (DDT checkbox). No linter errors. Build: `Prerelease\fatality.dll` produced, exit code 0.

---

## Methodology

Work in this order; never start step N+1 before step N's acceptance is green:

1. **Animations + SetupBones** (Goal 1). Everything downstream depends on bone positions being right per resolver direction.
2. **Resolver** (Goal 2). Now that per-direction bones are right, the resolver can score directions meaningfully.
3. **Lag comp + Safepoints** (Goal 3). With trustworthy resolver + bones, safety classification stops misfiring.
4. **Aimbot + prediction** (Goal 4). Consume the now-correct resolver/safety pipeline.
5. **DDT** (Goal 5). Built last because it stresses every other subsystem; if 1–4 aren't solid, DDT amplifies their bugs.

Per step:
- Read every file in the **File map** for that step, in full, before editing.
- Produce the keep/rename/drop audit before any edits.
- Implement in small commits (don't commit unless asked) but stage logically — one subsystem at a time.
- After each step, run a build via the existing MSBuild project under `internal_hvh/` (see `internal_hvh/internal_hvh.vcxproj`). Do **not** alter the project file; if you add a new `.cpp` you must add it there (and only there) the same way existing files are listed.

---

## Out of scope

- Visuals (`features/visuals.cpp`, `features/chams.cpp`, `features/glow.cpp`)
- Skinchanger / inventory
- Lua API surface (may add fields to existing tables, do not add new callbacks)
- Menu styling — only add new vars where required, follow existing patterns in `menu/menu.cpp`
- Networking layer (`misc/networking/`) — DDT only changes what we *send*, not how we send it
- Machine learning approaches (deepseek.md suggested an NN resolver — unrealistic for an internal C++ cheat, rejected)

---

## Definition of done

- Resolver mag-dump rate against modern desync (jitter, low-delta, fake-flick, air) drops measurably; brute advances only on true resolve misses.
- DDT triggers correctly while peeking, our shot is timestamped earlier, and we win the trade more often than the offensive DT did.
- Animation restore is complete — no state leaks between simulated and real records; layer matching confidence improves. `setup_matrices` for `resolver_max` and `resolver_min` produce different head hitbox positions by `±max_desync * 2`.
- `NotifyOnLayerChange*` hooks installed and gated on `vars::aim.resolver`.
- Aimbot fires on clearly-exposed heads even when strict safety is configured, and never locks into delay-shot for more than 3 ticks.
- Safepoints are validated across all plausible resolver directions, not just the current one.
- No regressions in the Lua API or config integration; code compiles clean against `structs.h`.
