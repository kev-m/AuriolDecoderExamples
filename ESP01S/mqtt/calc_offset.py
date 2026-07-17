#!/usr/bin/env python3
"""Compute per-sensor scale and offset for the Auriol temperature correction.

The firmware applies:  T_corrected = scale * T_raw + offset

Give it two calibration points. At each point you have:
  - the KNOWN (reference) temperature, from a trusted thermometer
  - the SENSOR reading you want to correct

Usage:
    python calc_offset.py <known1> <known2> <sensor1> <sensor2>

Example:
    python calc_offset.py 10 24 9 28
      known pair  = 10, 24   (trusted thermometer)
      sensor pair =  9, 28   (the sensor to correct)

Use two points as far apart as you can (e.g. an ice-water bath at 0.0 and a
warm room) for the most accurate slope.
"""

import sys


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(1)

    try:
        known1, known2, sensor1, sensor2 = (float(a) for a in sys.argv[1:5])
    except ValueError:
        print("Error: all four arguments must be numbers.\n")
        print(__doc__)
        sys.exit(1)

    if sensor1 == sensor2:
        print("Error: the two sensor readings are identical; can't compute a slope.")
        sys.exit(1)

    scale = (known2 - known1) / (sensor2 - sensor1)
    offset = known1 - scale * sensor1

    print(f"scale  = {scale:.3f}")
    print(f"offset = {offset:.3f}")
    print()
    print("Enter these on the web page for the sensor.")
    print(f"Check: {sensor1:g} -> {scale * sensor1 + offset:.2f}, "
          f"{sensor2:g} -> {scale * sensor2 + offset:.2f} "
          f"(targets {known1:g}, {known2:g})")


if __name__ == "__main__":
    main()
