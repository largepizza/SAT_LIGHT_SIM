---
pages: ["new: simulation/launches.md", sound/ambience.md, development/harness.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.h, src/AmbientSynth.cpp, src/simulations/SatelliteSimAmbience.cpp, data/launches.json]
---
## Now
Every upper stage deorbits: 30-90 min after deploying it pitches over to retrograde (120 s, through the vertical),
burns for 12-60 s (the dv to a perigee of 20 km, 45 km for Starship's returning ship, at ~12% of its thrust) onto an
ellipse with its apoapsis at the burn, and reaches the entry interface (100 km) about half an orbit (~32 min) later,
with position and velocity continuous through the burn and into the entry. An expendable stage re-enters there as a
fireball; Starship's ship pitches up to its entry attitude (nose 60 deg above the velocity) after the burn and flies
its 8-min plasma glow. The booster flips over through the vertical after staging and again after an RTLS boostback
(at most ~18 deg/s); no stage's attitude changes in a snap.

Payloads leave the dispenser one after another (`deploy_interval_s` in launches.json: Starship 8 s, others 2 s), each
appearing at the dispenser (Starship: the bay door, 31 m up the ship from its aft end, on the zenith side) and drifting
away at 0.3-0.55 m/s, mostly upward.

A rocket's roar is heard by the latest emission whose sound reaches the listener now (a scan back over 440 s, either
crossing direction, so a booster's supersonic descent is heard in reverse order); it stops when the burns that made it
have been heard. Rocket voices are their own kind in the thunder synth: a low roar under a 62-Hz sub layer, Mach-wave
crackle (out by 40 km), a punch as it first arrives (x3 over ~1.4 s, a crackle burst and a 70-30 Hz thump) and an echo
(FDN, t60 3.4 s).

Harness: `launch deorbit` / `launch deploy` frame the next sunlit stage at those moments; `launch state` lists
`upper_stages` (timeline, burning, nose angle from retrograde) and each sound's stage / burn / T+; `launch check`
reports the fastest nose turns and the upper stage's largest 1-s step through the burn and entry (no jump: ~7.9 km).

## History
Issues 20261019_180502 / _180951 / _175639 (the user's QA): Starship's ship never turned or burned and jumped from its
350-km orbit to 100 km at the entry interface 40 min after deploying; only expendable stages burned (40 s, sinking
30 km on their circle); stage axes switched instantly at staging and for the burn. The whole train appeared at once,
a string 120 m apart behind the stage. Clicking a rocket's mesh selected the mesh slot (only last frame's state), so a
selected or followed rocket froze there (still "up there" seven months later) and, followed, was drawn twice. The
retarded-time fixed-point iteration did not converge for a fast distant stage: an upper stage 1200 km downrange was
heard at full level 300 m from its pad minutes after lift-off. First cut of the smoothing averaged nose vectors:
~340 deg/s spins after an RTLS boostback (opposite vectors); replaced by averaging a pitch angle.
