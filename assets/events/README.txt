Event window banners (SatelliteSimEventWindows.cpp; docs/subsystems/sky-events.md "Event windows").

Drop one PNG per event here, named after the event's id. Size: 960 x 320 (3:1). Another size is scaled to
960 x 320 (stretched, not cropped), so keep the 3:1 shape. sRGB, RGBA or RGB. A missing file draws a labelled
placeholder. The folder ships with the app (assets/ is copied next to the exe and packaged whole).

    lunar_eclipse.png           every umbral lunar eclipse (partial and total)
    comet_inner_planets.png     the Great Comet crosses Mars's orbit
    comet_perihelion.png        the Great Comet survives perihelion
    comet_passes_earth.png      the Great Comet's closest approach
    comet_meteor_storm.png      the comet's meteor storm ("The Comet's Parting Omen")
    australasian_eclipse.png    the 2037-07-13 total solar eclipse
    suns_fury.png               the Sun's Fury (the CME's arrival)
    first_collision.png         the first collision of the cascade
    destroyed_10.png            10% of satellites destroyed
    destroyed_50.png            half of all satellites destroyed
    betelgeuse_explodes.png     the supernova's breakout
    betelgeuse_fades.png        the supernova fades below the star it was
    explode_warning.png         the warning before the first breakup before the Sun's Fury

A new file shows the next time that window opens (no rebuild needed in the exe's own assets/events folder; in the
source tree a build copies it there).
