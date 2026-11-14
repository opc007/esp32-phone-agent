#!/usr/bin/env python3
"""
ax_ui.py —— 直接读取/操作 macOS 应用界面控件（不经过 osascript / System Events）

为什么需要它：
  computer_portal.py 早期只能靠坐标盲点，而且 /state 返回的是一份写死的假控件树。
  这台 Mac 上 osascript 的 AppleEvent 被 TCC 拒绝（-10004 权限违例），
  但本进程已经被授予「辅助功能」（AXIsProcessTrusted() == True），
  所以可以走 ApplicationServices 的 AXUIElement C API：读真实控件树、
  读输入框里的文字、给输入框设值、按按钮——这些都不需要 AppleScript。

用法：
    import ax_ui
    tree = ax_ui.frontmost_tree()          # 真实控件树（含文字 / 位置 / 焦点）
    hits = tree.find(role="AXTextField")   # 按条件找控件
    hits[0].set_value("张三")              # 直接写进输入框
    hits[0].press()                       # 触发按钮
"""
import ctypes
import ctypes.util
from ctypes import (c_void_p, c_int, c_uint32, c_char_p, byref, POINTER,
                    c_long, c_ulong, c_bool, c_double, c_float, Structure)

from AppKit import NSWorkspace

kCFStringEncodingUTF8 = 0x08000100

AS = ctypes.cdll.LoadLibrary(
    ctypes.util.find_library("ApplicationServices") or
    "/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices")
CF = ctypes.cdll.LoadLibrary(
    ctypes.util.find_library("CoreFoundation") or
    "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")

# ---- CoreFoundation ----
CF.CFStringCreateWithCString.argtypes = [c_void_p, c_char_p, c_uint32]
CF.CFStringCreateWithCString.restype = c_void_p
CF.CFStringGetCString.argtypes = [c_void_p, c_void_p, c_long, c_uint32]
CF.CFStringGetCString.restype = c_int
CF.CFArrayGetCount.argtypes = [c_void_p]
CF.CFArrayGetCount.restype = c_long
CF.CFArrayGetValueAtIndex.argtypes = [c_void_p, c_long]
CF.CFArrayGetValueAtIndex.restype = c_void_p
CF.CFGetTypeID.argtypes = [c_void_p]
CF.CFGetTypeID.restype = c_ulong
CF.CFStringGetTypeID.restype = c_ulong
CF.CFArrayGetTypeID.restype = c_ulong
CF.CFBooleanGetTypeID.restype = c_ulong
CF.CFBooleanGetValue.argtypes = [c_void_p]
CF.CFBooleanGetValue.restype = c_bool
CF.CFNumberGetTypeID.restype = c_ulong
CF.CFNumberGetValue.argtypes = [c_void_p, c_int, c_void_p]
CF.CFNumberGetValue.restype = c_bool
CF.CFRetain.argtypes = [c_void_p]
CF.CFRetain.restype = c_void_p
CF.CFRelease.argtypes = [c_void_p]
CF.CFRelease.restype = None

# ---- Accessibility ----
AS.AXUIElementGetTypeID.restype = c_ulong
AS.AXValueGetTypeID.restype = c_ulong
AS.AXUIElementCreateApplication.argtypes = [c_int]
AS.AXUIElementCreateApplication.restype = c_void_p
AS.AXUIElementCopyAttributeValue.argtypes = [c_void_p, c_void_p, POINTER(c_void_p)]
AS.AXUIElementCopyAttributeValue.restype = c_int
AS.AXUIElementSetAttributeValue.argtypes = [c_void_p, c_void_p, c_void_p]
AS.AXUIElementSetAttributeValue.restype = c_int
AS.AXUIElementPerformAction.argtypes = [c_void_p, c_void_p]
AS.AXUIElementPerformAction.restype = c_int
AS.AXUIElementCopyAttributeNames.argtypes = [c_void_p, POINTER(c_void_p)]
AS.AXUIElementCopyAttributeNames.restype = c_int
AS.AXUIElementCopyActionNames.argtypes = [c_void_p, POINTER(c_void_p)]
AS.AXUIElementCopyActionNames.restype = c_int
AS.AXUIElementSetAttributeValue
AS.AXIsProcessTrusted.restype = c_bool
AS.AXValueGetType.argtypes = [c_void_p]
AS.AXValueGetType.restype = c_uint32
AS.AXValueGetValue.argtypes = [c_void_p, c_uint32, c_void_p]
AS.AXValueGetValue.restype = c_bool


