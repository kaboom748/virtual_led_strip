<img width="1846" height="968" alt="image" src="https://github.com/user-attachments/assets/884e29b1-7563-4ba1-a137-3d0b4d6b03f2" />

# virtual_led_strip

An ESPHome `light:` platform with no pin. **Your browser is the strip.**

Point your effects at it instead of `neopixelbus` or `esp32_rmt_led_strip`, open
`http://<ip>:8083/`, and the strip appears in a tab — with a panel that names
whatever goes wrong. ESP8266 **and** ESP32. No LEDs, no data pin, no solder.

```yaml
external_components:
  - source: github://kaboom748/virtual_led_strip
    components: [virtual_led_strip]
    refresh: 1d

light:
  - platform: virtual_led_strip
    id: strip
    name: "Shelf"
    num_leds: 188
    port: 8083
    max_refresh_rate: 32ms
    effects:
      - addressable_rainbow:
      - addressable_scan:
```

## Three keys, and that is the whole point

`num_leds`, `port`, `is_rgbw`. There is no `chipset:`, no `variant:`, no
`rgb_order:` — **those describe a wire we do not have.** `ESPColorView` hands out
pointers into our own buffer, so the memory layout is ours; an effect writes
`Color(255, 0, 0)` and the view puts it where it likes. Byte order on the wire is
a property of the bus.

`is_rgbw` earns its place because it changes `LightTraits`, and therefore what
effects can express. Rehearsing an SK6812 RGBW is not rehearsing a WS2812 RGB.

## Swapping to a real strip

Replace the `light:` block. Effects, automations, lambdas, `id`, `name` and
`num_leds` all carry over:

```yaml
light:
  - platform: neopixelbus          # ESP8266
    variant: WS2812
    pin: GPIO2
    num_leds: 188
```

Wiring advice worth more than any option: **use GPIO1, GPIO2 or GPIO3 on
ESP8266.** `neopixelbus` picks its method from the pin — DMA on GPIO3, UART on
GPIO1/GPIO2, and **bit-bang on anything else**. Bit-bang means 188 × 30 µs =
5.6 ms of loop with interrupts off, every frame. The other three cost nothing.
On ESP32 it is always I2S or RMT — never blocking.

## `max_refresh_rate` is a threshold, not a rate

Same semantics as `esp32_rmt_led_strip`, and the same trap. Writes leave from
`LightState::loop()`, one per pass, **so the loop interval quantizes it**:

| you write | you actually get |
|---|---|
| `16ms` | 62.5 fps |
| **`32ms`** | **31.2 fps** |
| `33ms` | **20.9 fps** — 30 % below what you asked |
| `48ms` | 20.9 fps |

Only multiples of the 16 ms loop land where you meant. `dump_config()` prints the
resolved rate so you are not left guessing.

## A bench pattern: 100 LEDs, ten tests, one image

`example/bench-mire-100-esp8266.yaml` and `-esp32.yaml` carry a test pattern
worth more than its size.

With 10 LEDs you are forced to code in *time*: each LED gets its rhythm and you
wait for the sequence to unfold. With 100 you code in *space* — ten decades, ten
diagnostics running **in parallel**. One image is a complete verdict, and there
is nothing to wait for.

| decade | LEDs | what it proves |
|---|---|---|
| d0-d2 | 00-29 | R, G, B ramps 25..255 — per-channel linearity, missing steps |
| d3 | 30-39 | white ramp — do the three channels track together |
| d4 | 40-49 | levels 1..10 of 255 — the quantization floor |
| d5 | 50-59 | checkerboard at fs/2 — signal integrity, dropped frames |
| d6 | 60-69 | pure R G B — channel order |
| d7 | 70-79 | hue wheel — continuity, banding |
| d8 | 80-89 | pure Y C M — two-channel mixing |
| d9 | 90-99 | full white — reference, end of strip |

You read a fault as **decade + rank**: "third LED of the blue ramp is dark" is
LED 22, blue channel. You get the index without counting the strip.

A white cursor walks one LED per frame, preceded by a black hole that shows the
direction. A dead LED is a fixed gap. **One pass is 100 frames** — time it and
you have your real frame rate without touching the log. 3.2 s is 31.25 fps; 4.8 s
is 20.8 fps, which means you landed on the 16 ms quantum described above.

