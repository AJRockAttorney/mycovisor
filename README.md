# stm32u5-mpack-rpc

## Overview

This project is a bare metal rpc over uart implementation for the STM32U5 series. 

## Why I built this

The Arduino Uno Q is an interesting product because it combines a STM32U585 MCU with a Qualcomm QRB2210 MPU on the same 
board. This architecture lends itself well to tasks that require both computationally "heavy" tasks (heavy data processing, edge AI, service hosting, etc.) 
with a need for timing-critical I/O (Interfacing with sensors, controlling motors, etc.). Additionally, it is quite affordable. 
When originally launched, the Uno Q priced in at \$44USD for the 2gb version, recently increased to \$59USD. 

I was initially interested in this board with the idea of using it for my projects in which the MPU side would be free to run
a webserver acting as a user interface for my project, using any framework of my choosing (Django or Flask for Python, Node.js or 
Bun for JavaScript or TypeScript) and leaving the mcu to handle any timing critical aspectsion of my project. 

As someone who is trying to learn more about embedded systems development, the Arduino IDE hides too much complexity. As a result, I wanted to write 
myself a library to interface with the RPC protocol that Arduino ships on the MPU side, but using only the STM32 Low Level 
drivers. This allows me the full flexibility (and complexity) to write my own C code for the microcontroller without having to 
reimplement the RPC stack on the MPU side as well. 

![router-bridge-architecture.svg](/Images/router-bridge-architecture.svg)

## Architecture

## Wire Protocol

## Fault Handling

## Build & Flash

## Testing

## CI

## Known Issues / History

## TODO / Open Questions