class CGPoint(Structure):
    _fields_ = [("x", c_double), ("y", c_double)]


class CGSize(Structure):
    _fields_ = [("width", c_double), ("height", c_double)]


def cfstr(s):
    return CF.CFStringCreateWithCString(None, s.encode("utf-8"), kCFStringEncodingUTF8)


def cfstr_value(s):
    """创建可作为 AX 属性值的 CFString（供 AXUIElementSetAttributeValue 使用）。"""
    return cfstr(s)


def _cf_to_py(v, depth=0):
    """把 CFTypeRef 转成 Python 值；AXUIElement 保留为 AXElement 对象。"""
    if not v or depth > 4:
        return None
    t = CF.CFGetTypeID(v)
    if t == CF.CFStringGetTypeID():
        buf = ctypes.create_string_buffer(8192)
        if CF.CFStringGetCString(v, buf, 8192, kCFStringEncodingUTF8):
            return buf.value.decode("utf-8", "replace")
        return None
    if t == CF.CFArrayGetTypeID():
        return [_cf_to_py(CF.CFArrayGetValueAtIndex(v, i), depth + 1)
                for i in range(min(CF.CFArrayGetCount(v), 300))]
    if t == CF.CFBooleanGetTypeID():
        return bool(CF.CFBooleanGetValue(v))
    if t == CF.CFNumberGetTypeID():
        d = c_double()
        if CF.CFNumberGetValue(v, 13, byref(d)):  # kCFNumberDoubleType
            return d.value
        return None
    if t == AS.AXUIElementGetTypeID():
        return AXElement(v)
    if t == AS.AXValueGetTypeID():
        kind = AS.AXValueGetType(v)
        if kind == 1:  # CGPoint
            p = CGPoint()
            return [round(p.x, 1), round(p.y, 1)] if AS.AXValueGetValue(v, kind, byref(p)) else None
        if kind == 2:  # CGSize
            s = CGSize()
            return [round(s.width, 1), round(s.height, 1)] if AS.AXValueGetValue(v, kind, byref(s)) else None
    return None


class AXElement:
    """一个真实的界面控件；持有 AXUIElementRef。"""

    def __init__(self, ref):
        CF.CFRetain(ref)
        self.ref = ref

    def __del__(self):
        try:
            CF.CFRelease(self.ref)
        except Exception:
            pass

    def attr(self, name):
        v = c_void_p()
        rc = AS.AXUIElementCopyAttributeValue(self.ref, cfstr(name), byref(v))
        if rc != 0 or not v.value:
            return None
        try:
            return _cf_to_py(v.value)
        finally:
            CF.CFRelease(v.value)

    def set(self, name, text):
        val = cfstr_value(str(text))
        rc = AS.AXUIElementSetAttributeValue(self.ref, cfstr(name), val)
        CF.CFRelease(val)
        return rc == 0

    def press(self, action="AXPress"):
        a = cfstr(action)
        rc = AS.AXUIElementPerformAction(self.ref, a)
        CF.CFRelease(a)
        return rc == 0

    def actions(self):
        v = c_void_p()
        rc = AS.AXUIElementCopyActionNames(self.ref, byref(v))
        if rc != 0 or not v.value:
            return []
        try:
            return _cf_to_py(v.value) or []
        finally:
            CF.CFRelease(v.value)

    def snapshot(self, depth=4, max_nodes=250, counter=None):
        if counter is None:
            counter = [0]
        if counter[0] >= max_nodes or depth < 0:
            return None
        counter[0] += 1
        node = {
            "role": self.attr("AXRole") or "",
            "title": self.attr("AXTitle") or "",
            "value": self.attr("AXValue"),
            "desc": self.attr("AXDescription") or "",
            "placeholder": self.attr("AXPlaceholderValue") or "",
            "identifier": self.attr("AXIdentifier") or "",
            "focused": self.attr("AXFocused"),
            "enabled": self.attr("AXEnabled"),
            "pos": self.attr("AXPosition"),
            "size": self.attr("AXSize"),
        }
        if isinstance(node["value"], (dict, list)):
            node["value"] = str(node["value"])[:200]
        elif isinstance(node["value"], str) and len(node["value"]) > 300:
            node["value"] = node["value"][:300]
        node["_el"] = self
        kids = self.attr("AXChildren")
        node["children"] = []
        if isinstance(kids, list):
            for k in kids:
                if isinstance(k, AXElement):
                    sub = k.snapshot(depth - 1, max_nodes, counter)
                    if sub:
                        node["children"].append(sub)
        return node


