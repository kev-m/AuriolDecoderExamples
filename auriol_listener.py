import socket
import json
import datetime
import sys


def format_temperature(value):
    """Return temperature formatted with one decimal place, or N/A."""
    try:
        return f"{float(value):.1f}"
    except (TypeError, ValueError):
        return "N/A"


def print_auriol_packet(timestamp, msg):
    """Print a compact line for valid Auriol payloads.

        {
            "sensor_id":"%04X",
            "channel":%u,
            "temperature_c":%.1f,
            "humidity_pct":%u,
            "battery":"%s"
        }

    
    """
    sensor_id = msg.get("sensor_id", "unknown")
    temp = format_temperature(msg.get("temperature_c"))
    humidity = msg.get("humidity_pct")
    battery = msg.get("battery")

    fields = [
        f"[{timestamp}]",
        f"ID:{sensor_id}",
        f"Temp:{temp}C",
    ]
    if humidity > 0:
        fields.append(f"Hum:{humidity}%")
    fields.append(f"Batt:{battery}")
    print(" | ".join(fields))


def start_listener(port=4242):
    """
    Listens for UDP heartbeat broadcasts from the Auriol gateway transmitter.
    """
    # Create a UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # Allow multiple listeners to bind to the same port if needed
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # Bind to all interfaces
    try:
        sock.bind(('', port))
    except PermissionError:
        print(
            f"Error: Permission denied binding to port {port}. Try using a higher port or sudo.")
        sys.exit(1)
    except Exception as e:
        print(f"Error binding to port {port}: {e}")
        sys.exit(1)

    print(f"--- Auriol Heartbeat Listener Started ---")
    print(f"Listening on UDP port: {port}")
    print(f"Press Ctrl+C to stop\n")

    try:
        while True:
            data, addr = sock.recvfrom(4096)
            timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

            try:
                # Strip trailing null characters or padding whitespace
                payload = data.decode('utf-8').strip('\x00').strip()
                msg = json.loads(payload)

                is_auriol = msg.get("type") == "Auriol"
                has_sensor = "sensor_id" in msg
                has_temp = "temperature_c" in msg
                if is_auriol and has_sensor and has_temp:
                    print_auriol_packet(timestamp, msg)

            except json.JSONDecodeError:
                # Catch raw serial print remnants or split lines
                print(f"[{timestamp}] {addr[0]} [RAW/MALFORMED] -> {data}")
            except Exception as e:
                print(f"[{timestamp}] Error parsing frame: {e}")

    except KeyboardInterrupt:
        print("\nListener stopped by user.")
    finally:
        sock.close()


if __name__ == "__main__":
    # The port matches HEARTBEAT_PORT in the ESP01S firmware
    start_listener(4242)
