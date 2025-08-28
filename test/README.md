# CAN Log Checker

This Python tool validates CAN bus logs generated with `'cangen can0 -D i -I i -L 4 -g 10'` and recorded in `candump`-style format:


## Features
- Parses `candump`-style log files.
- Starts from the first CAN ID found in the log.
- Verifies sequential CAN ID increments (with wrap at `0x7FF`).
- Detects missing frames and corrupted IDs.
- Prints per-line issues and a final summary with statistics.

## Examples of supported formats:
```
(1756343254.078218) can0 36B#6BA30700
(1756343254.078218) can 36B#6BA30700
(1756343254.078218) vcan 36B#6BA30700
```
## Usage
Run the checker on a log file:

```bash
python check_canlog.py <logfile>
