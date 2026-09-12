# 乱杀跑团

一个本地/局域网多人文字跑团对抗游戏，支持真人玩家与 AI 玩家一起游玩。

## 这是什么

- 在浏览器中打开网页即可开始游戏；
- 每位玩家用一句话描述自己的行动；
- 系统/AI 会判断行动结果，推进对抗与死亡；
- 支持添加 AI 补齐人数，一个人也能玩。

## 快速开始

### 编译

Windows 下使用 MinGW：

```bash
gcc -std=c99 -Wall -Wextra -O2 -o luansha.exe \
  src/main.c src/server.c src/game.c src/json.c src/ai.c src/web_assets.c \
  -lws2_32 -lshell32
```

### 运行

```bash
./luansha.exe --no-open 8080
```

浏览器打开：

```text
http://localhost:8080
```

- 默认端口：`8080`
- `--no-open`：不自动打开浏览器

### 桌面客户端

也可以直接使用桌面客户端（基于 pywebview）：

```text
dist/luansha_client_py.exe
```

客户端会自动启动本地 `luansha.exe --no-open 8080` 并打开游戏窗口，无需手动开浏览器。

- 需要把 `luansha_client_py.exe`、`luansha.exe`、`cloudflared.exe` 放在同一目录（`dist/` 已默认放好）；
- 客户端界面提供“退出游戏”按钮，会真正退出客户端；
- 浏览器模式仍然可用，二者互不影响。

## 怎么玩

1. 创建或加入房间；
2. 人数不足时可添加 AI；
3. 所有人准备好后开始；
4. 按顺序用一句话描述自己的行动；
5. 活到最后的人获胜。

### 操作类型

- 陈述：普通动作或说话
- 创造：制作、布置、利用身边物品
- 扭曲：改变局势、干扰他人行动
- 解释/补充：补充说明或化解当前局面
- 限定：使用限定词推进规则

### 状态

每位玩家会显示：

- 健康
- 受伤
- 濒死
- 死亡

玩家状态栏固定在页面左侧，随时可以查看。

## 核心规则

- 每位玩家初始有一定生命；
- 攻击和陷阱可能造成伤害；
- 生命过低会进入濒死状态；
- 濒死状态可能被其他玩家宣告死亡；
- 也可以主动自认死亡；
- 游戏过程中可能触发“灾厄事件”，带来随机影响；
- 灾厄事件会以红色提示显示，部分影响会以紫色提示显示。

## 难度

创建房间时可以选择以下 5 档难度：

- **休闲**：适合新人熟悉规则，节奏相对轻松，灾厄事件来得更慢，AI 也更友好。
- **普通**：适合熟悉规则，默认平衡体验。
- **困难**：挑战更高，灾厄更频繁，AI 更危险，随机判定也更严格。
- **噩梦**：高压力模式，灾厄积累明显加快，AI 会积极寻找机会，失败代价更大。
- **地狱**：最高难度，灾厄频繁发生，AI 救援意愿极低、致死意图极强，随机判定大幅收紧。

难度会影响以下内容，但具体数值不会在游戏界面上显示：

- 灾厄事件的积累速度；
- AI 玩家救援/宣告死亡的概率倾向；
- 随机判定的成功概率与失败效果；
- 系统对叙述合理性/铺垫/操作/幸运等因素的综合审计倾向。

## AI 配置

需要让 AI 玩家说话时，配置一个 AI 接口：

- 在页面点击“测试 AI 接口”；
- 填写接口地址、模型名、API Key；
- 保存后即可让 AI 参与游戏。

也可以通过环境变量配置：

```bash
LUANSHA_AI_URL="https://token.sensenova.cn/v1/chat/completions"
LUANSHA_AI_MODEL="sensenova-6.8-flash-lite"
LUANSHA_AI_KEY="sk-xxx"
```

配置会保存到 `luansha_ai.conf`，重启后仍生效。未配置 Key 时，系统会使用基础规则自动判断。

## 公网联机

使用桌面客户端可以一键开启 Cloudflare 免费隧道：

1. 在客户端大厅点击“🌐 开启公网联机”；
2. 等待生成 `https://xxx.trycloudflare.com` 公网地址；
3. 把地址复制给朋友，对方用浏览器打开即可加入你的房间。

- 需要 `dist/cloudflared.exe` 与客户端放在同一目录；
- 免费地址每次开启会变化，适合熟人临时联机；
- 如果网络到 Cloudflare 不稳定，后续计划支持 cpolar / ngrok / Tailscale 等备选方案。

## 历史战绩 / 回放

