---
pages: [new: simulation/sky-events.md, development/harness.md]
code: [src/simulations/SatelliteSimEventWindows.cpp, tools/harness/scripts/event_windows.satcmd]
---
## Now
(Follows 2026-11-17-event-windows.md and 2026-11-18-event-cards-1x-banners.md, and corrects them where they say a
window opens on a backward crossing.) An event window opens only when the running clock crosses the event's moment
going FORWARD, at any rate, or when a Watch takes the player to it. A clock running back, or set past a moment, opens
nothing: rewinding is navigation, and the window shows when time next runs forward through the moment. Every
moment row triggers the same way (the first collision's "forward only" trigger is now the rule for all; harness
`events list` reports `moment` or `share destroyed`, and `events state` -> `how` is never "crossed backward"). A window
shown with the clock before its moment (a Watch's landing, the second of a queued pair) counts as the showing for the
next forward crossing, unless the clock runs back first, in which case that next forward crossing shows it again.
Settings > Sky > EVENTS "Event windows" (`sky_events.windows`) turns the windows off; its tooltip says rewinding past an
event shows nothing.

## History
2026-11-18 (issue_20261118_181440, "Impossible to reverse past events"): a backward crossing opened the window, put
the clock 1 ms before the moment and the button resumed 1x FORWARD; the clock then crossed the moment forward (the same
showing, dropped from the list), and the next reverse met it again: a rewind could never get past an event. The user
chose "a backward crossing opens no card" over remembering backward showings.
