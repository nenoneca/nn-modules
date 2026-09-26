# nn_infer — edge-side AI inference (design)

## Goal
A portable inference module so cameras run detection at the edge; the media
service then streams + overlays edge boxes and only runs its own inference
when the hub says so.

## Layering
- **Core** (`include/nn_infer/nn_infer.h`, `src/core.c`): portable tier —
  nn_osal only. Query caps / run / result types. No platform includes.
- **PALs** (`src/pal/<name>/`): backend dirs (same convention as nn_osal
  backends, exempt from the portable-tier lint):
  - `byai_dlr` — BeagleY-AI: **in-process** TIDL via neo-ai-dlr's C API
    (`libdlr.so`, dlopen'd). Runs INSIDE the edgeai container (TI userspace);
    the whole camera app runs there — it already owns the tiovx pipeline.
  - `stub` — fixed fake detections; CI and protocol tests.
  - future: OPi NOE, ESP32-P4 espdl.

## API (v0)
- `nn_infer_init(model_dir)` / `nn_infer_deinit()`
- `nn_infer_query_caps(&caps)` → `{width,height,format,fps,model,nclasses}`
  — the input the PAL WANTS. The app resamples camera output to match
  (on byai: a second tiovxmultiscaler branch = free HW resize).
- `nn_infer_run(&in, &out)` — synchronous; caller owns the thread.
  - in: `{nn_osal_buf_t buf; uint64 ts_ms}` — buf is a black box
    (`NN_OSAL_BUF_MEM` ptr+len, or `NN_OSAL_BUF_DMABUF` fd on Linux;
    PALs use `nn_osal_buf_map()`).
  - out: `{ts_ms, count, det[]{x,y,w,h,class_id,conf_x1000}}` — coords in
    the INPUT's pixel space; the app rescales to stream space before sending.

## Wire (device → media service)
Same encrypted record stream as video ('V' 0x56, 'A' 0x41, 'S' 0x53):
- **NEW `'D'` = 0x44 detect record.** Header ts_ms = capture ts of the
  inferred frame. Payload LE: `[u8 ver=0][u8 count][u16 rsvd]` +
  `count × {u16 x,y,w,h (stream px), u16 class_id, u16 conf_x1000}`.
- **Capability advert:** first 'S' status record after handshake carries
  `{"infer": {"model": "...", "classes": N, "fps": F, "w": W, "h": H}}`.
  Service sets `edge_caps` for the session; hub reads it via the service's
  camera-settings API (config sync already forwards camera settings).

## Media service policy (per camera, hub-adjustable)
`infer_mode` ∈ {auto,on,off}, persisted in the service keydir, REST
`GET/POST /api/infer/mode` (hub reaches it through the existing camera
proxy). Effective local inference =
`on`, or `auto && no edge_caps advertised`. Off = never.
- local inference INACTIVE → engine's YOLO never runs; edge 'D' records
  update `last_dets` (UI overlay unchanged — same store) and drive event
  start/keep-alive; motion ticks still handle quiet-finalize.
- local inference ACTIVE → service behaves exactly as today (its results
  override edge for event decisions; edge boxes still shown if no local).
