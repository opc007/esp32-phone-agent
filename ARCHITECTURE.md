# Architecture & verified notes

Goal: a single ESP32-S3 board that controls an Android phone **without a PC**, using
an LLM (DeepSeek) for decisions and the phone's accessibility tree as its "eyes".

The phone runs the open-source **Mobilerun Portal** app (accessibility service + local
HTTP API). The ESP32 joins the phone's Wi-Fi hotspot, reads the UI as text, asks the
model for one action at a time, and injects taps/swipes/typing through Portal's HTTP
API.

```
┌────────────────────────────┐        Wi-Fi (2.4 GHz)        ┌──────────────────────────────┐
│ ESP32-S3 (MicroPython)     │   phone hotspot, ESP32 = STA  │ Android phone                │
│  · Wi-Fi client            │ ◄───────────────────────────► │  · Mobilerun Portal          │
│  · Portal HTTP client      │      HTTP :8080 + token       │    - accessibility tree      │
│  · DeepSeek HTTPS client   │                               │    - tap / swipe / input     │
│  · JSON + agent loop       │                               │    - HTTP server 0.0.0.0:8080│
│  · BOOT button = abort     │            HTTPS              └──────────────────────────────┘
└───────────┬────────────────┘   ┌───────────────────────┐
            └───────────────────► │ api.deepseek.com     │
                                  │ deepseek-v4-pro      │
                                  └───────────────────────┘
```

## Verified behaviour (measured with adb on a test device)

1. **Portal binds to all interfaces.** `/proc/net/tcp6` shows a LISTEN socket on
   `:::8080` owned by the Portal uid, so any device on the same network can reach it —
   no `adb forward` required.
2. **Auth.** Every request needs `Authorization: Bearer <36-char token>`. The token is
   exposed by Portal's content provider and is **stable for one installation**
   (unchanged across service restarts, app restarts and accessibility rebinds); it
   only changes when Portal is reinstalled.
3. **Server auto-start.** After the accessibility service is (re-)bound — which is what
   happens after a phone reboot — Portal starts its HTTP server automatically, no adb
   needed. (Do not `force-stop` Portal: on Android that also disables its
   accessibility service.)
4. **HTTP contract** (as used by mobilerun's own Android TCP client):
   | Endpoint | Notes |
   |---|---|
   | `GET /version` | Portal version |
   | `GET /state` | `{a11y_tree, phone_state}`; typical screens 4–12 KB, complex ones ~90 KB |
   | `GET /state_full` | adds `device_context`, larger |
   | `POST /tap {x,y}` | tap |
   | `POST /swipe {startX,startY,endX,endY,duration}` | swipe |
   | `POST /keyboard/input {base64_text,clear}` | type text (Portal IME must be active) |
   | `POST /keyboard/key {key_code}` | Android key events (3=HOME, 4=BACK, 66=ENTER) |
   | `POST /app {package[,activity]}` / `POST /app/stop {package}` | app start/stop |
   | `GET /screenshot` | ~5.7 MB JSON (4.2 MB PNG) → impractical for MCUs, not used |
5. **Token extraction (one-time, needs a PC):**
   `adb shell content query --uri content://com.mobilerun.portal/auth_token`
6. The phone needs a Wi-Fi network shared with the ESP32. The simplest setup is the
   **phone's own hotspot** (2.4 GHz; ESP32-S3 has no 5 GHz radio). The ESP32 gets its
   address from the hotspot and finds Portal via the DHCP **gateway** (= the phone).

## Why MicroPython

HTTP/HTTPS/JSON have usable libraries, iteration is fast, and there is no toolchain
install. Memory: with the text-only UI mode, payloads are small (a few KB after
pruning); an 8 MB-PSRAM board has plenty of headroom. Boards without PSRAM can still
handle the pruned text tree, with a lower node budget.

## Agent loop

1. `GET /state` → flatten the accessibility tree into compact lines, keeping only
   labelled / interactive nodes (`#idx label id=... bounds`), max ~80 nodes.
2. Send `task + current screen (+ short history)` to DeepSeek chat completions.
3. Parse exactly one JSON action: `tap`, `tap_text`, `swipe`, `input`, `key`, `open`,
   `wait`, `done`.
4. Execute through Portal, repeat until `done` or `MAX_STEPS`.

Design notes:
- `tap_text` (tap by visible label) is far more reliable than numeric indices, which
  are re-numbered every step.
- The prompt receives anti-loop hints ("screen unchanged — try something else"),
  a few-shot example, and forbidden "done without evidence on screen".
- `deepseek-v4-pro` benefits from a large `max_tokens` (8000): reasoning tokens count
  against the budget, and a too-small budget yields empty replies.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| Phone reboot | Accessibility setting persists; server auto-starts on service bind; a real reboot is still worth testing on your device |
| Portal force-stopped | Its accessibility service gets disabled too; re-enable it on the phone |
| Token changes | Only after reinstalling Portal; re-deploy `config.py` |
| Screenshots are huge | Text-only agent; no vision |
| Low-memory board | Prune the a11y tree harder (fewer nodes / lower depth) |
| Hotspot turns off | ESP32 logs a Wi-Fi failure and exits; keep the hotspot on and disable its "auto turn off when idle" option |
| "Done" without evidence | System prompt requires on-screen evidence before answering |

## Release status (tested build)

- Board: ESP32-S3 (N16R8 class) — 16 MB flash, 8 MB octal PSRAM.
- Runtime: MicroPython v1.29.0 (SPIRAM_OCT build), flashed at address `0x0`.
- Chain: Wi-Fi (phone hotspot 2.4 GHz) → Portal HTTP :8080 (Bearer token) →
  DeepSeek v4-pro → actions.
- Measured runs: "read Android version" 12 steps ✅, "read battery level" 6 steps ✅.
- Memory after boot: ~8.3 MB free on a PSRAM board; pruned prompts are ~3–8 KB.

## Roadmap ideas

1. Multiple tasks: BOOT button cycles `config.TASKS`, or add an on-device task input
   (web page over Wi-Fi / BLE).
2. Typing-heavy tasks: keep the Portal keyboard as the active IME, or build a small
   ADB-free IME.
3. Optional vision: region screenshots or icon templates instead of full-screen PNGs.
4. Operations: OTA firmware updates, retry/back-off, result feedback (OLED, LED or a
   notification on the phone).
