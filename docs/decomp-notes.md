# WWHD ↔ GameCube decompilation: findings

Function names for WWHD (`cking.rpx`) come from `tools/decomp/match.py` → `build/names.tsv`
(address, name, source file, evidence). Evidence `assert`/`profile` is near-certain, `strings`
high, `callgraph` ~85–90% (held-out test). Requires `tww/` (zeldaret/tww, built from your own
GameCube disc image for the call-graph stage).

## Camera (for frame interpolation)

| WWHD | Function | Notes |
|---|---|---|
| `024FFA3C` | `camera_execute` | per logic tick |
| `024FFC40` | `camera_draw` | builds the matrices each frame from `view.mLookat` (eye, center, up), `mFovy`, `mAspect`, `mNear`, `mFar`, `mBank` |
| `025F1EAC` | `mDoMtx_lookAt` | view matrix (called from `camera_draw`) |
| `028E9948` | `C_MTXPerspective` (unconfirmed) | projection, first matrix call in `camera_draw` |
| `028E91EC` | `PSMTXInverse` | inverse view |

Interpolation hook: the inputs of `camera_draw` (eye/center/up/fovy/bank) are a handful of
values; blending them between two ticks gives the in-between camera.

## Models

| WWHD | Function | Notes |
|---|---|---|
| `025E2DE0` | `mDoExt_modelUpdateDL` | per-model update from actor Draw |
| `027F4FE4` | `J3DModel::update` (structure) | 52 bytes: calls `calc` then `entry`; direct calls, no vtable in WWHD |
| `027F4D5C` | `J3DModel::calc` (structure) | joint/world matrices; called from actor Execute (e.g. `daBoko_c::execute`) |
| `027F4F1C` | `J3DModel::entry` (structure) | |
| `025E2BF4` → `027F55FC` → `027F53CC` → `027FDA54` | likely `viewCalc` → `calcDrawMtx` chain | world × view → draw matrices; to confirm |

WWHD restructured J3DModel (Hexa Drive's HD renderer): the GameCube virtual calls are direct
calls here, so names must be confirmed by call structure rather than vtables.

## Interpolation plan (sketch)

Logic stays at 30 Hz. For each rendered tick, record per model (J3DModel pointer) the world
matrices produced by `calc` and the camera inputs of `camera_draw`. For the in-between frame,
replay the frame's GX2 command list with draw matrices recomputed from blended world matrices and
the blended camera. Models without a previous tick (spawned, teleported) use the current matrices.
