import asyncio
from bleak import BleakScanner, BleakClient
import struct
import binascii
import time

IMU_SVC_UUID = "0000ff00-0000-1000-8000-00805f9b34fb"
IMU_CHAR_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"
TARGET_MFG = bytes([0x12, 0x34])


def handle_notification(sender, data):
    try:
        ax, ay, az, gx, gy, gz = struct.unpack("<6f", data)
        print(f"IMU | Accel: {ax:7.4f} {ay:7.4f} {az:7.4f} | Gyro: {gx:7.4f} {gy:7.4f} {gz:7.4f}")
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
        asyncio.run(main())
    except KeyboardInterrupt:
        print('\nExiting by user')