"""手机操作 agent：读无障碍树 → 让 DeepSeek 决策 → 调 Portal 执行。"""
import json
import re
import time

import portal as portal_mod

try:
    from machine import Pin
    _BOOT = Pin(0, Pin.IN, Pin.PULL_UP)
except Exception:  # 非 ESP32 环境（PC 上做逻辑测试）
    _BOOT = None

SYSTEM_PROMPT = """你是 Android 手机操作代理。你会收到当前屏幕的无障碍树（已压缩）和任务。
每次只输出一个 JSON 对象（不要解释、不要 markdown）：
{"action":"tap","index":12}            点击 index 指向的元素
{"action":"tap_text","text":"关于本机"}  按文字点击（最可靠：填界面上看到的文字）
{"action":"swipe","dir":"up"}          滑动（dir: up/down/left/right）
{"action":"input","text":"你好"}        向当前输入框输入文本（会先清空）
{"action":"key","name":"back"}         按键：back/home/enter/delete
{"action":"open","package":"com.android.settings"}  启动应用
{"action":"wait","seconds":1}          等待界面加载
{"action":"done","message":"结果"}     任务完成，汇报结果

规则：
1. 用 index 定位元素，不要凭空猜坐标；index 每一步都会重新编号，必须按当前列表选。
2. 输入文字前，先 tap 输入框；输入后可用 key enter 提交。
3. 只能依据"当前界面元素"里的内容下结论。done 的 message 必须能在界面中找到依据，例如界面上有 "Android 版本 16" 才能回答 16。
4. 如果当前界面没有答案：继续导航（open 应用 / tap 条目 / swipe），不要提前 done。
5. 同一个动作不要连续重复两次；点完没反应就换一种方式（换条目、滑动、返回）。
6. 界面上显示的应用名/包名可用于判断自己在哪；任务要求的应用没打开时用 open 启动。
7. 只列出了有文字/可交互的元素；目标条目不在列表里时用 swipe 滚动继续找。
8. 设置页经验："关于本机"通常在设置列表底部（先下滑）；进入后 "Android 版本" 可直接看到或点 "版本信息"。
9. 只有当前界面出现 EditText 时才用 input；否则先 tap 输入框。
10. 先在当前应用里找入口，找不到再用 open 切应用；不要从桌面图标乱点。
11. 一次只做一个动作，做完看界面变化再决定下一步。
12. 点击有文字的条目优先用 tap_text（填文字），比 tap index 更可靠；找不到文字时再 swipe 或 tap。
示例（学习这种节奏）：
任务：打开设置，找到 Android 版本。
界面=桌面 → {"action":"open","package":"com.android.settings"}
界面=设置列表没有"关于本机" → {"action":"swipe","dir":"up"}
界面=列表里看到"关于本机" → {"action":"tap_text","text":"关于本机"}
界面=关于本机页有"版本信息" → {"action":"tap_text","text":"版本信息"}
界面=版本信息页含"Android 版本 16" → {"action":"done","message":"Android 版本是 16"}

任务：看电池电量。
界面=桌面 → {"action":"open","package":"com.android.settings"}
界面=设置列表有"电池" → {"action":"tap_text","text":"电池"}
界面=电池页显示 61% → {"action":"done","message":"电量 61%"}
"""


def _parse_bounds(raw):
    try:
        x1, y1, x2, y2 = [int(v.strip()) for v in str(raw).split(",")]
        return x1, y1, x2, y2
    except Exception:
        return None


