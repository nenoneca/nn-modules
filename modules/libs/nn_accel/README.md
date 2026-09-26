# nn_accel — shared accelerator services: interface + zero-copy transport

Design: `nn-media-stream/docs/ACCEL_SERVICES_DESIGN.md`.

* `proto/nn_accel.proto` — protobuf over AF_UNIX **SOCK_SEQPACKET** (one
  datagram = one message). Not gRPC: it cannot pass file descriptors, and
  fd passing is how frames are shared.
* `src/shmpool.c`, `include/nn_accel/shmpool.h` — C side (cameras).
* `py/shmpool.py` — Python side (services). **The struct layout must match
  the C header exactly**; a mismatch is silent corruption, not an error.
* `tests/test_shmpool.c` — two processes, zero copy, backpressure.

Generate stubs with a modern protoc (Debian's 3.12 is too old for current
python-protobuf):

    python3 -m grpc_tools.protoc -I proto --python_out=gen proto/nn_accel.proto

Message sizes measured: InferReq 25 B, InferResp (1 detection) 37 B,
Capabilities (2 devices) 55 B — the frame itself never crosses the socket.
