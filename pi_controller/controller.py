import os
import serial
import serial.tools.list_ports
import time
import threading
import queue
import logging
import sys
import re
import struct

# --- Configuration ---
BAUD_RATE = 115200

# Default port order: USB first (when ESP connected by cable), then Pi GPIO UART
DEFAULT_PORT_ORDER = (
    "/dev/ttyUSB0",   # USB serial (CP2102/CH340)
    "/dev/ttyACM0",   # USB serial (some boards)
    "/dev/serial0",   # Pi primary UART (symlink on Raspberry Pi)
    "/dev/ttyS0",     # Pi mini UART or primary on some images
)


def get_default_serial_port():
    """Use first port that exists. Prefer USB, then Pi UART."""
    for path in DEFAULT_PORT_ORDER:
        if os.path.exists(path):
            return path
    return "/dev/ttyUSB0"  # fallback so error message is clear

# --- Logging Setup ---
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger("PiController")


def _dtr_reset_master(serial_conn):
    """Toggle DTR to reset ESP32 Master (USB only; no-op on GPIO UART e.g. ttyS0)."""
    try:
        serial_conn.dtr = False
        time.sleep(0.1)
        serial_conn.dtr = True
    except Exception:
        pass


def _log_serial_port_help(requested_port):
    """On connection failure, list available serial ports and usage hint."""
    logger.info(
        "Tip: Pass the correct port, e.g. python3 controller.py /dev/ttyS0 (GPIO UART) or /dev/ttyUSB0 (USB)."
    )
    try:
        ports = list(serial.tools.list_ports.comports())
        if not ports:
            logger.info("No serial ports found. For GPIO UART try: python3 controller.py /dev/ttyS0")
        else:
            logger.info("Available serial ports:")
            for p in ports:
                logger.info(f"  {p.device}  - {p.description or 'Unknown'}")
    except Exception:
        pass


