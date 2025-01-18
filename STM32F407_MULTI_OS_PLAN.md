# Master Implementation Plan: Unified Multi-RTOS STM32F407 USB Storage Showcase

> **Target Audience**: Autonomous Embedded Firmware Developer / Engineer  
> **Repository Type**: Single-Branch Unified Multi-Target Hardware Showcase Repository  
> **Core Dependency**: `janus` (`libcfuture`) as a Git submodule at `external/cfuture`  
> **Target Hardware**: STM32F407ZGT6 Development Board (ARM Cortex-M4F @ 168 MHz) + USB OTG FS Host  
> **Middleware**: ChaN's FatFS (R0.15+)  
> **Build Orchestration**: Cross-platform `build.py` + CMake supporting `--os {host,freertos,threadx,zephyr}` and `--flash`  

---

## 1. Architecture Overview: Single-Branch Multi-Target Design

Instead of managing multiple git branches, this repository uses a **unified single-branch architecture**. All RTOS kernels live under `third_party/`, and the build system conditionally compiles the selected OSAL (Operating System Abstraction Layer) and PAL (Platform Abstraction Layer).

```
stm32f407/
├── CMakeLists.txt              # Root build orchestrator (selects OSAL/PAL via -DTARGET_OS=...)
├── build.py                    # Unified CLI script (--os, --build, --flash, --stats, --clean)
├── README.md                   # Hardware setup, wiring, flashing, and quickstart guide
├── external/
│   └── cfuture/                # Git submodule (libcfuture C11 core library)
├── third_party/
│   ├── fatfs/                  # ChaN's FatFS (ff.c, ff.h, diskio.h)
│   ├── stm32f4_hal/            # STM32F4 HAL + USB Host Core + USBH MSC Class
│   ├── freertos/               # FreeRTOS Kernel (Source/, portable/GCC/ARM_CM4F/)
│   └── threadx/                # Eclipse ThreadX (common/, ports/cortex_m4/gnu/)
├── include/
│   ├── app_config.h            # Buffer sizes, pool capacity, timeouts
│   ├── osal.h                  # Pure OS Abstraction (queues, events, tasks, delays)
│   ├── pal_storage.h           # Platform Abstraction for Block Storage / USB Host
│   └── storage_service.h       # Shared Storage Task & Command Protocol
└── src/
    ├── main.c                  # Unified main entry point (spawns tasks via OSAL)
    ├── storage_service.c       # Identical Storage Servicer Task (TS)
    ├── client_tasks.c          # Identical Requesters (Happy path, timeout, cancellation, ABA isolation)
    ├── osal/
    │   ├── osal_posix.c        # Host OSAL (pthreads, cond vars, cfuture_posix)
    │   ├── osal_freertos.c     # FreeRTOS OSAL (QueueHandle_t, EventGroupHandle_t)
    │   ├── osal_threadx.c      # ThreadX OSAL (TX_QUEUE, TX_EVENT_FLAGS_GROUP)
    │   └── osal_zephyr.c       # Zephyr OSAL (k_msgq, k_event)
    └── pal/
        ├── pal_host_disk.c     # Host RAM-disk / file-backed image driver for FatFS
        └── pal_stm32f4_usb.c   # Hardware STM32 USB Host MSC driver for FatFS
```

### Key Architectural Principle
**Zero application code differences between Host, FreeRTOS, ThreadX, and Zephyr.**
- `src/main.c`, `src/storage_service.c`, and `src/client_tasks.c` are 100% identical across all targets.
- The `TARGET_OS` CMake variable selects the corresponding `osal_<target>.c` and `pal_<target>.c`.

---

## 2. Supported Targets & Default Behavior

| Target OS | Platform / Hardware | Synchronization Primitive (`cfuture_sync_ops_t`) | Storage Driver | Default? |
| :--- | :--- | :--- | :--- | :---: |
| **`host`** | Linux / macOS / Windows | POSIX condition variables (`cfuture_posix`) / Win32 events | 64 MB RAM disk / image file | **YES (Default)** |
| **`freertos`** | STM32F407ZGT6 | FreeRTOS EventGroups (`EventGroupHandle_t`) | USB OTG FS Host MSC (`usbh_msc`) | No |
| **`threadx`** | STM32F407ZGT6 | ThreadX Event Flags (`TX_EVENT_FLAGS_GROUP`) | USB OTG FS Host MSC (`usbh_msc`) | No |
| **`zephyr`** | STM32F407ZGT6 | Zephyr kernel events (`k_event`) | Zephyr Native USB Host MSC | No |

