---
pages: [new: simulation/sky-events.md, simulation/kessler.md, development/harness.md]
code: [src/simulations/SatelliteSimEventWindows.cpp, tools/harness/scripts/event_banners.satcmd, tools/make_event_banners.py, assets/events/README.txt, tools/harness/scripts/event_windows.satcmd]
---
## Now
(Follows 2026-11-17-event-windows.md.) An event window sets the clock to 1x forward the moment it opens and holds it
there while open (a speed-up or reverse key meanwhile does not stick); the explode warning only pauses and restores the
clock it found. The clock is at the event's own moment when the window opens: a crossed moment within 1 ms at any rate,
and the 10% / 50% destroyed windows at their own second (the k-th earliest death among the satellites present at the
CME, k = ceil(share x N0)), not where the lagging roster walk noticed them. Each event shows once per crossing: a
window shown with the clock before its moment (a Watch, a backward crossing, the second of a queued pair) is not shown
again when the clock resumes forward through it. The explode warning is the title, the one sentence and the three
buttons (no sub-line). Every event (and the warning, `explode_warning.png`) ships a PLACEHOLDER banner: one in-app
capture of its scene, HUD hidden, cropped to 3:1 and scaled to 960 x 320, no text
(`tools/harness/scripts/event_banners.satcmd` + `tools/make_event_banners.py`; regenerate per assets/events/README.txt).
Harness: `events state` adds `clock_from_moment_s`, `now_utc`, `time_reverse`; `ui key .` / `ui key ,`.

## History
2026-11-18, the user: "When an event card fires, set timestep to 1x so when it resumes we aren't speeding away from
it". Measured before the change (fixed dt 0.05 s): crossed moments were already snapped (clock 1 ms from the moment
at 1 d / 1 w / 1 mo / 1 yr both ways, 3.05 s past it 60 frames after the button), but the time bar read the old rate
under the card (a key could set 1 yr/s and reverse there; Continue reset it); a backward crossing showed its window
twice and the queued meteor storm twice; 10% at 1 mo/s opened at 2037-09-22, days past its second, with the clock
left there; at 1 yr/s the 50% window never opened (the walk ended under Betelgeuse's window and took 68% as the
baseline). After: 10% at 1 mo/s opens at its second 2037-09-10 04:41:08, 50% (1 yr/s) at 2037-10-24 14:06:48; the step
back replays the Kessler cascade (~3 s hitch, the clock held at the replay's frontier a few seconds), and the share
baseline never drops while the clock runs forward nor while the replay is under way (the replay's partial dead
seconds made 10% open a second time).
