# Host-side tools

The initial public host tooling provides `host/client/uart_echo_test.py` for byte-exact validation of polling and interrupt-driven UART echo firmware. It includes nine deterministic binary vectors, an optional seeded streaming workload, SHA-256 comparisons and a measured mode that reports host-observed timing rather than physical UART line utilization.

Run the offline harness tests with `cd host/client && python3 -m unittest test_uart_echo`. Physical serial tests require pySerial, the correct virtual COM port and appropriate permissions; for example, `python3 host/client/uart_echo_test.py --port /dev/cu.usbmodem21303` from the repository root uses a port name specific to one macOS setup.

The session-aware host client, deterministic simulator and reset-campaign tooling are still experimental and are not included in this initial public source set.
