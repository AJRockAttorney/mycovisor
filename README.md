# mycovisor

Firmware and control software for an automated grow box for cultivating
gourmet mushrooms (oyster, lion's mane, shiitake, etc.), built around an
Arduino Uno Q.

## Objective

Monitor and control the environmental conditions mushroom fruiting depends
on — temperature, humidity, CO2, and fresh-air exchange — closing the loop
with sensors and actuators (heaters, humidifiers/misters, fans) so the box
can hold a target climate with minimal manual intervention.

The Uno Q's two processors split the work:

- **STM32U585 (MCU)** — real-time sensor reads and actuator control. Forked
  from [stm32u5-mpack-rpc](https://github.com/AJRockAttorney/stm32u5-mpack-rpc),
  which implements the RPC-over-UART transport this side talks to the Linux
  side over; that repo stays the place for transport-layer-only changes,
  while this one carries the grow-box-specific drivers and RPC methods.
- **QRB2210 (MPU, Linux)** — the non-realtime side: a web dashboard for
  monitoring/control, camera access for visual logging, and anything else
  that benefits from a full Linux environment rather than bare metal.

Project is a work in progress.
