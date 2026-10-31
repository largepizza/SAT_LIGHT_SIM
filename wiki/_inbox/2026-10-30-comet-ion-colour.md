---
pages: ["new: simulation/comets.md", "rendering/atmosphere-and-sky.md"]
code: [shaders/comet_splat.vert, shaders/comet_composite.frag]
---
## Now
The ion tail is CO+ fluorescence (the 400-430 nm tail bands, plus a little H2O+) in a deep indigo, linear
(0.16, 0.26, 0.72): noticeably darker and greyer than the electric blue it replaces, and still clearly blue against the
dust tail's warm white (1.05, 0.98, 0.86). The colour is a constant in `comet_splat.vert`'s ion branch and reaches the
screen only through `comet_composite.frag`, which never tints it: it sums the particles' light from the splat atlas,
scales it by the line of sight's extinction (one scalar for all channels, so no differential reddening), the dark-sky
gate against the sky's own surface brightness, the cloud occlusion and the exposure gain, and draws it through a
shoulder on the brightest channel, `K (1 - (1 + x / (K p))^-p)` with K 0.92, p 0.5, divided by that channel: the hue
survives any brightness.

## History
The colour's size does not set how bright the tail draws, its shape does. The composite divides by the brightest
channel (the shoulder keeps the hue), so a colour whose blue sits far above its luminance is lifted hard toward the
display's white and reads pale: (0.16, 0.42, 1.95) was a pastel cornflower (issue 20261030_140112), (0.05, 0.17, 2.1)
an electric blue. The new colour's ratios are (0.222, 0.361, 1.0) against (0.024, 0.081, 1.0), its luminance 0.272
against 0.284 (so the photometry and the dark-sky gate barely move, 0.02 mag), and its brightest channel over that
luminance 2.65 against 7.40 (the dust's is 1.06): the same surface brightness draws about 15% darker and much less
saturated. This note supersedes the ion colour in `wiki/_inbox/2026-10-30-comet-graphics.md`.
