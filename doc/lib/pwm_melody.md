# PWM Melodies

A landed rocket needs a way to communicate its position to the searching squad.
One simple way of making it easier to find is to play loud buzzer sounds in an
endless loop until the recovery team finds it.

But only playing loud noises is boring and annoying in testing.
Instead, use the PWM Melody API to play music of your liking.

Melodies are played through Zephyr's generic buzzer API
(`<zephyr/drivers/buzzer.h>`), so the buzzer is described with the upstream
`pwm-buzzer` binding and enabled with `CONFIG_BUZZER`
(`CONFIG_BUZZER_PWM` follows from the devicetree). The period in `pwms` is the
tone `buzzer_beep()` plays; every note sets its own frequency.

```devicetree
/ {
   buzzer0: buzzer_0 {
      compatible = "pwm-buzzer";
      pwms = <&ledc0 0 PWM_HZ(4000) PWM_POLARITY_NORMAL>;
   };
};
```

```c
#include <aurora/lib/pwm_melody.h>

// play astronomia from aurora/lib/pwm_melody.h
PWM_MELODY_CTX_DEFINE(melody_ctx, DEVICE_DT_GET(DT_NODELABEL(buzzer0)),
                      astronomia, 1024);

int main()
{
   // optional: the pwm-buzzer driver boots at 50 % volume
   buzzer_set_volume(DEVICE_DT_GET(DT_NODELABEL(buzzer0)), BUZZER_VOLUME_MAX);

   pwm_melody_start(&melody_ctx);

   // --snip--
   // play as long as needed
   // --snip

   pwm_melody_stop(&melody_ctx);
}
```

## Several Tunes, One Buzzer

A board has one buzzer, so two playback threads would fight over the
same buzzer. Melodies therefore share a single context and are
switched with {c:func}`pwm_melody_play`, which stops the current tune,
waits for its thread to be gone, and starts the new one:

```c
// while the post-flight log conversion runs
pwm_melody_play(&melody_ctx, mii_channel, ARRAY_SIZE(mii_channel));

// ... and back to the recovery beacon when it is done
pwm_melody_play(&melody_ctx, astronomia, ARRAY_SIZE(astronomia));
```

The melodies shipped in `aurora/lib/pwm_melody.h`:

| Melody | Used for |
| --- | --- |
| `astronomia` | Post-landing recovery beacon. |
| `mii_channel` | Post-flight log conversion in progress (arming is held off meanwhile). |

Playback loops until stopped, with a 500 ms gap between repeats.

:::{note}
A melody has to survive a single square-wave channel with no dynamics, so
pick tunes that are carried by their melody line. `mii_channel` is the
tune usually meant by "the Wii menu music"; the actual Wii System Menu
music is carried by its harmony and reduces to something unrecognisable
on one buzzer.
:::

## API Reference

```{doxygengroup} lib_pwm_melody
   :content-only:
```
