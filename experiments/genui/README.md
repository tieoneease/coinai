# Generative UI, experiment 1: cards with slots

Can a fast model answer a spoken request with a card the coin draws well? This
experiment tests one design (PLAN.md D21). The model, or *composer*, fills one card from a fixed
catalog, and the coin lays the card out natively and reports anything that doesn't fit.

| File | What it is |
|---|---|
| `catalog.md` | The composer's system prompt: the card kinds, their slot limits, the routing rules, three examples |
| `questions.txt` | 47 requests in five classes: `local` (answerable from general knowledge), `timer`, `act` (needs SamBot's tools), `live` (needs live or personal data), `vague` |
| `run.py` | `compose` asks the models, `render` shows every answer on the simulator, `report` writes `report.html` and `summary.json` |
| `coinscene.py` | Shows one scene on the simulator or the coin, optionally with a screenshot |
| `samples.json` | Hand-written scenes, one per kind, plus one broken on purpose |

Firmware side: `firmware/main/scene.c` parses and lays out a scene; `ui.c` draws it (mode `M_SCENE`).

## A scene

```json
{"say": "Five minutes. I'll tell you when it's ready.", "mood": "content",
 "card": {"kind": "timer", "label": "Tea", "seconds": 300}}
```

The coin doesn't speak `say` yet: it has a speaker but no text-to-speech. The coin
checks `say` and keeps it, nothing more.

- **Card kinds:** answer, stat, list (tick items), steps (tap for the next page), timer (runs on the coin, tap to pause), confirm (two pills), and later.
- **The later card** hands the request to SamBot and shows "SamBot is on it" under the task.
- **Mood:** one of the eyes' expressions.

Every line is fitted to the circle's chord at its height. Each slot tries its fonts largest first. When nothing fits, the slot is cut with "..." and the cut is reported as an issue. These scenes are also reported:

- unknown fields;
- unknown moods;
- characters the fonts don't have;
- a `say` over 160 characters.

A card with issues is still shown; the issues are for the composer's report.

## Console

The simulator and the coin take the same commands:

```
ui scene begin
ui scene + <hex of the scene JSON, in chunks>
ui scene end      -> SCENE shown kind=list lines=6 issues=1: mood: "helpful" is not a mood
ui scene last     -> the last verdict again
```

Taps print `SCENE-ACTION ...` lines, for example `list 2 checked`, `steps page 1`, `confirm yes` or `timer pause`.

```
python coinscene.py sim samples.json shot.png        # needs SIMPORT (the simulator's port)
python coinscene.py COM9 scene.json shot.png         # the coin, over its USB console
GENUI_URL=<chat completions URL> python run.py compose <run dir> qwen/qwen3.8-27b,openai/gpt-oss-120b
python run.py render <run dir> && python run.py report <run dir>
```

`compose` runs on the SamBot VM over ssh, because the model endpoint is only reachable from there. Settings:

- temperature 0.3, JSON mode, `reasoning_effort` low for gpt-oss;
- the context line is "Local time: Friday 16 October 2026, 10:45 PM."

## Results

The scores:

- **No issues:** the scene showed with nothing cut or flagged.
- **Routing:** the card kind matched the request's class. That is a `later` or `confirm` card for act and live requests, a timer for timer requests, and anything but `later` for local ones. Vague requests are judged by eye.
- **Claims:** the scene says it did something only SamBot could do, outside a later card. Examples: "I'll send", "coming up".
- **Latency:** the model call from the VM, not the round trip from the coin.

**Run 1: first catalog, three models**

| Model | No issues | Routing | Claims | Latency p50 / p90 |
|---|---|---|---|---|
| qwen/qwen3.8-27b | 45/47 | 42/44 | 0 | 0.34 / 0.42 s |
| openai/gpt-oss-120b | 39/47 | 43/44 | 0 | 0.54 / 0.78 s |
| openai/gpt-oss-20b | 43/47 | 37/44 | 2 | 0.50 / 0.75 s |

- **gpt-oss-20b is unsafe for this:**
  - it answered act requests as if it had acted ("Sure thing, jazz coming up", "Sure, I can draft that for you");
  - it invented a live result ("They won 112-107");
  - 30 of its 47 cards were plain answers.
- **qwen's two misroutes:**
  - a reminder "at eight tonight" became a timer: the coin's timer counts a duration, and 8 PM had already passed in the context;
  - "what did I tell you about my trip" got "I have no memory" instead of a hand-off to SamBot, which does remember.
- **gpt-oss-120b's layout issues:** it wrote list items and later text too long for their lines, and one `say` of 163 characters.
- **All three models:**
  - invented moods ("helpful", "concerned");
  - used characters the coin can't draw (U+202F, curly quotes, kana).

**Run 2: revised catalog, two models**

Changes to the catalog:

- clock-time reminders and memory questions go to `later`;
- the later text is shorter, and the coin adds the caption;
- "exactly one of these words" for the mood;
- other languages in Latin letters;
- emergencies put calling for help on the card as well;
- three short examples.

| Model | No issues | Routing | Claims | Latency p50 / p90 |
|---|---|---|---|---|
| qwen/qwen3.8-27b | 46/47 | 44/44 | 0 | 0.37 / 0.45 s |
| openai/gpt-oss-120b | 45/47 | 44/44 | 0 | 0.59 / 1.01 s |

What still fails:

- **qwen:** one invented mood ("helpful").
- **gpt-oss-120b:**
  - list items too long for one line;
  - kana in `say` ("Arigatou (ありがとう)").

## What it says

- **qwen3.8-27b is the composer to build on.** It is the fastest of the three, its cards fit, and it routes correctly once the catalog says where clock-time reminders and memory go.
- **The prompt can't guarantee a valid scene, so whatever sends scenes to the coin must check them.** It should:
  - clamp an unknown mood to `neutral`;
  - turn non-ASCII into ASCII;
  - ask the model once more with the firmware's issue list when a slot was cut.

  The firmware already produces that list; on the coin, the same check runs before the card is drawn.
- **Hand-offs read well as cards.** Of the 47 requests, 17 became `later` cards, such as "Text Mia: I'm running ten minutes late" over "SamBot is on it". The value depends on SamBot acting on them, which this experiment doesn't touch.

## On the coin

The same scenes drew the same on the coin as on the simulator, checked with `fbdump` captures:

- `evidence/genui-coin.png`: the coin;
- `evidence/genui-sim.png`: the simulator, run 2.

Over the minute the five scenes were on screen, the slowest frame took 10.4 ms to draw, apart from the boot screen (`ui perf`). Taps on lists, steps, confirm and the timer work in the simulator. On the coin they print the same `SCENE-ACTION` lines, but only the simulator's taps have been driven.

## Not tested yet

- How the cards look and feel on the AMOLED in hand.
- The full trip: coin → phone or Wi-Fi → SamBot → composer → coin. There is no message type yet for sending a scene to the coin over BLE or from SamBot.
- Speaking `say` out loud.
- Requests from real use. The 47 test requests are made up and typed, not transcribed speech.