def flatten(state, max_nodes=80, max_depth=14):
    """把 a11y 树压成紧凑文本；过滤布局/ref_ 噪声，返回 (文本, 节点列表)。"""
    tree = state.get("a11y_tree") or []
    nodes = []
    lines = []

    def walk(children, depth):
        if not children or depth > max_depth:
            return
        for node in children:
            if len(nodes) >= max_nodes:
                return
            if not isinstance(node, dict):
                continue
            idx = len(nodes)
            nodes.append(node)
            cls = str(node.get("className") or "").split(".")[-1]
            text = str(node.get("text") or "").strip()
            desc = str(node.get("contentDescription") or node.get("desc") or "").strip()
            rid = str(node.get("resourceId") or "").split("/")[-1]
            label = text or desc
            if len(label) > 36:
                label = label[:36]
            garbage = (not label) or re.match(r"^ref_[0-9]+$", label) or label == cls
            if (not garbage) or cls in (
                    "EditText", "Button", "ImageButton", "Switch", "CheckBox", "RadioButton"):
                tag = label if label else ("<%s>" % cls)
                extra = (" id=%s" % rid) if rid else ""
                lines.append("#%d %s%s %s" % (idx, tag, extra, node.get("bounds") or ""))
            walk(node.get("children") or [], depth + 1)

    walk(tree, 0)
    if not lines:
        lines.append("(empty screen)")
    return "\n".join(lines), nodes


def _phone_state_text(state):
    ps = state.get("phone_state") or {}
    act = str(ps.get("activityName") or "").split(".")[-1]
    return "当前应用: %s (%s/%s)  键盘: %s" % (
        ps.get("currentApp") or "?",
        ps.get("packageName") or "?",
        act,
        "显示" if ps.get("keyboardVisible") else "隐藏",
    )


def _extract_json(text):
    if not text:
        raise ValueError("empty model reply")
    raw = text.strip()
    if raw.startswith("```"):
        raw = raw.strip("`")
        idx = raw.find("\n")
        if idx >= 0:
            raw = raw[idx + 1:]
        raw = raw.strip("` \n")
    start = raw.find("{")
    end = raw.rfind("}")
    if start < 0 or end <= start:
        raise ValueError("no JSON object in reply: %s" % raw[:120])
    return json.loads(raw[start:end + 1])


def _screen_size(nodes):
    for node in nodes:
        b = _parse_bounds(node.get("bounds"))
        if b and b[2] - b[0] > 200 and b[3] - b[1] > 200:
            return b[2], b[3]
    return 1272, 2800


def _center(node):
    b = _parse_bounds(node.get("bounds"))
    if not b:
        return None
    return (b[0] + b[2]) // 2, (b[1] + b[3]) // 2


def _find_by_text(nodes, needle):
    needle = str(needle or "").strip()
    if not needle:
        return None
    lowered = needle.lower()
    exact = None
    partial = None
    for node in nodes:
        label = str(node.get("text") or "") + " " + str(
            node.get("contentDescription") or node.get("desc") or "")
        label = label.strip()
        if not label:
            continue
        if lowered in label.lower():
            if label == needle:
                exact = node
                break
            if partial is None:
                partial = node
    return exact or partial