`gamma_correct: 0` is not optional on a bench: with the 2.8 default, d4 is
entirely black and your ramps stop being ramps. Set it back to 2.8 to *see* the
crushing — decade d4 vanishing is the same finding as the cliff below effect 28.

Decoded off the wire, d0 reads `25, 51, 76, 102, 127, 153, 178, 204, 229, 255`
and d4 reads exactly `1..10`. The encoder picks DELTA and spends **12 B/frame**:
only the cursor moves.

The one fault this pattern cannot see is voltage drop — it needs all 100 LEDs at
full *simultaneously*, which is incompatible with a test pattern. That is what
the second effect, `MUR`, is for: a white ramp to 100 %. Watch the far end.
White turning yellow then orange is the 5 V collapsing; blue (Vf ~3.2 V) dies
before red (Vf ~2.0 V). **The hue drift is the voltmeter.** Note the percentage
where it starts: that is your supply limit, in a number.

`MUR` only means anything on real hardware. This platform has no wire and no
supply, and cannot simulate it.


## Reading the panel

The heartbeat (10/s, always, playing or not) is the metronome. Every beat is
stamped **on the ESP**, before the network can touch it, which lets the panel
separate the only two things that can go wrong:

| Line | Means |
|---|---|
| `Beat rate / target` | Must read `10.0 / 10.0`. Any sag is real. |
| `ESP loop worst` | Largest gap between beats **as the ESP lived it**. |
| `Link jitter` | Arrival spread **minus** the ESP's own period. The network, and nothing else. |
| `Dropped frames` | A frame was superseded before it left. Your effect outruns the link. |
| `Encoding` / `RAW / RLE / DELTA` | The three candidates and which won, per frame. |
| `Late frames` | Play-out shorter than the link jitter. Raise the slider; `Margin min` says by how much. |

## Bandwidth is decided by the effect, not by `num_leds`

Measured on ESPHome's own effects, on the wire:

| effect | 60 LEDs | 300 LEDs | 5000 LEDs |
|---|---|---|---|
| Scan | 12 B | 12 B | **12 B** |
| Twinkle | 10 B | 13 B | **12 B** |
| Fireworks | 6 B | 5 B | **5 B** |
| **Rainbow** | 180 B | 900 B | **15000 B** |

Sparse effects are **O(1)** — delta sends only what moved, and what moves does
not grow with the strip. Only a full-length gradient is O(N). At 188 LEDs a
rainbow is 564 B/frame: 33 kB/s at 60 fps (over an ESP8266's practical ~32 kB/s),
**16.5 kB/s at 32ms** (half the budget).

The encoder picks the shortest **per frame** — and that is not a flourish. On a
rainbow, RLE (1199 B) and delta (955 B) both cost *more* than raw (900 B); on a
twinkle, delta collapses to 13 B. No single encoding wins.

## The colour pipeline, and the bug it prevents

`gamma_correct` defaults to **2.8** on every ESPHome light, and
`ESPColorView::set_red()` applies it **into our buffer**. So what we stream is not
colour — it is **PWM duty**, which is *linear* light. Your screen wants sRGB.

| effect writes | buffer carries | LED emits | `rgb(buffer)` emits |
|---|---|---|---|
| 128 | 37 | 14.5 % | 1.43 % — **10× too dark** |
| 64 | 5 | 2.0 % | 0.02 % — **112×** |
| 32 | 1 | 0.4 % | 0.00 % — **772×** |

A naive render erases the bottom half of your range. This page converts
duty → linear → sRGB with the real piecewise curve (not a 2.2 power, whose linear
segment matters exactly where your fade lives).

Which means the page shows you something you would otherwise only find on the
shelf: **below effect 28, the LED is simply off.** Gamma 2.8 quantizes 0–27 to
buffer 0, and the last visible step is a jump of 21 sRGB levels. **Your fade to
black does not fade — it snaps.**

## What this cannot rehearse

- **A wrong `rgb_order`.** No wire, no order. Your rainbow will be right here and
  blue-green on the shelf.
- **Interrupt glitches** on a bit-banged WS2812. No wire, no malformed bit.
- **Above 60 Hz** the strip fuses — that is your monitor, and it is also your eye.
- **Rows, elbows and serpentine** are assertions about *your soldering*. The ESP
  knows `num_leds` and nothing else. The browser lets you toggle them because
  picking the one where your comet snakes correctly is how you decide what to
  solder.

## Design notes

- **`setup_priority::HARDWARE`, not `AFTER_WIFI`.** `LightState` sits at
  `HARDWARE - 1.0f`, so it sets up *after* the output and immediately calls
  `setup_state()`, `init_internal()` on every effect, then restores saved state
  with a `call.perform()` — all of which reach into our buffer. At `AFTER_WIFI`
  (200) that buffer was still empty 599 priority levels earlier: a pointer into
  nothing, a crash, a boot loop, safe mode. Both real drivers are at `HARDWARE`
  for exactly this reason. The `host` bench cannot see it — it has no flash to
  restore from, so nothing touches the buffer early.
- **The listen socket opens on the first `loop()`, not in `setup()`.** The buffer
  must exist at `HARDWARE`; the socket wants wifi's `setup()` done (`api` is at
  `AFTER_WIFI` for that reason). You cannot be in both places, so the memory goes
  where `LightState` needs it and the socket waits for `loop()`, which follows
  every `setup()`.
