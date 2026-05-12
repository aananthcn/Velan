#!/usr/bin/env python3

# Copyright 2026 Aananth C N
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Velan voice assistant trigger test client.

Press ENTER       →  VOICE_ASSIST_TRIGGER ON  (Velan starts recording)
Press BACKSPACE   →  VOICE_ASSIST_TRIGGER OFF (manual stop)
Press ESC         →  VOICE_ASSIST_TRIGGER OFF if recording, quit if idle
Ctrl-C            →  quit

No external packages beyond grpcio are needed.

Install deps (one time):
    pip install grpcio grpcio-tools
"""

import argparse
import os
import pathlib
import select
import sys
import termios
import tty

# ---------------------------------------------------------------------------
# Property ID: VehiclePropertyGroup::VENDOR | VehicleArea::GLOBAL
#            | VehiclePropertyType::INT32   | unique id 0x0001
# Must match the constant in src/main.cpp.
# ---------------------------------------------------------------------------
VOICE_ASSIST_TRIGGER = 0x21400001

TRIGGER_OFF = 0
TRIGGER_ON  = 1

# ENTER starts a trigger; BACKSPACE or ESC stops it.
START_KEYS = {'\r', '\n'}
STOP_KEYS  = {'\x7f', '\x08'}   # BACKSPACE (DEL) and BS control char

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

    all_protos = [str(p) for p in pathlib.Path(PROTO_DIR).rglob('*.proto')]

    rc = protoc.main([
        'grpc_tools.protoc',
        f'--proto_path={PROTO_DIR}',
        f'--proto_path=/usr/include',
        f'--python_out={GEN_DIR}',
        f'--grpc_python_out={GEN_DIR}',
    ] + all_protos)

    if rc != 0:
        sys.exit(f"protoc failed (rc={rc}). Check proto dir: {PROTO_DIR}")

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
# Push-to-talk loop
# ---------------------------------------------------------------------------

def _ptt_loop(stub: VehicleServer_pb2_grpc.VehicleServerStub) -> None:
    """
    Puts stdin in raw mode and waits for discrete keypresses.

    State machine:
      idle      — ENTER → TRIGGER_ON
                  ESC   → quit
      recording — BACKSPACE or ESC → TRIGGER_OFF → idle
    """
    fd           = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    triggered    = False
    req_id       = 1

    print("Press ENTER to start recording.  BACKSPACE or ESC to stop.  Ctrl-C to quit.\n")

    def send(state: int, label: str) -> None:
        nonlocal req_id
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        _send(stub, state, label, req_id)
        req_id += 1
        tty.setraw(fd)

    try:
        tty.setraw(fd)

        while True:
            ch = sys.stdin.read(1)

            if ch == '\x03':                 # Ctrl-C → quit
                break

            if ch == '\x1b':
                # Peek ahead: escape sequences (arrow keys etc.) send more bytes
                # immediately; a plain ESC press has nothing following it.
                more, _, _ = select.select([sys.stdin], [], [], 0.05)
                if more:
                    sys.stdin.read(1)        # discard '[', 'O', etc.
                    continue                 # ignore escape sequence

                # Plain ESC
                if triggered:
                    triggered = False
                    send(TRIGGER_OFF, 'TRIGGER_OFF')
                    termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
                    print("Press ENTER to record again.  Ctrl-C to quit.\n")
                    tty.setraw(fd)
                else:
                    break                    # ESC while idle → quit

            elif ch in STOP_KEYS:            # BACKSPACE
                if triggered:
                    triggered = False
                    send(TRIGGER_OFF, 'TRIGGER_OFF')
                    termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
                    print("Press ENTER to record again.  Ctrl-C to quit.\n")
                    tty.setraw(fd)

            elif ch in START_KEYS:           # ENTER
                if not triggered:
                    triggered = True
                    send(TRIGGER_ON, 'TRIGGER_ON ')
                    termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
                    print("Recording... press BACKSPACE or ESC to stop.\n")
                    tty.setraw(fd)

    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        if triggered:
            print("\n[trigger] Sending TRIGGER_OFF before exit...")
            _send(stub, TRIGGER_OFF, 'TRIGGER_OFF', req_id)

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Velan push-to-talk trigger client")
    parser.add_argument('--server', default='localhost:50051',
                        help="VHAL gRPC server address (default: localhost:50051)")
    args = parser.parse_args()

    channel = grpc.insecure_channel(args.server)
    stub    = VehicleServer_pb2_grpc.VehicleServerStub(channel)

    print(f"Connected to VHAL server at {args.server}")

    try:
        _ptt_loop(stub)
    except Exception as e:
        print(f"\n[trigger] Error: {e}")
    finally:
        print("[trigger] Bye.")
        channel.close()


if __name__ == '__main__':
    main()