def execute(p, nodes, action):
    kind = action.get("action")
    if kind == "tap_text":
        node = _find_by_text(nodes, action.get("text"))
        if node is None:
            return "未找到文字: %s（可能不在当前界面，先 swipe 或返回）" % action.get("text")
        xy = _center(node)
        if not xy:
            return "错误: 该节点没有 bounds"
        p.tap(xy[0], xy[1])
        time.sleep(0.4)
        return "已点击文字 %r @%s" % (str(action.get("text"))[:20], xy)
    if kind == "tap":
        idx = int(action.get("index", -1))
        if idx < 0 or idx >= len(nodes):
            return "错误: index %d 不存在" % idx
        xy = _center(nodes[idx])
        if not xy:
            return "错误: 该节点没有 bounds"
        p.tap(xy[0], xy[1])
        time.sleep(0.4)
        return "已点击 #%d @%s" % (idx, xy)
    if kind == "swipe":
        w, h = _screen_size(nodes)
        d = str(action.get("dir") or "up").lower()
        if d == "up":
            p.swipe(w // 2, int(h * 0.75), w // 2, int(h * 0.25))
        elif d == "down":
            p.swipe(w // 2, int(h * 0.25), w // 2, int(h * 0.75))
        elif d == "left":
            p.swipe(int(w * 0.8), h // 2, int(w * 0.2), h // 2)
        else:
            p.swipe(int(w * 0.2), h // 2, int(w * 0.8), h // 2)
        return "已滑动 %s" % d
    if kind == "input":
        p.input_text(str(action.get("text") or ""))
        return "已输入文本"
    if kind == "key":
        name = str(action.get("name") or "").lower()
        code = {"back": portal_mod.KEY_BACK, "home": portal_mod.KEY_HOME,
                "enter": portal_mod.KEY_ENTER, "delete": portal_mod.KEY_DEL}.get(name)
        if code is None:
            return "错误: 未知按键 %s" % name
        p.key(code)
        return "已按键 %s" % name
    if kind == "open":
        pkg = str(action.get("package") or "").strip()
        if not pkg:
            return "错误: 缺少 package"
        p.start_app(pkg)
        time.sleep(2.5)
        return "已启动 %s" % pkg
    if kind == "wait":
        secs = float(action.get("seconds") or 1)
        time.sleep(min(secs, 5))
        return "已等待 %.1fs" % secs
    if kind == "done":
        return None
    return "错误: 未知 action %s" % kind


def _ui_key(ui_text):
    """轻量屏幕指纹：长度 + 首末字符，用于判断界面是否变化。"""
    if not ui_text:
        return 0
    return len(ui_text) * 131 + ord(ui_text[0]) * 7 + ord(ui_text[-1])


def run(p, llm, task, max_steps=15):
    history = []
    last_ui_key = None
    last_action_str = None
    action_streak = 0
    for step in range(1, max_steps + 1):
        if _BOOT is not None and _BOOT.value() == 0:
            print("[agent] BOOT 按下，中止")
            return "已中止（BOOT）"
        try:
            state = p.state()
            ui_text, nodes = flatten(state)
            ps = _phone_state_text(state)
        except Exception as exc:
            print("[agent] 读取屏幕失败: %s" % exc)
            time.sleep(1)
            continue

        ui_key = _ui_key(ui_text)
        warn = ""
        if last_ui_key is not None and ui_key == last_ui_key:
            warn += "注意：上一步执行后界面没有变化，不要重复同样的动作，换一种方式（换条目/滑动/返回）。\n"
        if action_streak >= 2:
            warn += "注意：你已经连续 %d 次做同一个动作，必须换一种方式。\n" % action_streak

        action = None
        for attempt in range(2):
            ui_use = ui_text if attempt == 0 else ui_text[:4000]
            user = "任务: %s\n\n%s\n\n当前界面元素:\n%s" % (task, ps, ui_use)
            if warn:
                user += "\n\n" + warn
            if history:
                user += "\n最近动作:\n" + "\n".join(history[-6:])
            user += "\n\n只输出一个 JSON 动作，不要解释。"
            try:
                reply = llm.chat([
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": user},
                ])
                action = _extract_json(reply)
                break
            except Exception as exc:
                print("[agent] 模型/解析失败(%d): %s" % (attempt + 1, exc))
                action = None
                time.sleep(1)
        if action is None:
            return "失败: 模型连续出错"

        action_str = json.dumps(action)
        if action_str == last_action_str:
            action_streak += 1
        else:
            action_streak = 1
        last_action_str = action_str
        last_ui_key = ui_key

        print("[agent] %d/%d %s" % (step, max_steps, action_str[:160]))
        try:
            result = execute(p, nodes, action)
        except Exception as exc:
            result = "动作失败: %s" % exc
            print("[agent] 动作异常: %s" % exc)
        if result is None:
            msg = str(action.get("message") or "完成")
            print("[agent] 完成: %s" % msg)
            return msg
        history.append("步骤%d: %s -> %s" % (step, action_str[:100], result))
        time.sleep(0.5)
    return "达到最大步数 (%d)" % max_steps
