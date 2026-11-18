---
pages: [new: simulation/sky-events.md, simulation/kessler.md, development/harness.md, development/controls-and-settings.md]
code: [src/simulations/SatelliteSimEventWindows.cpp, src/simulations/SatelliteSimUI.cpp, src/text/ui_text_en.inc, assets/events/README.txt, tools/make_event_sound.py]
---
## Now
The story's events open a modal window in the centre (a 960 x 320 banner from `assets/events/<id>.png` or a labelled
placeholder, the UTC date, a title, a short text, one button) and pause sim time; the button runs the clock on at 1x
forward. Twelve rows in `kEventWinDefs`: every umbral lunar eclipse (recurring), the Great Comet crossing Mars's orbit,
perihelion + 6 h, its closest approach, its meteor storm's peak, the 2037-07-13 Australasian eclipse, the Sun's Fury
(the CME's arrival), the cascade's first collision, 10% and 50% of the roster satellites present at the CME destroyed,
Betelgeuse's breakout and its fading below the star it was. A window opens when the RUNNING clock crosses the moment
(either way, any rate; the clock is put back to the moment; the shares forward only), or a Watch lands on it; a clock
SET past an event (bookmark, snapshot, rollback, `time set`, a Watch elsewhere) shows nothing: `eventWinAdvance`
compares the clock at this frame's start with last frame's end. Never during the opening's captions, a cinematic, video
export or HQ photo. Setting: Settings > Sky > EVENTS "Event windows", `sky_events.windows`, default on. A cue
(`assets/sound/ui/event.wav`, a synthesized placeholder) plays on the UI effects bus. The Explode button warns first
("WARNING / Exploding satellites disrupts the space-time continuum. Are you sure?" Yes / No / PRAISE BE TO THE SUN)
while the clock is before the CME and no player breakup before it exists: such a breakup voids the shipped prebake
(the world then starts at its step). Harness: `events list|state|on|off|fire <id>|ack [choice=] [soft=1]`, off in a
harness run until `events on`; `tools/harness/scripts/event_windows.satcmd`.

## History
New (2026-11-17), from the user's request for Stellaris-style event popups and a warning before the first explosion.
