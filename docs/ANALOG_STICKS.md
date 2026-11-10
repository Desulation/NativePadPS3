# Analog sticks: why they fall short at the extremes, and what the plugin does

## The symptom

The controller is recognized and the buttons work, but in games the analog
sticks "don't go all the way": there is no full lock on the steering wheel, the
character doesn't run at full speed, the aim doesn't complete its turn, and the
diagonals always seem incomplete. With Hall effect sticks it is worse.

The natural suspicion is a mapping or scaling error. **It isn't.** The
DS4/DualSense bytes are already 0..255 with 0x80 at rest, the same convention
as the DualShock 3, and the plugin copies them untouched. The difference is the
**response curve**.

## The cause

The DualShock 3 analog converter **saturates before the end of the mechanical
travel**, at around 80% of it. Two consequences:

- pushing an axis all the way gives `0x00`/`0xFF` **with travel to spare**;
- a full diagonal puts each axis at 0.707 of the mechanical travel, which gives
  0.707 / 0.80 = **0.88** of full scale, about `0x0F`.

The DS4 and the DualSense are linear up to the stop. The same full diagonal
gives 0.707 of full scale, about `0x26`. That is a long way from what a PS3
game was tuned to receive.

```
        Up-left diagonal, pushed to the stop

        DualShock 3 ....... 0x0F   ← what the game expects
        DS4 / DualSense ... 0x26   ← what it gets today
```

Hall effect adds a second problem: these modules are linear **and** often have
a reduced range, and many only go from `0x14` to `0xEB`. Then not even a single
axis reaches the extreme, and the game never sees the maximum value in any
direction.

## The fix

A single parameter reproduces the whole curve. Each axis is normalized against
the travel that stick **actually** reaches, divided by the saturation fraction,
and clamped at the limit:

```
f = clamp(f_measured / 0.80, -1, +1)
```

- **On the axis:** 1.00 / 0.80 saturates. Full scale is reached early, exactly
  as on a DS3, and a short-travel Hall effect stick gets there too.
- **On the diagonal:** 0.707 / 0.80 = 0.88, the DS3 number.
- **Rotating the stick:** the trace is a circle cut off by the clamp, becoming
  a square with rounded sides. This is the shape a real DS3 produces.

`STICK_SATURATION` is that number, as a percentage.

## Verified numbers

A simulation of the pipeline exactly as the code runs it (integer arithmetic
included), with a 4% dead zone:

| Situation | Raw | `100` (raw) | `80` (default) | `71` |
|---|---|---|---|---|
| **Factory DS4** (0–255) | | | | |
| rest | 128,128 | 128,128 | 128,128 | 128,128 |
| full left | 0,128 | **0**,128 | **0**,128 | **0**,128 |
| full diagonal | 37,37 | 37,37 | **14,14** | 0,0 |
| half travel | 64,128 | 66,128 | 51,128 | 41,128 |
| **Hall effect** (20–235) | | | | |
| full left | 21,128 | 1,128 | **0**,128 | **0**,128 |
| full diagonal | 52,52 | 37,37 | **15,15** | 1,1 |
| **Worn stick** (45–210) | | | | |
| full left | 46,128 | 1,128 | **0**,128 | **0**,128 |
| full diagonal | 70,70 | 38,38 | **16,16** | 1,1 |

DualShock 3 reference: full axis `0x00`, full diagonal ≈ `0x0F` (15).

Note two things:

1. With `80`, the diagonal lands at **14–16** in all three cases, the DS3 value.
2. Once calibrated, the Hall effect stick produces **the same numbers** as a
   factory stick. The hardware difference disappears.

And note what happens **without** correction: a Hall effect stick pushed all the
way delivers `1`, never `0`. Games that test for the exact extreme simply never
trigger.

## Automatic calibration

`STICK_CALIBRATION = AUTO` learns the real travel of each axis while you play,
and it is what makes the "Hall effect" row of the table work.

How it works, and why it is safe:

- Each axis keeps a **histogram** of the values seen in a window of 512
  samples. At the end of the window, `min`/`max` move part of the way toward the
  most extreme bucket that was hit at least 8 times. A value seen once moves
  nothing, however extreme it is.
- **Widening is fast, narrowing is 8x slower.** Widening is how the real travel
  is discovered; narrowing slowly keeps a few windows of partial movement from
  noticeably retuning the response, but guarantees that a wrong extreme decays
  on its own.
- With the controller at rest, all the samples fall in the central bucket, both
  estimates stay within `CAL_MIN_SPAN` and **nothing moves**, so leaving the
  controller on the table does not collapse the range.
- The center is only accepted after the value stays still for about a second,
  **and** only if it is within 25 units of 0x80. Without that second lock,
  holding the stick to one side during a match would drag the center along with
  it.
- It starts by assuming a conservative range (`0x14`–`0xEB`), typical of Hall
  effect. Erring on the narrow side is harmless, because the clamp absorbs it.
  Erring on the wide side is what would prevent reaching the maximum.

The result goes to `/dev_hdd0/plugins/nativepadps3/nativepadps3_cal.txt`, so the first menu after a
reboot is already right. Rotating both analog sticks once at power-on speeds
everything up.

### Why it is no longer a ratchet

The first version widened one step per sample and **never narrowed**. That is a
one-way ratchet: any spurious extreme became permanent, and recalibrating ran
the same one-way code, so there was no way to undo it.

An old build had exactly such a source of spurious extremes: an unfilled report was
read as raw 0, which dragged the `min` of every axis to the rail while the
`max` values stayed honest.

What made this take so long to find is **where the damage shows up**:

- **On pure axes, it is invisible.** The saturation clamp delivers a solid
  0x00/0xFF either way, with the right range or an inflated one.
- **On diagonals, it shows.** Nothing clamps there (0.707/0.80 = 0.88, mid
  scale), so the inflated range translates directly into less reach.
- And since each diagonal uses a different pair of half-spans, a leak on the
  `min` side alone costs **16% of reach at up-left, 8% on both sides, and zero
  at down-right**.

In other words: one specific corner of one stick that doesn't close, with
everything else perfect, and immune to recalibration. That was exactly the
report that led to replacing the ratchet with the histogram.

## Fine-tuning

| Symptom | What to adjust |
|---|---|
| Still short on reach in the corners | `STICK_SATURATION = 75` or `71` |
| Character "sticks" on the diagonals | `STICK_SATURATION = 85` |
| I want the raw value, with no correction | `STICK_SATURATION = 100` |
| Stick drifts on its own | Raise `STICK_DEADZONE` |
| Hall effect, I want maximum precision at the center | `STICK_DEADZONE = 0` |
| Calibration came out odd | Delete `/dev_hdd0/plugins/nativepadps3/nativepadps3_cal.txt` and rotate the sticks |

`71` deserves a note: it is the value at which 0.707 / 0.71 ≈ 1, meaning the
diagonal reaches the exact corner (`0x00`/`0xFF`). A real DS3 does **not** do
that; it goes to `0x0F`. Use `71` if a specific game only reacts at the absolute
extreme, but `80` is what reproduces the original controller.

## Bluetooth

The correction also applies in Bluetooth mode: the firmware's generic driver
delivers axis values, and the curve is applied to them the same way. The
calibration learns whatever range arrives.
