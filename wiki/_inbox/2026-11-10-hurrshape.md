# Hurricane low cloud ramp, smoothing, eye bowl (2026-11-10, hurrshape)

- A hurricane's low cloud (the storm mass under the CDO) is now one deep, smooth cloud whose top rises as a ramp from
  0.35 of the span at 0.9 CDO radii to the CDO sheet's base at the eyewall; full overcast under it. Setting "Hurricane fill"
  `clouds_v2.hurricane_fill` (default 1, 0 = the old cut-up tower field).
- "Hurricane smoothing" `clouds_v2.hurricane_smoothing` (default 0.7): within 1.3 CDO radii the low cloud's lobes (x0.58),
  erosion (x0.44) and shallow cut-through (x0.3) are reduced; nothing changes outside a cyclone's reach.
- The eye is a bowl: the weather bake clears only 0.45-0.8 eye radii in 2D; the low field clears inside the eye radius
  and the eyewall slopes outward from ~0.16 of the span at the eye's edge to full height at 1.5 eye radii.
- Perf (RTX 3070 Ti, 1080p): under the CDO at 11.9 km GPU 29.5 -> 18.9 ms; eye from above 56.8 -> 46.0; orbit unchanged.
- Source: clouds_v2.glsl cv2LowA "THE HURRICANE'S RAMP" / "THE EYE AS A BOWL"; cloud_v2_weather.comp eyeAcc.
  Detail: docs/subsystems/weather.md, "The low field's ramp, its smoothing and the eye's bowl".
