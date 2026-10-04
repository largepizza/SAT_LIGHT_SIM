# Using SAT LIGHT SIM

SAT LIGHT SIM is a real-time view of the night (and day) sky as it would look with the satellite
megaconstellations that are planned or already flying: Starlink, OneWeb, Amazon LEO, Guowang, orbital mirrors
such as Reflect Orbital, a million-satellite orbital data-centre disk, space stations and debris. You can stand
anywhere on Earth or fly anywhere up to about 100 000 km, at any speed of time, and see every satellite as it
would appear: its real brightness from its shape and attitude, its glint, its flare, its beam on the ground,
through a physically based atmosphere with clouds, terrain, sea, city lights, aurora and the Moon.

This section is for players. It explains how to install the program, move around, find things and get good
pictures out of it. It does not explain how anything is computed: the [Simulation](../simulation/index.md) and
[Rendering](../rendering/index.md) sections do that, and [Modding](../modding/index.md) explains how to change
the satellites themselves.

## Who it is for

- **Anyone curious** about what the sky will look like with hundreds of thousands to millions of satellites in it.
- **Astronomers and dark-sky advocates** who want a realistic, magnitude-calibrated sense of how bright a
  constellation is from a given place and time. The brightness model is checked against published observation
  campaigns; see [Accuracy](../accuracy/index.md).
- **Photographers and film makers**: an HQ photo mode and a cinematics editor with keyframes, follow shots and
  frame export.
- **Modders** who want to add their own constellations or satellite shapes.

## Map of this section

| Page | What it covers |
|---|---|
| [Getting started](getting-started.md) | Download, install, build from source, the first launch, where files are kept |
| [Controls](controls.md) | Keyboard, mouse and gamepad, rebinding, movement speed, follow mode, typing values |
| [Features tour](features.md) | Selecting satellites and planets, the satellite window, magnitude traces, time, photos, cinematics, the settings tabs |
| [Graphics settings](graphics-settings.md) | Presets, render scale, temporal anti-aliasing, the sliders that cost the most |

## The screen at a glance

```
+--------------------------------------------------------------------+
|                       [follow chip, when following]                |
|                                                                    |
|                    sky, satellites, Earth                          |
|                                                                    |
|            [selection panel next to the selected object]           |
|                                                                    |
| [time bar: UTC time, speed,     ]          [lat | lon | alt  MSL  ]|
| [ < || > R  camera photo film * ]          [              gear    ]|
+--------------------------------------------------------------------+
```

- The **time bar** (bottom left) shows the simulated UTC time and the time speed, with buttons for slower,
  pause, faster, reverse, screenshot, HQ photo, the cinematics window and star trails.
- The **position readout** (bottom right) shows the observer's latitude, longitude and altitude. Hover any of
  them and scroll to change it. The small button beside the altitude switches between height above sea level
  (MSL) and above the ground (AGL). The gear opens Settings.
- **Tab** hides and shows the whole interface.

!!! tip
    The program starts at a fixed moment in late 2036 and runs on real UTC: the Sun, Moon, planets and eclipses
    are where they really will be at that time. Time speed goes from real time up to one year per second.