---

## 3. Automation Script Specification (`build.py`)

A single cross-platform Python CLI (`build.py`) manages all targets and hardware flashing:

### Command-Line Interface
```bash
# Default build: Builds Host OS target (runs instantly on desktop)
python3 build.py

# Build specific OS target:
python3 build.py --os host --build
python3 build.py --os freertos --build
python3 build.py --os threadx --build
python3 build.py --os zephyr --build

# Run host simulation and test suite:
python3 build.py --os host --run

# Flash target binary to STM32F407 board via OpenOCD / ST-Link:
python3 build.py --os freertos --flash
python3 build.py --os threadx --flash

# Output binary footprint & libcfuture size breakdown:
python3 build.py --os freertos --stats

# Build all OS targets and print consolidated size comparison table:
python3 build.py --all

# Clean all build artifacts:
python3 build.py --clean
```

### CLI Implementation Structure
```python
def main():
    parser = argparse.ArgumentParser(description="STM32F407 Multi-RTOS Storage Showcase CLI")
    parser.add_argument("--os", choices=["host", "freertos", "threadx", "zephyr"], default="host")
    parser.add_argument("--build", action="store_true", help="Build selected OS target")
    parser.add_argument("--run", action="store_true", help="Run executable (host target)")
    parser.add_argument("--flash", action="store_true", help="Flash binary to STM32 board via OpenOCD/ST-Link")
    parser.add_argument("--stats", action="store_true", help="Print memory footprint and libcfuture size")
    parser.add_argument("--all", action="store_true", help="Build all OS targets and show comparison table")
    parser.add_argument("--clean", action="store_true", help="Clean build directories")
```

---

## 4. Hardware Specifications & STM32 Pinout

- **MCU**: STM32F407ZGT6 (168 MHz Cortex-M4F, 1024 KB Flash, 192 KB SRAM + 64 KB CCM).
- **USB OTG FS Host Pinout**:
  - `PA11`: `USB_OTG_FS_DM` (Data Minus).
  - `PA12`: `USB_OTG_FS_DP` (Data Plus).
  - `PA9`: `USB_OTG_FS_VBUS` (VBUS 5V Sense).
  - `PB15` (or board jumper): `USB_PWR_EN` (Active-low/high 5V switch powering the flash drive).
- **UART Telemetry**:
  - `USART2` (`PA2` TX, `PA3` RX) @ 115200 baud, 8N1 (connects to ST-Link VCP or USB-UART dongle).

---

## 5. Step-by-Step Task Breakdown

### Phase 1: Repository Skeleton & Common Abstractions
- **Task 1.1**: Initialize git repo and add `janus` submodule:
  ```bash
  git submodule add https://github.com/Mrunmoy/janus.git external/cfuture
  ```
- **Task 1.2**: Create `include/osal.h` defining:
  - `cfuture_sync_ops_t` getter for the active OS.
  - Generic message queue: `osal_queue_create`, `osal_queue_send`, `osal_queue_receive`.
  - Generic task creation: `osal_task_create`, `osal_delay_ms`, `osal_get_time_ms`.
- **Task 1.3**: Create `include/pal_storage.h` defining block storage initialization, read, write, and FatFS disk status hooks.
- **Task 1.4**: Integrate ChaN's FatFS under `third_party/fatfs/` (`ff.c`, `ff.h`, `diskio.h`, `ffconf.h`).
- **Task 1.5**: Implement common application files:
  - `src/storage_service.c`: Shared Servicer Task $T_S$ with `cpromise_is_active()` cancellation check.
  - `src/client_tasks.c`: Client Tasks executing the 4 test scenarios (Happy Path, Queue Timeout Cancellation, Late Completion Discard, Queue ABA Slot Isolation).
  - `src/main.c`: Initializes PAL, mounts FatFS volume, spawns tasks via OSAL, prints UART banner.

### Phase 2: Host OS Desktop Target (`host`)
- **Task 2.1**: Implement `src/osal/osal_posix.c` using standard pthreads, condition variables, and `external/cfuture/src/adapters/cfuture_posix.c`.
- **Task 2.2**: Implement `src/pal/pal_host_disk.c` using a 64 MB RAM-backed or image-backed FatFS block driver.
- **Task 2.3**: Configure CMake for Host target:
  - Default target: `add_executable(storage_demo_host ...)`
  - Run natively via `./build.py --os host --run` and verify all 4 scenarios pass on the host desktop without hardware!

