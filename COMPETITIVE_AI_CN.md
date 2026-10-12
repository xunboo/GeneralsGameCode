# Zero Hour 竞技 AI

[English](COMPETITIVE_AI.md) | **简体中文**

遭遇战的电脑玩家下拉菜单在 Brutal AI（冷酷）后新增 **Competitive AI**。新模式使用独立的三阵营策略控制器；原有简单、中等、冷酷模式继续使用原策略。首版支持 USA、China、GLA 三个基础阵营。选择 Random 时只会抽取这三个阵营；新模式搭配九位专用将军会在开始游戏时给出提示。

本实现使用安装目录内实际加载的游戏规则，未修改单位平衡或 INI 价格。当前开发验证使用原版 Zero Hour 1.04 的安装数据和 $10,000 初始资金。它是可选择、可测试的首版策略，尚无同条件比较证据证明它比原策略更强。

## 启动

编译产生的程序为 `build/win32/GeneralsMD/Release/generalszh.exe`。根目录的 `Run-CompetitiveAI.ps1` 用该程序读取现有安装目录的数据：

```powershell
.\Run-CompetitiveAI.ps1
# 其他安装位置：
.\Run-CompetitiveAI.ps1 -GameDirectory 'D:\Games\Command & Conquer Generals - Zero Hour'
```

启动后进入遭遇战，在电脑玩家的难度菜单中选择 Competitive AI，并选择基础阵营或 Random。程序读取现有游戏数据，不覆盖 Steam 安装目录内的程序、地图或 INI 文件。英文名称也作为缺少 `GUI:CompetitiveAI` 本地化条目时的显示文本。

## 决策与公平规则

核心实现位于 `GeneralsMD/Code/GameEngine/Source/GameLogic/AI/CompetitiveSkirmishAI.cpp`，由 `AISkirmishPlayer` 仅在新模式下调用。

- **情报：**当前可见对象与历史记录分开保存。敌方移出视野后，不读取其新位置、血量、乘员或命令。移动单位 15 秒后位置变得不确定，45 秒后用于区域风险，90 秒后记录过期；建筑位置保留，直到重新看到空位置。敌方内部弹药、未观察到的升级和未暴露的载兵内容使用公开单位模板估计，不通过内部状态取值。
- **地图资源：**补给点位置从地图公开的预览标记缓存，不要求先侦察位置。是否可安全施工、补给点是否仍存在、剩余资源量，需要当前视野。隐藏的耗尽或摧毁不会改变缓存的位置。开局侦察优先补充附近第二矿的视野，再复侦出生区。
- **正常规则：**使用运行时模板成本、建造前置、生产队列、英雄限制、载员容量、晋升和技能条件。通过正常建造者、采集和命令系统执行。新模式跳过原遭遇战脚本中的额外资金、全图情报和即时建造逻辑，关闭原电脑难度的血量与武器加成，也不享受采集搜索半径和弱化施工检查的待遇。
- **频率与优先级：**所有计时使用模拟帧。战略约每 3 秒、经济和技能每秒、战术每 0.2 秒更新；普通状态切换保持至少 6 秒，守家和恢复可立即打断。优先恢复采集、供电、施工和关键生产，再反制、推进、升科技。
- **经济：**初始每矿 USA/China 2 个采集单位、GLA 6 个采集工人，计入附带单位及队列；GLA 另留 2 个建筑工人。根据本方送货周期和拥堵逐步调整。非紧急投资保留至少 $1,000 或两个必要反制单位的实际成本。科技建筑同时预留首批解锁单位和应急资金；稳定编组形成后，日常补兵也遵守该预算，必要反制可优先支出。指挥中心被毁后按正常规则重建，受袭的建造者和采集单位寻找安全基地或避开已观察到的最近威胁。
- **交战：**结合实际武器伤害、装甲、射程、血量、弹药、本方载兵和历史风险估计局部战力。约 1.25 倍且有撤退路线时主动交战，低于 0.85 倍时退却。未知区域先武装侦察。受损载兵车辆、攻城单位和英雄优先保命，控制集火、追击和编队间距。按目标区域记录袭击；连续两次亏损后暂停该区域，由侦察重新打开。收益使用已观察到的目标损伤折算价值，属于估计，合围时可能包含其他部队贡献。
- **技能：**解锁与使用均检查实际条件；侦察技能优先获取视野，打击技能使用合法情报和公开超级武器倒计时，避开本方高价值单位及已知防空。敌方隐藏目标丢失后取消直接跟踪，已发射弹药继续按原规则结算。

## 三阵营策略

| 阵营 | 开局与中期 | 科技与后期 |
| --- | --- | --- |
| USA | Missile Defender 载兵 Humvee、Ranger 侦察、Ambulance 支援；保持距离、轮换伤车，安全时使用激光锁定 | Strategy Center 的 Search and Destroy；按已见威胁补 Avenger、Tomahawk、坦克或 Raptor；Burton、Pathfinder、Supply Drop Zone 和对应升级 |
| China | Gattling、Tank Hunter、Battlemaster、Listening Outpost 合成编队；Dragon 处理合适的步兵目标 | Overlord、Speaker Tower、双 ECM 与攻城火炮；ECM 只有存在另一辆掩护时才主动瘫痪；按侦察选择 MiG/Helix，Lotus 争夺安全目标，分散 Hacker 与 Internet Center |
| GLA | 安全双矿与必要隧道；Quad、RPG、Scorpion 及 Scorpion Rockets；Technical 骚扰和按实际伤害计算的爆破运输 | Rocket Buggy 齐射撤离、Quad、Battle Bus、Jarmen Kell、残骸升级；隧道按正常共享容量和出口风险调度；Palace、Black Market、Cash Bounty 和终结技能 |

