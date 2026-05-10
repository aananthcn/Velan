#!/usr/bin/env python3
"""
Velan voice assist trigger test client.

Simulates a button press or HMI event by writing VOICE_ASSIST_TRIGGER to
the VHAL core server via SetValues.  VHAL core must be running (port 50051).

Usage:
    python3 trigger_velan.py [--server localhost:50051]

Commands at the prompt:
    on   — set VOICE_ASSIST_TRIGGER = 1  (Velan starts recording)
    off  — set VOICE_ASSIST_TRIGGER = 0  (Velan stops recording)
    quit — exit

Install deps (one time):
    pip install grpcio grpcio-tools
"""

import argparse
import os
import pathlib
import readline
import sys

# ---------------------------------------------------------------------------
# Property ID: VehiclePropertyGroup::VENDOR | VehicleArea::GLOBAL
#            | VehiclePropertyType::INT32   | unique id 0x0001
# Must match the constant in src/main.cpp.
# ---------------------------------------------------------------------------
VOICE_ASSIST_TRIGGER = 0x21400001

TRIGGER_OFF = 0
TRIGGER_ON  = 1

# ---------------------------------------------------------------------------
# Generate Python gRPC stubs from the VHAL proto files (one-time setup)
# ---------------------------------------------------------------------------
_HERE     = os.path.dirname(os.path.abspath(__file__))
PROTO_DIR = "/home/aananth/labs/networking/vhal-core/test/vhal"
GEN_DIR   = os.path.join(_HERE, 'gen')


def _generate_stubs() -> None:
    os.makedirs(GEN_DIR, exist_ok=True)

    try:
        from grpc_tools import protoc
    except ImportError:
        sys.exit("grpc_tools not found. Run: pip install grpcio grpcio-tools")

    # Collect all .proto files under the VHAL proto dir
    all_protos = [str(p) for p in pathlib.Path(PROTO_DIR).rglob('*.proto')]

    rc = protoc.main([
        'grpc_tools.protoc',
        f'--proto_path={PROTO_DIR}',
        f'--proto_path=/usr/include',   # google/protobuf/empty.proto
        f'--python_out={GEN_DIR}',
        f'--grpc_python_out={GEN_DIR}',
    ] + all_protos)

    if rc != 0:
        sys.exit(f"protoc failed (rc={rc}). Check proto dir: {PROTO_DIR}")

    # Create __init__.py in every sub-package so Python can import them
    for dirpath, _, filenames in os.walk(GEN_DIR):
        if any(f.endswith('.py') for f in filenames):
            init = os.path.join(dirpath, '__init__.py')
            if not os.path.exists(init):
                open(init, 'w').close()


if not os.path.exists(os.path.join(GEN_DIR, 'VehicleServer_pb2.py')):
    print("Generating gRPC stubs from VHAL protos...")
    _generate_stubs()
    print("Done.\n")

sys.path.insert(0, GEN_DIR)
import VehicleServer_pb2        # noqa: E402
import VehicleServer_pb2_grpc  # noqa: E402
from android.hardware.automotive.vehicle import (  # noqa: E402
    VehiclePropValue_pb2 as vval_pb2,
    VehiclePropValueRequest_pb2 as vreq_pb2,
)

import grpc  # noqa: E402

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_trigger_request(request_id: int, state: int) -> vreq_pb2.VehiclePropValueRequests:
    requests = vreq_pb2.VehiclePropValueRequests()
    req = requests.requests.add()
    req.request_id = request_id
    req.value.prop    = VOICE_ASSIST_TRIGGER
    req.value.area_id = 0
    req.value.int32_values.append(state)
    return requests


def _send(stub: VehicleServer_pb2_grpc.VehicleServerStub,
          state: int, label: str, req_id: int) -> None:
    try:
        result = stub.SetValues(_make_trigger_request(req_id, state))
        statuses = [r.status for r in result.results]
        print(f"  → {label} sent  (status: {statuses})")
    except grpc.RpcError as e:
        print(f"  → {label} failed: {e.code()} — {e.details()}")

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Velan voice assist trigger client")
    parser.add_argument('--server', default='localhost:50051',
                        help="VHAL gRPC server address (default: localhost:50051)")
    args = parser.parse_args()

    channel = grpc.insecure_channel(args.server)
    stub    = VehicleServer_pb2_grpc.VehicleServerStub(channel)

    print(f"Connected to VHAL server at {args.server}")
    print("Commands: on | off | quit\n")

    readline.parse_and_bind('tab: complete')

    req_id = 1
    while True:
        try:
            cmd = input("trigger> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if cmd == 'on':
            _send(stub, TRIGGER_ON,  'TRIGGER_ON ', req_id)
            req_id += 1
        elif cmd == 'off':
            _send(stub, TRIGGER_OFF, 'TRIGGER_OFF', req_id)
            req_id += 1
        elif cmd in ('quit', 'q', 'exit'):
            break
        elif cmd:
            print("  Unknown command. Use: on | off | quit")

    channel.close()


if __name__ == '__main__':
    main()
