# Zero Hour Competitive AI

**English** | [简体中文](COMPETITIVE_AI_CN.md)

The Skirmish computer-player dropdown now includes **Competitive AI** after Brutal AI. The new mode uses an independent strategy controller for the three base factions. Easy, Medium, and Brutal AI retain their original strategies. This first version supports USA, China, and GLA. Random selects only these three factions; choosing one of the nine specialized generals with this mode displays a message when starting the game.

The implementation uses the game rules actually loaded from the installation directory. It does not change unit balance or INI prices. Development validation uses the original Zero Hour 1.04 installation data and $10,000 starting cash. This is an optional, testable first version; no comparison under equal conditions has yet established that it is stronger than the original strategy.

## Launching

The build produces `build/win32/GeneralsMD/Release/generalszh.exe`. The repository-root launcher, `Run-CompetitiveAI.ps1`, runs that executable using data from an existing game installation:

```powershell
.\Run-CompetitiveAI.ps1
# For another installation directory:
.\Run-CompetitiveAI.ps1 -GameDirectory 'D:\Games\Command & Conquer Generals - Zero Hour'
```

After launching, enter Skirmish, select Competitive AI in the computer player's difficulty dropdown, and choose a base faction or Random. The program reads the existing game data without replacing executables, maps, or INI files in the Steam installation. The English name is also the fallback display text when the `GUI:CompetitiveAI` localization entry is unavailable.

## Decisions and Fair Play

The core implementation is in `GeneralsMD/Code/GameEngine/Source/GameLogic/AI/CompetitiveSkirmishAI.cpp`. `AISkirmishPlayer` invokes it only for the new mode.

- **Intelligence:** Currently visible objects and historical observations are stored separately. Once an enemy leaves vision, the controller does not read its new position, health, passengers, or orders. Mobile-unit positions become uncertain after 15 seconds, represent regional risk after 45 seconds, and expire after 90 seconds. Building positions remain in memory until their location is observed to be empty. Enemy ammunition, unobserved upgrades, and undisclosed passengers are estimated from public unit templates rather than read from private state.
- **Map resources:** Supply locations are cached from public map-preview markers and do not require scouting to discover their coordinates. Safe construction, continued existence of the supply source, and remaining stock require current vision. Hidden depletion or destruction does not alter the cached locations. Opening scouts first reveal nearby second-supply locations, then revisit starting areas.
- **Normal game rules:** The controller uses runtime template costs, construction prerequisites, production queues, hero limits, transport capacity, promotions, and ability requirements. It acts through normal builders, gathering, and command systems. The new mode skips the original skirmish scripts that provide extra cash, full-map intelligence, and instant construction. It also disables computer-difficulty health and weapon bonuses and does not receive extended gathering search radii or relaxed construction checks.
- **Timing and priorities:** All timing uses simulation frames. Strategy updates approximately every 3 seconds, economy and abilities every second, and tactics every 0.2 seconds. Ordinary state transitions have a minimum 6-second hold; defense and recovery can interrupt immediately. Gathering, power, construction, and essential production are restored before counter-production, advances, and technology investment.
- **Economy:** Initial gathering targets are 2 collectors per supply for USA/China and 6 gathering Workers per supply for GLA, including automatically provided units and queued units. GLA reserves 2 additional Workers for construction. Targets adjust gradually according to delivery cycles and congestion. Non-emergency investment reserves at least the greater of $1,000 and the actual cost of two necessary counter-units. Technology investment also budgets for initial unlocked units and emergency funds. Once a stable force is established, routine reinforcement respects that budget; necessary counters can take priority. A destroyed Command Center is rebuilt through normal rules. Builders and collectors under attack seek a safe base or move away from the nearest observed threat.
- **Combat:** Local strength estimates account for weapon damage, armor, range, health, ammunition, friendly passengers, and historical risk. The controller attacks at approximately 1.25 times the opposing strength when an escape route exists, and retreats below 0.85. Unknown areas receive armed reconnaissance first. Damaged transports, siege units, and heroes prioritize survival, with controls for focus fire, pursuit, and formation spacing. Raids are tracked by target region; two consecutive unprofitable raids suspend that region until it is scouted again. Benefit is estimated from observed target damage converted into value and may include contributions from other friendly units during joint attacks.
- **Abilities:** Unlocking and activation check actual game requirements. Reconnaissance abilities prioritize obtaining vision. Strikes use legal intelligence and public superweapon countdowns while avoiding high-value friendly units and known anti-air defenses. Direct tracking stops when an enemy target becomes hidden; ammunition already in flight continues under the original rules.

## Faction Strategies

| Faction | Opening and Midgame | Technology and Late Game |
| --- | --- | --- |
| USA | Humvees carrying Missile Defenders, Ranger scouting, and Ambulance support; maintain distance, rotate damaged vehicles, and use Laser Lock when safe | Search and Destroy from the Strategy Center; add Avengers, Tomahawks, tanks, or Raptors according to observed threats; Burton, Pathfinders, Supply Drop Zones, and relevant upgrades |
| China | Combined Gattling, Tank Hunter, Battlemaster, and Listening Outpost forces; Dragon Tanks handle suitable infantry targets | Overlords, Speaker Towers, two ECM Tanks, and siege artillery; an ECM actively disables a vehicle only when another ECM can provide cover; scouting informs MiG/Helix choices, Lotus pursues safe objectives, and Hackers are dispersed alongside investment in the Internet Center |
| GLA | Safe dual supplies and necessary tunnels; Quads, RPGs, Scorpions, and Scorpion Rockets; Technical harassment and demolition transports sized using actual damage estimates | Rocket Buggy volleys followed by withdrawal, Quads, Battle Buses, Jarmen Kell, and salvage upgrades; tunnel movement respects normal shared capacity and exit risk; Palace, Black Markets, Cash Bounty, and finishing abilities |

