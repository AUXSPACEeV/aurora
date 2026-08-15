# PWM Melodies

A landed rocket needs a way to communicate its position to the searching squad.
One simple way of making it easier to find is to play loud buzzer sounds in an
endless loop until the recovery team finds it.

But only playing loud noises is boring and annoying in testing.
Instead, use the PWM Melody API to play music of your liking:

```devicetree
/ {
   buzzer0: buzzer_0 {
      compatible = "auxspaceev,pwm-buzzer";
      pwms = <&ledc0 0 PWM_MSEC(200) PWM_POLARITY_NORMAL>;
   };
};
```

```c
#include <aurora/lib/pwm_melody.h>

// dt node that contains a "pwms" child node
static const struct pwm_dt_spec buzzer =
   PWM_DT_SPEC_GET(DT_NODELABEL(buzzer0));

// play astronomia from aurora/lib/pwm_melody.h
PWM_MELODY_CTX_DEFINE(melody_ctx, &buzzer, astronomia, 1024);

int main()
{
   pwm_melody_start(&melody_ctx);

   // --snip--
   // play as long as needed
   // --snip

   pwm_melody_stop(&melody_ctx);
}
```

## Several Tunes, One Buzzer

A board has one buzzer, so two playback threads would fight over the
same PWM output. Melodies therefore share a single context and are
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