### Phase 3: FreeRTOS + STM32 USB Host Target (`freertos`)
- **Task 3.1**: Add FreeRTOS kernel under `third_party/freertos/` and STM32 HAL under `third_party/stm32f4_hal/`.
- **Task 3.2**: Implement `src/osal/osal_freertos.c`:
  - `cfuture_sync_ops_t` backed by FreeRTOS `EventGroupHandle_t` (`xEventGroupCreate`, `xEventGroupSetBits`, `xEventGroupWaitBits`).
  - Queue backed by FreeRTOS `QueueHandle_t`.
- **Task 3.3**: Implement `src/pal/pal_stm32f4_usb.c`:
  - Configure STM32 USB OTG FS Host stack (`usbh_core.c`, `usbh_msc.c`).
  - Connect FatFS `diskio.c` to `USBH_MSC_Read` / `USBH_MSC_Write`.
- **Task 3.4**: Configure CMake cross-compilation with `arm-none-eabi-gcc`:
  - Target: `storage_demo_freertos.elf`.
  - Linker script for STM32F407ZGT6 (1024 KB Flash, 192 KB SRAM).
- **Task 3.5**: Implement `--flash` in `build.py`:
  - Uses OpenOCD (`openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program build_freertos/storage_demo_freertos.elf verify reset exit"`) or STM32_Programmer_CLI.

### Phase 4: Eclipse / Azure RTOS ThreadX Target (`threadx`)
- **Task 4.1**: Add ThreadX kernel under `third_party/threadx/` (`common/src/`, `ports/cortex_m4/gnu/`).
- **Task 4.2**: Implement `src/osal/osal_threadx.c`:
  - `cfuture_sync_ops_t` backed by ThreadX `TX_EVENT_FLAGS_GROUP` (`tx_event_flags_create`, `tx_event_flags_set`, `tx_event_flags_get`).
  - Queue backed by ThreadX `TX_QUEUE`.
- **Task 4.3**: Reuse `src/pal/pal_stm32f4_usb.c` and FatFS.
- **Task 4.4**: Configure CMake target `storage_demo_threadx.elf` and test flashing.

### Phase 5: Zephyr RTOS Target (`zephyr`)
- **Task 5.1**: Provide Zephyr application configuration (`prj.conf`, `app.overlay` for USB Host).
- **Task 5.2**: Implement `src/osal/osal_zephyr.c`:
  - `cfuture_sync_ops_t` backed by Zephyr `k_event`.
  - Queue backed by Zephyr `k_msgq`.
- **Task 5.3**: Build via `west build -b nucleo_f407zg` wrapped in `build.py --os zephyr --build`.

### Phase 6: Build Automation, Memory Footprint & Telemetry Output
- **Task 6.1**: Add CMake size extraction targets printing `.text`, `.data`, and `.bss` for both the application binary and `external/cfuture/libcfuture.a`.
- **Task 6.2**: Test `python3 build.py --all` to verify all 4 OS targets compile cleanly and output the comparative memory stats table.

---

## 6. Acceptance Criteria

1. **Host Builds & Runs Instantly Out-of-the-Box**:
   ```bash
   python3 build.py
   python3 build.py --run
   ```
   Must execute all 4 scenarios against the simulated FatFS drive with 0 errors.
2. **Single-Command Firmware Cross-Compilation**:
   ```bash
   python3 build.py --os freertos --build
   python3 build.py --os threadx --build
   ```
   Compiles ARM Cortex-M4 ELF binaries with zero warnings under `-Wall -Wextra -Werror`.
3. **Single-Command Flashing**:
   ```bash
   python3 build.py --os freertos --flash
   ```
   Flashes the board via ST-Link and begins outputting real-time telemetry over USART2.
4. **Live Verification on Physical USB Drive**:
   - Plug in any standard FAT32 USB drive into the STM32 board.
   - Live UART console shows:
     - USB drive mounted.
     - Scenario 1: Fast file write succeeds.
     - Scenario 2: Cancellation detected on queued timeout -> FatFS write skipped.
     - Scenario 3: Late timeout discard -> no dangling stack pointers or memory corruption.
     - Scenario 4: ABA isolation -> slot protected until servicer releases reference.
     - Residual pool mask = `0x0`.