def trusted():
    """本进程是否拥有辅助功能权限（决定能否注入按键 / 读写控件）。"""
    try:
        return bool(AS.AXIsProcessTrusted())
    except Exception:
        return False


def app_element(pid):
    ref = AS.AXUIElementCreateApplication(int(pid))
    return AXElement(ref) if ref else None


def running_apps(match=None):
    out = []
    for a in NSWorkspace.sharedWorkspace().runningApplications():
        name = str(a.localizedName() or "")
        bid = str(a.bundleIdentifier() or "")
        if match and match.lower() not in (name + " " + bid).lower():
            continue
        out.append({"name": name, "bundle_id": bid, "pid": int(a.processIdentifier()),
                    "active": bool(a.isActive())})
    return out


def frontmost():
    a = NSWorkspace.sharedWorkspace().frontmostApplication()
    if not a:
        return None
    return {"name": str(a.localizedName() or ""), "bundle_id": str(a.bundleIdentifier() or ""),
            "pid": int(a.processIdentifier())}


def frontmost_tree(depth=5, max_nodes=250):
    f = frontmost()
    if not f:
        return None, None
    el = app_element(f["pid"])
    return f, (el.snapshot(depth, max_nodes) if el else None)


class Tree:
    """控件树 + 检索。"""

    def __init__(self, root, app=None):
        self.root = root
        self.app = app or {}

    def nodes(self):
        out = []

        def walk(n, d=0):
            if not n:
                return
            n["_depth"] = d
            out.append(n)
            for c in n.get("children", []):
                walk(c, d + 1)
        walk(self.root)
        return out

    def find(self, role=None, text=None, placeholder=None, focused=None, limit=20):
        """按角色 / 文字 / 占位符 / 焦点筛选，返回匹配节点（含 _el）。"""
        hits = []
        for n in self.nodes():
            if role and n.get("role") != role:
                continue
            if focused is not None and bool(n.get("focused")) != focused:
                continue
            blob = " ".join(str(n.get(k) or "") for k in
                            ("title", "value", "desc", "placeholder", "identifier"))
            if text and text not in blob:
                continue
            if placeholder and placeholder not in str(n.get("placeholder") or ""):
                continue
            hits.append(n)
            if len(hits) >= limit:
                break
        return hits

    def brief(self, limit=120):
        """给设备端 / 大模型看的紧凑文本：角色 + 文字 + 屏幕位置。"""
        lines = []
        for n in self.nodes():
            bits = [n.get("role") or "?"]
            for k in ("title", "value", "desc", "placeholder"):
                v = n.get(k)
                if v and isinstance(v, str) and v.strip():
                    bits.append(v.strip().replace("\n", " ")[:60])
            p, s = n.get("pos"), n.get("size")
            if isinstance(p, list) and isinstance(s, list):
                bits.append(f"@({int(p[0])},{int(p[1])},{int(s[0])}x{int(s[1])})")
            if n.get("focused"):
                bits.append("[焦点]")
            line = " ".join(bits)
            if len(set(line)) > 8 and line not in lines:
                lines.append(line)
            if len(lines) >= limit:
                break
        return lines

    def to_json(self, with_el=False):
        def clean(n):
            if not n:
                return None
            d = {k: v for k, v in n.items() if k not in ("_el", "children")}
            d["children"] = [clean(c) for c in n.get("children", [])]
            return d
        return clean(self.root)


def screenshot(path):
    """截屏保存为 PNG；需要「屏幕录制」权限。返回是否成功。"""
    try:
        import Quartz
        from AppKit import NSBitmapImageRep, NSPNGFileType
        img = Quartz.CGWindowListCreateImage(
            Quartz.CGRectInfinite, Quartz.kCGWindowListOptionOnScreenOnly,
            Quartz.kCGNullWindowID, Quartz.kCGWindowImageDefault)
        if not img:
            return False
        rep = NSBitmapImageRep.alloc().initWithCGImage_(img)
        data = rep.representationUsingType_properties_(NSPNGFileType, None)
        return bool(data and data.writeToFile_atomically_(path, True))
    except Exception as e:
        print("screenshot failed:", e, flush=True)
        return False


if __name__ == "__main__":
    print("AXIsProcessTrusted:", trusted())
    f, root = frontmost_tree()
    print("前台:", f)
    t = Tree(root, f)
    for line in t.brief(60):
        print("  ", line)