- **No undefined behaviour.** `AddressableLight` is a public API and this
  component owns its buffer. Nothing is reached by casting to a class the object
  is not an instance of.
- **One socket path.** ESPHome's `socket` covers ESP8266 through `lwip_tcp`.
  Note that `ListenSocket` and `Socket` are distinct types there:
  `socket_ip()` cannot `listen()`.
- **Drain the request, do not read once.** `socket.h` states the contract, and
  the cost of ignoring it is not a slow read: closing a socket with *received*
  bytes still unread makes TCP send an RST instead of a FIN, and the RST discards
  the **send** buffer. The page arrived truncated mid-`<script>`, nothing ran, and
  the panel showed its hard-coded HTML defaults as if all were well. Invisible on
  loopback — everything is delivered before the `close()`. It takes a real
  browser.
- **`read() == 0` is EOF. `-1/EWOULDBLOCK` is "wait".** They are not the same, and
  `if (len <= 0) return;` conflates them. Chrome opens speculative sockets it never
  uses; treated as "would block", the first one holds the pending slot forever,
  `accept()` stops accepting, `fetch('/events')` never lands, and the strip stays
  black until reboot. `lwip_raw_tcp_impl.cpp` is explicit about which is which.
- **The panel's HTML defaults declare failure**, in red, and the script contradicts
  them on its first line. A panel that shows a plausible state while nothing runs
  is worse than an empty one.
- **A short write is not a failure.** At 564 B every 32 ms an ESP8266's send
  buffer fills in normal service. The frame is kept and retried; nothing is
  thrown away mid-stream.
- **`prev_` only advances on a frame committed to the wire.** Otherwise the
  client would rebuild deltas against a frame it never saw.
- **The heartbeat advances its phase** (`last_beat_ += 100`, never `= now`). A
  metronome that cannot reach its own target measures nothing.
- **The heartbeat runs before the frame.** Put `promote_()` first and the frame
  refills the send buffer every pass; the beat never finds it free, and
  `Beat rate` collapses precisely when you are reading the panel to find out why.
  The data starves the instrument.
- **No `on` / `off` branch in the renderer.** Ambient and emission add; zero is
  not a special case. Two separate formulas do not meet at zero, and a comet's
  tail rendered *darker than an unlit strip* — a darkness invented by the drawing,
  present nowhere in the buffer. It is checked, not intended: monotone across all
  256 levels.
- **`Display` counts requestAnimationFrame; `Render` counts frames painted.**
  Conflating them made the panel blame the ESP for judder the renderer could have
  caused. Two gradients per lit dome is not free.
- **`ESP loop worst` is a 30 s window**, with the lifetime max on its own line. A
  max that never decays welds a boot-time stall to the panel forever, and you
  cannot tell an old freeze from a live one.
- **The browser integrates over the display frame**, in linear. Sampling the
  newest frame drops one at every beat between 16 ms and 16.7 ms — a 2.5 Hz
  stutter *we* would be adding. And averaging sRGB bytes would darken a 50/50
  strobe from 188 to 128.

## License

GPLv3 for the C++, MIT for the Python — ESPHome's own model. GitHub will show
`NOASSERTION`; so does `esphome/esphome`.
