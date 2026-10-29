// comet_frame.glsl — the comet buffer's frame header + particles (SatelliteSimComets.cpp GpuCometFrame, comet::GpuGrain).
#ifndef COMET_FRAME_GLSL
#define COMET_FRAME_GLSL
struct CometFrame
{
    mat4 skyView;   // ENU -> camera
    vec4 proj;      // x tan(fovY / 2), y aspect, z splat target width, w height
    vec4 eclX, eclY, eclZ; // rows of ecliptic -> ENU
    vec4 misc;      // x lum cull, y largest splat radius (px), z the splat pixel's angle (rad), w coma count
    vec4 comet[6];  // per tail slot s: [2s] xyz anchor - observer (AU), w the frame's time - the build's (days);
                    //                  [2s+1] xyz the anchor (heliocentric ecliptic AU), w the comet's rSat^2 (AU^2)
    vec4 coma[9];   // per slot: [3s] head ENU, w rho_s (rad); [3s+1] x L (kLum flux / (2 pi rho_s)), y rho0, z gas share,
                    // w on; [3s+2] rgb dust colour
    vec4 pad[8];    // [0].x the tail zoom detail (comets.tail_detail; 0 = off), w its debug view; [2] the field atlas (as [1]); [1] the splat atlas: xy level 0 (px), zw the atlas
};
struct Grain
{
    vec4 p; // xyz position at the build minus the anchor (AU); w flux weight
    vec4 v; // xyz heliocentric velocity (AU/day); w spread (AU) at the build
    vec4 q; // x 1 - beta (0: no gravity), y spread growth (AU/day), z age at the build (days), w kind + 4 x slot
    uvec4 d; // dust: half floats (dP/dte xy), (dP/dte z, dP/dlnbeta x), (dP/dlnbeta yz) (AU/day, AU); w the float bits
             // of the field's emission time here (days from perihelion); ions 0 (Comets.h GpuGrain)
};
layout(std430, set = 0, binding = 0) readonly buffer CometBuf
{
    CometFrame F;
    Grain grains[];
};
#endif