class SerialController:
    def __init__(self, port, baud_rate, debug_serial=False):
        self.port = port
        self.baud_rate = baud_rate
        self.serial_conn = None
        self.running = False
        self.send_queue = queue.Queue()
        self.master_connected = False   # True after first HEARTBEAT/RX/MAC from Master
        self.last_activity = 0.0        # Time of last serial data from Master
        self.watchdog = None             # Set in start(); used for status
        self.debug_serial = bool(debug_serial)

    def _close_serial(self):
        try:
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.close()
        except Exception as e:
            logger.debug("Close serial: %s", e)
        self.serial_conn = None
        if getattr(self, "watchdog", None):
            self.watchdog.serial_conn = None

    def _reconnect_serial(self):
        self._close_serial()
        try:
            self.serial_conn = serial.Serial(self.port, self.baud_rate, timeout=1)
            _dtr_reset_master(self.serial_conn)
            if getattr(self, "watchdog", None):
                self.watchdog.serial_conn = self.serial_conn
            logger.info("Reconnected to %s at %s baud.", self.port, self.baud_rate)
            return True
        except serial.SerialException as e:
            logger.warning("Reconnect failed: %s", e)
            return False

    def connect(self):
        try:
            self.serial_conn = serial.Serial(self.port, self.baud_rate, timeout=1)
            _dtr_reset_master(self.serial_conn)
            logger.info("Connected to %s at %s baud.", self.port, self.baud_rate)
            return True
        except serial.SerialException as e:
            logger.error("Failed to connect to serial port: %s", e)
            _log_serial_port_help(self.port)
            return False

    def reader_thread(self):
        logger.info("Reader thread started.")
        while self.running:
            try:
                if not self.serial_conn or not self.serial_conn.is_open:
                    self._reconnect_serial()
                    if not self.serial_conn:
                        time.sleep(5)
                    continue
                if self.serial_conn.in_waiting > 0:
                    line = self.serial_conn.readline().decode("utf-8", errors="ignore").strip()
                    if line:
                        self.watchdog.pet()
                        self.last_activity = time.time()
                        self.process_incoming_data(line)
                else:
                    time.sleep(0.02)
            except (serial.SerialException, OSError) as e:
                logger.error("Serial error (will reconnect): %s", e)
                self._close_serial()
                time.sleep(2)
            except Exception as e:
                logger.error("Error reading from serial: %s", e)
                time.sleep(1) 

    def _mark_master_connected(self):
        if not self.master_connected:
            self.master_connected = True
            logger.info("Master connected.")

    def process_incoming_data(self, line):
        line = line.replace("\r", "").strip()
        if not line:
            return
        if self.debug_serial:
            logger.info("SERIAL <- %s", line[:80] + ("..." if len(line) > 80 else ""))

        # 1. Master boot line (current firmware sends this; older sends "MAC: XX:XX:...")
        if line.startswith("MAC:"):
            self._mark_master_connected()
            return
        if "Master Gateway" in line or line.strip().startswith("Master Gateway"):
            self._mark_master_connected()
            return

        # 2. Heartbeat
        if line == "HEARTBEAT":
            self._mark_master_connected()
            return

        # 3. Sensor Data: RX:<MAC>:<HEX_DATA> (allow spaces in hex like main app)
        match = re.search(r"RX:([0-9A-Fa-f:]+):([0-9A-Fa-f\s]+)", line)
        if match:
            self._mark_master_connected()
            mac = match.group(1)
            hex_data = match.group(2).replace(" ", "").replace("\r", "").strip()
            if not hex_data:
                return
            try:
                data_bytes = bytes.fromhex(hex_data)
                if len(data_bytes) == 6:
                    ctype, cmd, val = struct.unpack('<BBf', data_bytes)
                    logger.info(f"SENSOR [{mac}] -> Type:{ctype} Cmd:{cmd} Val:{val:.2f}")
                else:
                    # For unknown/other device types, just log the raw hex
                    logger.info(f"DATA [{mac}] -> RAW HEX: {hex_data}")
            except Exception as e:
                logger.error(f"Failed to decode data from {mac}: {e}")
            
        elif line.startswith("DEBUG:"):
            self._mark_master_connected()
            logger.debug("Master: %s", line)
        elif line.startswith("OK:") or line.startswith("ERR:"):
            self._mark_master_connected()
            logger.info("Master: %s", line)

    def send_command(self, mac_address, hex_data):
        command = f"TX:{mac_address}:{hex_data}\n"
        try:
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.write(command.encode("utf-8"))
                logger.info("SENT to %s: %s", mac_address, hex_data)
            else:
                logger.error("Serial connection lost. Cannot send.")
        except (serial.SerialException, OSError) as e:
            logger.error("Serial send failed: %s", e)
            self._close_serial()
        except Exception as e:
            logger.error("Error sending data: %s", e)

    def start(self):
        if not self.connect():
            return

        self.running = True
        
        # Start Watchdog (pass self so it can clear master_connected on timeout)
        self.watchdog = WatchdogThread(self.serial_conn, controller=self)
        self.watchdog.start()

        # Start Reader
        self.read_thread = threading.Thread(target=self.reader_thread)
        self.read_thread.daemon = True
        self.read_thread.start()
        
        self.ui_loop()

    def print_status(self):
        """Print Master connection status (connected/disconnected, last activity)."""
        if self.master_connected and self.last_activity:
            ago = time.time() - self.last_activity
            logger.info("Master: connected (last activity %.1fs ago)", ago)
        else:
            logger.info("Master: disconnected (waiting for HEARTBEAT)")
            logger.info(
                "Tip: Run with --debug to see if any serial data is received. "
                "GPIO UART: wire ESP32 UART0 (TX=GPIO1, RX=GPIO3) to Pi GPIO15(RX)/14(TX); use /dev/serial0 or /dev/ttyS0."
            )

    def ui_loop(self):
        print("\n--- House Automation Controller (Transparent Mode) ---")
        print("Format: <MAC_ADDRESS> <HEX_MESSAGE>")
        print("Commands: 'status' = show Master connection, 'exit' = quit")
        print("")
        print("Sample: send to RFID reader (replace with your reader MAC if different)")
        print("  START reader (1 byte 0x01):   F0:24:F9:0D:90:A4 01")
        print("  STOP reader (1 byte 0x00):    F0:24:F9:0D:90:A4 00")
        print("  (If your reader uses raw UHF frames: START = bb0027000322ffff4a7e, STOP = bb00280000287e)")
        print("  Slaves (e.g. slave_template) receive these and can send ACK back; you'll see RX:<MAC>:41434b... (ACK).")
        print("")
        print("Sample: send to sensor (6-byte type,cmd,value as BBf little-endian hex)")
        print("  e.g. type=1 cmd=1 value=25.0:  F0:24:F9:0D:92:D8 01010000c841")
        print("")
        print("Run with --debug to log every line from Master.\n")
        
        while self.running:
            try:
                user_input = input("Enter command: ").strip()
                if user_input.lower() == 'exit':
                    logger.info("Exiting...")
                    self.running = False
                    self.watchdog.stop()
                    break
                if user_input.lower() == 'status':
                    self.print_status()
                    continue

                parts = user_input.split(' ', 1)
                if len(parts) == 2:
                    mac = parts[0]
                    hex_val = parts[1].strip().replace('0x', '')
                    # Basic validation
                    if len(mac) == 17 and mac.count(':') == 5:
                        # Ensure hex_val is valid hex
                        if all(c in '0123456789ABCDEFabcdef' for c in hex_val) and len(hex_val) % 2 == 0:
                             self.send_command(mac, hex_val)
                        else:
                             logger.warning("Please provide a valid even-length HEX payload (e.g. 0102AABBCC)")
                    else:
                        logger.warning("Invalid MAC format. Use XX:XX:XX:XX:XX:XX")
                else:
                    logger.warning("Invalid Input. Format: <MAC> <HEX_MSG>")
            except KeyboardInterrupt:
                self.running = False
                self.watchdog.stop()
                break
            except Exception as e:
                logger.error("Input Error: %s", e)

        self._close_serial()

