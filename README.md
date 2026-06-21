# bt_dos

## Overview

`bt_dos` is a command-line tool written in C++ (`bt_dos.cpp`).

## Features

* Feature 1
* Feature 2
* Feature 3

## Requirements

* Linux operating system
* Bluetooth adapter
* Required libraries and dependencies

## Compilation

```bash
g++ bt_dos.cpp 
```
## OutPut

bt_dos

## Installation

```bash
sudo pacman -S gcc
sudo apt install gcc
pkg 
```

## Usage

Options:
  -t <num>    Threads (default: 50)
  -s <bytes>  Packet size (default: 600)
  -r <num>    Rounds per second (default: 10)
  -m <mode>   Attack mode: ping | connect | hybrid (default: hybrid)
  -i <hciX>   Bluetooth interface (default: hci0)
  --burst <t> <s>  Burst mode: threads seconds
  --scan      Scan nearby devices first
  -h          Show this help


### Basic Usage

```bash
sudo ./bt_dos AA:BB:CC:DD:EE:FF
sudo  ./bt_dos --scan AA:BB:CC:DD:EE:Ff
```

### Advanced Usage

```bash
sudo  ./bt_dos AA:BB:CC:DD:EE:FF
sudo  ./bt_dos -t 100 -r 20 -m ping AA:BB:CC:DD:EE:FF
sudo  ./bt_dos --burst 200 15 AA:BB:CC:DD:EE:FF
sudo   ./bt_dos --scan AA:BB:CC:DD:EE:FF

```


## Notes

* Use only in authorized environments.
* Ensure all required dependencies are installed.
* Verify hardware compatibility before execution.


## Author

SIDDHATH