The nine specialized generals, campaigns, and challenge mode do not enable this controller. The first version retains the Command Center to support builder recovery and abilities. The design conditions that permit selling the Command Center have not become a default opening. Failed-route suspension and reconnaissance, along with ground and air combat evaluation, remain heuristics that require practical tuning; these thresholds are not established optimal values.

## Saving, Replays, and Diagnostics

The new mode includes observations, unit tasks, decision timers, resource locations, gathering cycles, transport missions, and failure records in saved state and CRC calculations. The original strategy continues to write its original AI snapshot layout; the new mode uses an extended version. Loading rebuilds current-visibility and owned-object lists, avoiding serialized raw enemy pointers.

Strategic transitions and construction reasons are written to the existing script debug log. Headless runs additionally print per-minute `CompetitiveAI` telemetry: cash, income, supply centers, combat units, loaded transports, technology, losses, kills, and total money earned. This output does not become part of simulation state.

## Validation and Reproduction

Build the Release executables for both Zero Hour and Generals, along with their GoogleTest executables. Current checks include 11 Competitive AI unit tests covering hidden changes not overwriting memory, declining intelligence confidence, attack and retreat thresholds, state transitions, prevention of accidental activation for specialized generals, non-empty save/load consistency, and CRC consistency. Including existing tests, Zero Hour has 17 tests and Generals has 6.

```powershell
cmake -S . -B build/win32 -DRTS_BUILD_OPTION_TESTS=ON
cmake --build build/win32 --config Release --target z_generals z_googletest g_generals g_googletest
& build/win32/Tests/Google/Release/z_googletest.exe
& build/win32/Tests/Google/Release/g_googletest.exe
```

Real-engine runtime tests use `Tests/CompetitiveAI/run_smoke_tests.py`. The script creates matches between two Competitive AI players through the normal replay loader, without injecting player commands, cheating income, or balance changes. By default, it covers 9 faction combinations across 3 original maps, running each for 600 simulation seconds, then repeats 3 combinations with the same map and seed, for 30 scenarios in total.

```powershell
python Tests/CompetitiveAI/run_smoke_tests.py `
  --exe build/win32/GeneralsMD/Release/generalszh.exe `
  --game-dir 'C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour' `
  --output build/competitive-ai/validation
```

The default maps are Tournament Desert, Final Crusade, and Tournament City, covering open terrain, choke points, and areas with more garrisonable buildings. Checks cover starting cash and objects, gathering income, combat production, successful engine completion, and telemetry repeatability with the same seed. The command above writes its report and raw output to `build/competitive-ai/validation/report.json`, `engine.log`, and `engine.err.log`. The report records the executable's SHA-256 to identify the tested version. Matching repeated telemetry is a limited repeatability check, not full network-synchronization or real saved-game recovery validation.

Final-build results: **30 scenarios, 0 check failures, engine exit code 0, and empty standard-error output**. Both players in every scenario collected income and produced combat units. Per-minute telemetry matched exactly in all three repeated scenarios. The report's SHA-256 was checked against the current executable. Logs show fully loaded USA Humvees, a fully loaded GLA Battle Bus, technology transitions for all three factions, and combat losses. China's airborne transports, demolition transport missions, ability combinations, and emergency unloading have not been independently established by this batch and still need dedicated scenarios.

Fixtures for individual matches and swapped starting positions can also be generated separately:

```powershell
python Tests/CompetitiveAI/make_smoke_replay.py build/competitive-ai/example.rep `
  --sides USA GLA --map 'tournament desert' --seconds 600 --seed 1 --swap-starts
```

## Adoption Thresholds and Outstanding Validation

These 30 scenarios are runtime tests between instances of the new strategy. They do not replace the planned 270-match comparison against the original strategy. The original strategy includes unfair scripts, and a validated fair comparison baseline has not yet been established. Consequently, no comparative win rate is reported, and the new mode does not replace the existing difficulties.

The comparison should use 9 faction combinations × 3 map categories × 5 matched seeds × swapped starting positions, for 270 matches. Both sides must have equal starting funds, rules, and fair intelligence. Report overall and per-faction win rates, confidence intervals, opening collapse rates, gathering efficiency, combat exchanges, production idle time, and match duration. Adoption thresholds remain at least 60% overall and at least 55% for each faction, with no serious fairness, stability, or compatibility issues.

Outstanding validation includes complete-match victory/defeat and endgame search, dedicated recovery-under-pressure scenarios, emergency unloading and dangerous tunnel exits, air strikes and promotion-ability combinations, play against humans, runtime regressions for the nine generals/campaigns/challenge mode, continued simulation after loading real saved games, network synchronization, and large-army simulation cost. Controller save/load and CRC unit checks have passed, but they do not replace these runtime scenarios.

The current executable is a Release build using modern MSVC. As described in the project's `TESTING.md`, retail replay CRC compatibility requires validation with the corresponding VC6 build. These synthetic fixtures have no retail CRC expectations, so passing them does not establish retail replay compatibility.