- 每局对局结束后，系统会自动把完整时间线保存到 `records/room_<房间ID>.json`；
- 在大厅点击“📜 历史战绩”可以查看历史对局列表；
- 点击“查看回放”可以浏览该局的完整叙事链、玩家状态与胜者。



## 开发者信息

### 常用环境变量

| 变量 | 作用 |
|---|---|
| `LUANSHA_AI_URL` | AI 接口地址 |
| `LUANSHA_AI_MODEL` | 模型名 |
| `LUANSHA_AI_KEY` | API Key |
| `LUANSHA_AI_MIN_INTERVAL` | AI 请求最小间隔（秒） |
| `LUANSHA_AI_CURL_RETRY` | curl 重试次数 |
| `LUANSHA_RATE_LIMIT_MAX` | 每 IP 限流次数 |
| `LUANSHA_RATE_LIMIT_WINDOW` | 限流窗口（秒） |
| `LUANSHA_DEBUG_LOG` | 调试日志路径 |

### 调试日志

默认生成 `debug_operations.log`，用于排查对局过程。每次启动会清空旧日志。

### 主要接口

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/room/create` | 创建房间 |
| POST | `/api/room/add_ai` | 添加 AI |
| POST | `/api/game/start` | 开始游戏 |
| GET | `/api/game/state` | 获取游戏状态 |
| POST | `/api/game/speak` | 发言/行动 |
| POST | `/api/game/declare_death` | 宣告死亡 |
| POST | `/api/ai/config` | 保存 AI 配置 |
| POST | `/api/ai/test` | 测试 AI 接口 |
| GET | `/api/records` | 历史战绩列表 |
| GET | `/api/records?id=<房间ID>` | 查看单局回放 |

## 目录结构

```text
src/
  main.c        启动入口
  server.c      HTTP 服务与 API
  game.c        游戏核心逻辑
  game.h        数据结构
  ai.c          AI 请求与生成
  ai.h          AI 接口声明
  json.c/json.h JSON 解析与序列化
  web_assets.c  前端嵌入资源（由 tools/gen_web_assets.pl 生成）
web/
  index.html    页面结构
  style.css     样式
  app.js        前端逻辑
client/
  py/client.py  pywebview 桌面客户端
  win/          WebView2 旧版客户端（已弃用）
tools/
  gen_web_assets.pl  前端资源生成脚本
  sim_ai_stats.js    模拟统计脚本
dist/
  luansha.exe          游戏服务器/网页服务（发布版）
  luansha_client_py.exe 桌面客户端（发布版）
  cloudflared.exe      公网隧道组件（发布版）
records/
  对局结束自动保存的回放文件（运行时生成）
```

## 提示

- 描述行动时尽量具体，越具体越容易产生有趣的结果；
- 留意自己和他人的状态，濒死时尽快自救或远离危险；
- 灾厄事件无法提前预知，保持谨慎。

## 一局游戏示例

1. 你创建房间“废弃仓库”，添加 3 个 AI，开始游戏；
2. 轮到 AI-1：  
   “我踢开仓库侧门，抄起墙角的铁管，朝你脚边砸去。”
3. 轮到你：  
   “我侧身躲开，顺势把地上的工具箱踢向 AI-1。”
4. 游戏会判断行动结果、伤害和状态变化；
5. 如果有人进入濒死，其他玩家可以在自己回合宣告其死亡；
6. 最后只剩一人时游戏结束。

## 常见问题

### 为什么 AI 不说话？

- 可能没有配置 AI 接口；
- 可以在页面“测试 AI 接口”检查配置；
- 未配置时 AI 会使用基础规则行动，只是内容比较模板化。

### 为什么我一直“受伤/濒死”？

- 战斗会造成伤害；
- 濒死时建议描述自救动作；
- 注意观察左侧状态栏。

### 可以中途加入吗？

- 游戏开始前可以加入；
- 游戏开始后暂不支持中途加入。

### 房间人数必须是多少？

- 推荐 4~7 人；
- 人数不足时可以添加 AI 补位。

## 注意事项

- 请文明游戏，不要使用恶意刷屏；
- 描述行动时不要直接操控其他玩家的角色；
- 不要使用超出当前场景的离谱能力；
- 如果遇到异常，可以查看 `debug_operations.log` 反馈给开发者。

## 后续方向

- 更多场景与道具；
- 更丰富的灾厄事件；
- 道具赛模式；
- 账号体系与跨设备战绩同步（当前回放/战绩为本地保存）；
- 自定义公网穿透（cpolar / ngrok / Tailscale）。