class WatchdogThread(threading.Thread):
    def __init__(self, serial_conn, timeout=60, controller=None):
        super().__init__()
        self.serial_conn = serial_conn
        self.timeout = timeout
        self.controller = controller
        self.last_pet = time.time()
        self.running = True
        self.daemon = True

    def pet(self):
        self.last_pet = time.time()

    def stop(self):
        self.running = False

    def run(self):
        logger.info("Watchdog started.")
        while self.running:
            time.sleep(1)
            if time.time() - self.last_pet > self.timeout:
                if self.controller:
                    self.controller.master_connected = False
                logger.critical(
                    "Master disconnected (no HEARTBEAT/data for %.0fs). Resetting Master via DTR...",
                    self.timeout,
                )
                self.reset_master()
                self.pet()

    def reset_master(self):
        if not self.serial_conn or not self.serial_conn.is_open:
            logger.warning("Cannot reset Master: serial not connected.")
            return
        try:
            self.serial_conn.dtr = False
            time.sleep(0.1)
            self.serial_conn.dtr = True
            time.sleep(0.1)
            self.serial_conn.dtr = False
            logger.info("Master Reset Signal Sent.")
        except Exception as e:
            logger.error("Failed to reset Master: %s", e)

if __name__ == "__main__":
    port = get_default_serial_port()
    debug_serial = False
    for arg in sys.argv[1:]:
        if arg == "--debug":
            debug_serial = True
        elif not arg.startswith("-"):
            port = arg
            break
    logger.info("Using serial port: %s", port)
    controller = SerialController(port, BAUD_RATE, debug_serial=debug_serial)
    controller.start()
