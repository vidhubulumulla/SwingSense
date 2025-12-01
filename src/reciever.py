import asyncio
from bleak import BleakScanner, BleakClient
import struct
import binascii
import time
import os
import threading
import sys

IMU_SVC_UUID = "0000ff00-0000-1000-8000-00805f9b34fb"
IMU_CHAR_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"
TARGET_MFG = bytes([0x12, 0x34])

# Recording/file state
recording = False
data_file = None
file_count = 0
base_filename = None
# Track current and last-closed filenames so user can delete the last file
current_fname = None
last_closed_fname = None
last_closed_index = 0

# Lock for coordinating stdin thread and notification handler
state_lock = threading.Lock()


def handle_notification(sender, data):
    # Declare globals used in this function
    global recording, data_file, file_count, base_filename
    global current_fname, last_closed_fname, last_closed_index

    # Two notification types supported:
    # - 1 byte: header packet (0x01 = start, 0x02 = stop)
    # - 24 bytes: six float32 values (ax,ay,az,gx,gy,gz)
    if len(data) == 1:
        b = data[0]
        if b == 0x01:
            print("Notification: header -> START (0x01)")
            # Start a new recording file (numbered)
            # close any previous file (defensive)
            if data_file:
                try:
                    data_file.close()
                except Exception:
                    pass
                data_file = None
            file_count += 1
            # ensure output directory exists
            try:
                os.makedirs(base_filename, exist_ok=True)
            except Exception as e:
                print(f"Failed to create directory {base_filename}: {e}")
            fname = os.path.join(base_filename, f"{base_filename}_{file_count}.csv")
            try:
                data_file = open(fname, 'w', buffering=1)
                recording = True
                with state_lock:
                    current_fname = fname
                print(f"Recording started -> writing to {fname}")
            except Exception as e:
                data_file = None
                recording = False
                print(f"Failed to open {fname} for writing: {e}")
        elif b == 0x02:
            print("Notification: header -> STOP  (0x02)")
            # Stop recording and close file
            if data_file:
                try:
                    data_file.close()
                    print("Recording stopped -> file closed")
                    # remember last closed file for possible deletion
                    with state_lock:
                        last_closed_fname = current_fname
                        last_closed_index = file_count
                        current_fname = None
                except Exception as e:
                    print(f"Error closing file: {e}")
                data_file = None
            recording = False
            # reset any pending header state on explicit stop
        else:
            print(f"Notification: header -> unknown byte 0x{b:02x}")
        return

    try:
        ax, ay, az, gx, gy, gz = struct.unpack("<6f", data)
        print(f"IMU | Accel: {ax:7.4f} {ay:7.4f} {az:7.4f} | Gyro: {gx:7.4f} {gy:7.4f} {gz:7.4f}")
        # If we're recording, write CSV line (exclude headers)
        if recording and data_file:
            try:
                line = f"{ax:.6f},{ay:.6f},{az:.6f},{gx:.6f},{gy:.6f},{gz:.6f}\n"
                data_file.write(line)
            except Exception as e:
                print(f"Error writing to file: {e}")
    except struct.error:
        print(f"Notification: unexpected length={len(data)} bytes -> {binascii.hexlify(data)}")


async def find_swing_device(timeout=20.0):
    """Find a device advertising the IMU service, name 'SwingSense', or our manufacturer bytes."""
    print(f"Scanning for device (timeout={timeout}s)...")

    def adv_filter(device, adv):
        # 1) manufacturer data
        m = adv.manufacturer_data
        if isinstance(m, dict):
            for v in m.values():
                if isinstance(v, (bytes, bytearray)) and v.startswith(TARGET_MFG):
                    return True
        elif isinstance(m, (bytes, bytearray)):
            if m.startswith(TARGET_MFG):
                return True
        # 2) local name
        if adv.local_name and "SwingSense" in adv.local_name:
            return True
        # 3) service UUIDs
        if adv.service_uuids:
            for u in adv.service_uuids:
                if 'ff00' in u.lower() or IMU_SVC_UUID == u.lower():
                    return True
        return False

    try:
        dev = await BleakScanner.find_device_by_filter(adv_filter, timeout=timeout)
        return dev
    except Exception as e:
        print("Scanner error:", e)
        return None


async def _is_connected(client):
    """Normalize bleak's is_connected which may be a property, method or coroutine."""
    try:
        attr = getattr(client, 'is_connected', None)
        if callable(attr):
            res = attr()
            if asyncio.iscoroutine(res):
                return await res
            return bool(res)
        return bool(attr)
    except Exception:
        return False


async def connect_and_listen(device):
    backoff = 1.0
    while True:
        try:
            addr = getattr(device, 'address', str(device))
            print(f"Connecting to {addr}...")
            async with BleakClient(device) as client:
                connected = await _is_connected(client)
                print("Connected:", connected)
                await client.start_notify(IMU_CHAR_UUID, handle_notification)
                print("Started notifications. Listening... (Ctrl+C to quit)")
                # stay connected until disconnected
                while await _is_connected(client):
                    await asyncio.sleep(1)
                print("Disconnected from peripheral")
        except Exception as e:
            print("Connection error:", e)
        print(f"Reconnecting in {backoff:.1f}s...")
        await asyncio.sleep(backoff)
        backoff = min(backoff * 2.0, 30.0)


async def main():
    print("Receiver starting — press Ctrl+C to stop")
    while True:
        dev = await find_swing_device(timeout=20.0)
        if dev is None:
            print("Device not found. Retrying in 5s...")
            await asyncio.sleep(5)
            continue

        name = getattr(dev, 'name', None)
        print(f"Found device: {name} [{dev.address}]")
        # attempt connection and listen (this blocks until disconnect)
        await connect_and_listen(dev.address)


if __name__ == '__main__':
    try:
        # Ask user for base filename to use for recordings
        base = input("Enter base filename for recordings (no extension, e.g. 'swing'): ").strip()
        if not base:
            base = "recording"
        base_filename = base
        print(f"Using base filename: {base_filename}")
        # Start a background thread to accept simple stdin commands
        def stdin_thread():
            global file_count, last_closed_fname, last_closed_index
            print("Commands: 'd' + Enter -> delete last closed file and redo it")
            while True:
                try:
                    line = sys.stdin.readline()
                    if not line:
                        break
                    cmd = line.strip()
                    if cmd.lower() == 'd':
                        with state_lock:
                            if last_closed_fname and os.path.exists(last_closed_fname):
                                try:
                                    os.remove(last_closed_fname)
                                    print(f"Deleted {last_closed_fname}")
                                    # decrement file_count so next file reuses this index
                                    if last_closed_index > 0:
                                        file_count -= 1
                                    # clear last closed state
                                    last_closed_fname = None
                                    last_closed_index = 0
                                except Exception as e:
                                    print(f"Failed to delete {last_closed_fname}: {e}")
                            else:
                                print("No recently closed file to delete")
                except Exception:
                    break

        t = threading.Thread(target=stdin_thread, daemon=True)
        t.start()
        asyncio.run(main())
    except KeyboardInterrupt:
        print('\nExiting by user')