九位将军、战役和挑战模式没有启用这一控制器。首版保留指挥中心，以保证建造者恢复和支援技能；允许出售指挥中心的设计条件没有转成首版默认开局。路线失败后的停止与复侦、地面和空军交战评估仍是启发式策略，需要实战调优，不能把这些阈值当作已经验证的最优值。

## 保存、重放与诊断

新模式的观察记录、单位任务、决策计时、资源位置、采集周期、运输任务和失败记录纳入保存及 CRC。原策略继续写原版本的 AI 存档布局；新模式使用扩展版本。加载后重新构建当前可见与本方对象列表，避免保存敌方裸指针。

战略切换和建造原因写入现有脚本调试日志。无界面运行额外输出逐分钟 `CompetitiveAI` 指标：资金、收入、矿区、战斗单位、已载兵车辆、科技、损失、击杀和已赚取资金。这些输出不加入模拟状态。

## 验证和复现

编译 Zero Hour、Generals 两个 Release 程序及其 GoogleTest 测试程序。当前检查包括 11 个竞技 AI 单元测试，覆盖不可见变化不会覆盖历史记录、情报可信度衰减、进攻与撤退阈值、状态切换、防止将军误启用、非空保存加载一致性和 CRC 一致性；连同原有测试，Zero Hour 共 17 项、Generals 共 6 项。

```powershell
cmake -S . -B build/win32 -DRTS_BUILD_OPTION_TESTS=ON
cmake --build build/win32 --config Release --target z_generals z_googletest g_generals g_googletest
& build/win32/Tests/Google/Release/z_googletest.exe
& build/win32/Tests/Google/Release/g_googletest.exe
```

真实引擎的运行测试使用 `Tests/CompetitiveAI/run_smoke_tests.py`。它通过正常重放加载器建立两方竞技 AI 的测试局，不注入玩家命令、作弊资金或平衡修改。默认覆盖三阵营的 9 种组合 × 3 张原版地图，每局运行 600 秒模拟时间，再重复 3 个相同地图与种子的组合，共 30 局。

```powershell
python Tests/CompetitiveAI/run_smoke_tests.py `
  --exe build/win32/GeneralsMD/Release/generalszh.exe `
  --game-dir 'C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour' `
  --output build/competitive-ai/validation
```

默认地图为 Tournament Desert、Final Crusade、Tournament City，分别覆盖开阔区域、窄口与驻军建筑较多的场景。检查初始资金与单位、采集收入、战斗生产、引擎正常结束以及同种子的指标重复性。以上命令的报告及原始输出位于 `build/competitive-ai/validation/report.json`、`engine.log` 和 `engine.err.log`；报告记录程序 SHA-256，便于确认测试对应的版本。重复指标一致是有限的重复性检查，不等价于完整网络同步或真实存档恢复验证。

最终编译的验证结果：**30 个场景、0 个检查失败，引擎退出码 0，标准错误输出为空**。所有场景的两方都产生采集收入和战斗单位，三组重复运行的逐分钟指标完全一致。报告 SHA-256 已与当前编译程序核对。日志中观察到 USA 的完整载兵 Humvee、GLA 的完整载兵 Battle Bus、三个阵营的科技转型及战斗损失。China 的空军载兵、爆破运输、技能组合和紧急卸载未由这批数据单独证明，仍需专项场景。

单局与交换出生位置的测试文件也可独立产生：

```powershell
python Tests/CompetitiveAI/make_smoke_replay.py build/competitive-ai/example.rep `
  --sides USA GLA --map 'tournament desert' --seconds 600 --seed 1 --swap-starts
```

## 采纳门槛与未完成验证

这 30 局是新策略互战的运行测试，没有替代计划中的 270 局对原策略比较。原策略包含不公平脚本，现阶段还没有建立经验证的公平比较基准；因此不报告比较胜率，不把新模式设为原有难度的替代策略。

后续比较应使用 9 种阵营组合 × 3 类地图 × 5 组同种子 × 交换出生位置，共 270 局。双方同资金、同规则、同公平情报；统计总胜率及各阵营胜率、置信区间、开局崩溃、采集效率、兵力交换、生产空闲和比赛时长。采纳标准仍为总体至少 60%、每阵营至少 55%，并无严重公平性、稳定性或兼容问题。

尚需验证完整局的胜负与残局搜索、受压恢复的专项场景、载兵紧急卸载和隧道危险出口、空袭与晋升技能组合、人类对战、九位将军/战役/挑战的运行回归、真实保存加载后的继续模拟、网络同步和大军耗时。控制器保存加载与 CRC 的单元检查已经通过，但不能代替这些运行场景。

当前使用现代 MSVC 的 Release 编译。依照项目 `TESTING.md`，原零售版重放 CRC 兼容性需要 VC6 对应构建验证；本次合成测试文件没有零售版 CRC 期望值，不能用其通过来声称零售版重放兼容。
