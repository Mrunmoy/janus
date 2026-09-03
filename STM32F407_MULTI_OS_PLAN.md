# Master Implementation Plan: STM32F407 Multi-RTOS USB Mass Storage Showcase

> **Target Audience**: Claude Code / Autonomous Embedded Firmware Agent  
> **Repository Type**: Dedicated Hardware Demonstration Repository (consuming `janus`/`libcfuture` as a Git submodule)  
> **Target Hardware**: STM32F407ZGT6 Development Board (ARM Cortex-M4F @ 168 MHz, 1024 KB Flash, 192 KB SRAM + 64 KB CCM)  
> **Key Peripheral**: USB OTG FS Host connected to FAT32 USB Flash Drive  
> **Middleware**: ChaN's FatFS (R0.15+)  

---

## 1. Executive Summary & Objective

This repository proves the production readiness, zero-heap safety, and cross-OS portability of **`libcfuture`** on real silicon.

### Core Problem Demonstrated
A shared **Storage Servicer Task ($T_S$)** manages high-latency USB flash drive operations (FatFS sector writes, directory flushes, sector erases). Multiple client tasks dispatch concurrent file requests over an OS queue with deadlines:
1. **Scenario 1 (Happy Path)**: Fast telemetry log write completes within 100 ms deadline.
2. **Scenario 2 (Queue Timeout & Work Cancellation)**: Low-priority bulk log times out while waiting in the queue behind heavy I/O. $T_S$ pops the request, checks `cpromise_is_active()`, and **completely skips the expensive FatFS write**.
3. **Scenario 3 (Late Timeout Discard & Zero Dangling Pointers)**: Multi-block file write has a 25 ms deadline, but the USB flash write takes 65 ms. The caller times out and safely unwinds its stack frame. When $T_S$ completes at 65 ms, `cpromise_set_value()` safely detects the timeout and discards the payload without writing to the caller or corrupting memory.
4. **Scenario 4 (Queue ABA Isolation)**: Proves that while $T_A$'s request is queued or timing out, a concurrent task $T_B$ receives an isolated slot, preventing the Queue ABA / TOCTOU hazard.

### Multi-Branch Architecture
The repository maintains **one single identical application codebase** across 4 branches by abstracting the kernel via **OSAL** (Operating System Abstraction Layer) and hardware via **PAL** (Platform Abstraction Layer):
- `master`: Core application logic, OSAL/PAL interfaces, FatFS abstraction, and submodule setup.
- `freertos-stm32`: FreeRTOS 10.x + STM32 HAL USB Host MSC + ARM GCC build.
- `threadx-stm32`: Eclipse / Azure RTOS ThreadX + USB Host + ARM GCC build.
- `zephyr-stm32`: Zephyr RTOS 3.x + Native Zephyr USB Host MSC & Zephyr FatFS subsystem.
- `host-unix`: Desktop simulation (Linux/macOS) using POSIX threads and FatFS disk image / loopback block driver.

---

## 2. Hardware Specification & Pinout (STM32F407ZGT6)

| Pin | Function | Hardware Connection / Notes |
| :--- | :--- | :--- |
| **PA11** | `USB_OTG_FS_DM` | USB D- (Differential Data Minus) |
| **PA12** | `USB_OTG_FS_DP` | USB D+ (Differential Data Plus) |
| **PA9** | `USB_OTG_FS_VBUS` | 5V VBUS Sense (or jumpered to 5V power) |
| **PB15** (or board GPIO) | `USB_PWR_EN` | Active-Low/High USB Power Switch (enables 5V VBUS to flash drive) |
| **PA2** / **PA3** | `USART2_TX` / `USART2_RX` | Telemetry UART console (115200 baud, 8N1) for live log output |
| **LED0** / **LED1** | Status LEDs | Activity / Error heartbeat indicators |

---

## 3. Directory Layout (Identical Across Branches)

```
stm32f407-cfuture-storage/
├── .git/
├── .gitmodules                 # Submodule: external/cfuture -> https://github.com/Mrunmoy/janus.git
├── CMakeLists.txt              # Root build orchestrator (prints libcfuture footprint)
├── README.md                   # Hardware wiring, flashing, and test verification guide
├── external/
│   ├── cfuture/                # Git submodule (libcfuture C11 core library)
│   └── fatfs/                  # ChaN's FatFS (source files ff.c, ff.h, diskio.h)
├── include/
│   ├── app_config.h            # Buffer sizes, pool capacity, timeouts
│   ├── osal.h                  # OS Abstraction Layer (queues, events, tasks, delays)
│   ├── pal_usb.h               # Platform Abstraction for USB Host MSC
│   └── storage_service.h       # Shared Storage Task & Command Protocol
└── src/
    ├── main.c                  # Board init, task spawning, telemetry banner
    ├── storage_service.c       # Common Servicer Task (TS) implementation
    ├── client_tasks.c          # Common Requester Tasks (TA, TB, TC)
    ├── osal_impl.c             # [Branch-Specific] OSAL backend (FreeRTOS / ThreadX / Zephyr / POSIX)
    └── pal_usb_impl.c          # [Branch-Specific] Hardware USB Host Driver
```

---

## 4. Phase-by-Phase Implementation Roadmap

### Phase 1: Common Submodule Setup & Interfaces (`master` branch)

#### Task 1.1: Git Submodule Integration
- Initialize empty repository `stm32f407-cfuture-storage`.
- Add `janus` as a submodule at `external/cfuture`:
  ```bash
  git submodule add https://github.com/Mrunmoy/janus.git external/cfuture
  ```
- Verify `external/cfuture/include/cfuture.h` and `external/cfuture/src/cfuture.c` are accessible.

#### Task 1.2: Define OSAL Interface (`include/osal.h`)
Create a zero-overhead C abstraction interface for OS primitives:
- **Event / Sync Ops**: Returns `cfuture_sync_ops_t` table for the active RTOS.
- **Message Queue**:
  - `osal_queue_handle_t osal_queue_create(uint32_t len, uint32_t item_size);`
  - `bool osal_queue_send(osal_queue_handle_t q, const void *item, uint32_t timeout_ms);`
  - `bool osal_queue_receive(osal_queue_handle_t q, void *item, uint32_t timeout_ms);`
- **Task Spawning & Timing**:
  - `bool osal_task_create(const char *name, void (*entry)(void *), void *arg, uint32_t stack_depth, uint32_t priority);`
  - `void osal_delay_ms(uint32_t ms);`
  - `uint32_t osal_get_time_ms(void);`

#### Task 1.3: Define Storage Command Protocol (`include/storage_service.h`)
```c
typedef enum {
    STORAGE_OP_WRITE_SECTOR,
    STORAGE_OP_READ_SECTOR,
    STORAGE_OP_SYNC_FILE,
    STORAGE_OP_FORMAT
} storage_op_type_t;

typedef struct {
    storage_op_type_t op;
    char filename[32];
    uint32_t file_offset;
    const uint8_t *data;
    uint32_t data_len;
    cpromise_t promise;
} storage_request_t;

typedef struct {
    uint32_t bytes_transferred;
    int32_t error_code;
    uint32_t execution_time_ms;
} storage_response_t;
```

#### Task 1.4: Implement Common Servicer & Client Tasks (`src/storage_service.c`, `src/client_tasks.c`)
- **`storage_service.c`**:
  1. Pops `storage_request_t` from queue.
  2. **Cancellation Check**:
     ```c
     if (!cpromise_is_active(&req.promise)) {
         printf("[TS] Request for '%s' was CANCELLED by caller. Skipping FatFS write!\n", req.filename);
         cpromise_drop(&req.promise, 0);
         continue;
     }
     ```
  3. Invokes FatFS write (`f_write` / `f_sync`).
  4. Fulfills via `cpromise_set_value(&req.promise, &resp, 0)`.
- **`client_tasks.c`**:
  - `task_fast_telemetry`: Writes 64-byte logs every 500 ms (timeout 200 ms) -> Always succeeds.
  - `task_queued_timeout`: Enqueues heavy write behind busy storage with 10 ms timeout -> Times out while in queue, demonstrating work cancellation.
  - `task_late_timeout`: Enqueues 16 KB file write with 20 ms timeout (USB write takes 60 ms) -> Times out while writing, demonstrating safe unwinding and late discard.
  - `task_aba_verifier`: Arrives immediately after timeout to verify slot isolation.

---

### Phase 2: FreeRTOS + STM32 USB Host Branch (`freertos-stm32`)

#### Task 2.1: Branch Creation & FreeRTOS Kernel Integration
- Create branch: `git checkout -b freertos-stm32`.
- Integrate FreeRTOS Kernel (V10.4+):
  - `FreeRTOS/Source/tasks.c`, `queue.c`, `list.c`, `timers.c`, `event_groups.c`.
  - Portable layer: `ARM_CM4F/port.c` (hardware FPU support).
  - Memory manager: `heap_4.c` (or static allocation via `xTaskCreateStatic`).
  - Configure `FreeRTOSConfig.h` (`configUSE_PREEMPTION 1`, `configTICK_RATE_HZ 1000`, `configMAX_PRIORITIES 7`).

#### Task 2.2: FreeRTOS OSAL Backend (`src/osal_impl.c`)
- Implement `cfuture_sync_ops_t` using FreeRTOS EventGroups (`EventGroupHandle_t`).
  - `event_create`: `xEventGroupCreate()` (or `xEventGroupCreateStatic`).
  - `event_set`: `xEventGroupSetBits()`.
  - `event_set_from_isr`: `xEventGroupSetBitsFromISR()`.
  - `event_wait`: `xEventGroupWaitBits()` with millisecond tick conversion.
  - `event_destroy`: `vEventGroupDelete()`.
- Implement `osal_queue_*` using FreeRTOS `QueueHandle_t`.

#### Task 2.3: STM32 USB Host MSC & FatFS Integration (`src/pal_usb_impl.c`)
- Initialize STM32 USB OTG FS peripheral via STM32CubeF4 HAL (`USBH_Init`, `USBH_RegisterClass(&hUSBHost, USBH_MSC_CLASS)`, `USBH_Start`).
- Link FatFS disk I/O layer (`diskio.c`) to STM32 USB MSC host (`usbh_diskio.c`).
- In `USBH_UserProcess`: On `HOST_USER_CONNECTION`, mount FatFS drive (`f_mount(&fs, "", 1)`).

#### Task 2.4: Toolchain, CMake & Size Telemetry
- Configure `arm-none-eabi-gcc` CMake cross-compilation toolchain file.
- Add post-build custom target to print `libcfuture` memory footprint:
  ```cmake
  add_custom_command(TARGET storage_demo POST_BUILD
      COMMAND arm-none-eabi-size --format=berkeley $<TARGET_FILE:storage_demo>
      COMMAND ${CMAKE_COMMAND} -E echo "--- libcfuture static footprint ---"
      COMMAND arm-none-eabi-size external/cfuture/libcfuture.a
  )
  ```

---

### Phase 3: Eclipse / Azure RTOS ThreadX Branch (`threadx-stm32`)

#### Task 3.1: Branch Creation & ThreadX Kernel Integration
- Create branch: `git checkout -b threadx-stm32`.
- Add Eclipse ThreadX source files:
  - `common/src/tx_*.c`
  - Cortex-M4 port: `ports/cortex_m4/gnu/src/tx_*.S`

#### Task 3.2: ThreadX OSAL Backend (`src/osal_impl.c`)
- Implement `cfuture_sync_ops_t` using ThreadX Event Flags (`TX_EVENT_FLAGS_GROUP`):
  - `event_create`: `tx_event_flags_create()`.
  - `event_set`: `tx_event_flags_set(&group, 0x01, TX_OR)`.
  - `event_wait`: `tx_event_flags_get(&group, 0x01, TX_OR_CLEAR, &actual, ticks)`.
  - `event_destroy`: `tx_event_flags_delete()`.
- Implement `osal_queue_*` using `TX_QUEUE` (`tx_queue_create`, `tx_queue_send`, `tx_queue_receive`).

#### Task 3.3: Storage & FatFS Integration
- Connect ChaN's FatFS disk I/O or FileX to the STM32 USB MSC Host driver.
- Run identical `storage_service.c` and `client_tasks.c`.

---

### Phase 4: Zephyr RTOS Branch (`zephyr-stm32`)

#### Task 4.1: Branch Creation & Zephyr Environment Setup
- Create branch: `git checkout -b zephyr-stm32`.
- Create `prj.conf` enabling:
  ```ini
  CONFIG_USB_HOST=y
  CONFIG_USB_MASS_STORAGE=y
  CONFIG_FILE_SYSTEM=y
  CONFIG_FAT_FILESYSTEM_ELM=y
  CONFIG_LOG=y
  CONFIG_UART_CONSOLE=y
  ```
- Board target: `nucleo_f407zg` or `stm32f4_disco`.

#### Task 4.2: Zephyr OSAL Backend (`src/osal_impl.c`)
- Implement `cfuture_sync_ops_t` using Zephyr `k_event` kernel objects.
- Implement `osal_queue_*` using Zephyr `k_msgq`.
- Run identical common application files.

---

### Phase 5: Host Desktop Simulation (`host-unix`)

#### Task 5.1: Branch Creation & Host Driver
- Create branch: `git checkout -b host-unix`.
- Build natively with GCC / Clang on Linux or macOS.
- Implement OSAL using `cfuture_posix_sync_ops()` and POSIX pthreads/condition variables.
- Emulate FatFS block device:
  - Use a 64 MB RAM disk or file image (`fat32_usb.img`) via ChaN's `diskio.c` file-backed driver.
  - Allows full automated testing in CI/Nix without requiring physical STM32 hardware!

---

## 5. Verification & Acceptance Criteria for Claude

Before declaring any branch complete, verify:

1. **Clean Build**: Zero warnings under `-Wall -Wextra -Werror -pedantic`.
2. **Flash / RAM Footprint Display**:
   Build prints `.text`, `.data`, `.bss` size of `libcfuture.a` showing **0 bytes dynamic RAM** and **< 3 KB ROM**.
3. **Live UART Telemetry Output**:
   Flashing the STM32F407 with a USB flash drive connected outputs:
   ```
   ==============================================================
     libcfuture STM32F407 USB Storage Showcase [RTOS: FreeRTOS]
   ==============================================================
   [USB Host] Flash drive mounted: 32 GB FAT32 filesystem ready.
   
   --- Scenario 1: Fast Telemetry Write ---
   [T_fast] Dispatched Block 0 (Timeout: 200 ms)...
   [T_S] Processing Block 0... Written 64 bytes.
   [T_fast] SUCCESS: Result received in 18 ms!
   
   --- Scenario 2: Queue Timeout & Cancellation ---
   [T_S] Servicing heavy file operation...
   [T_queued] Dispatched Block 1 (Timeout: 10 ms) queued behind heavy I/O...
   [T_queued] TIMEOUT! Deadline expired. Stack frame unwound cleanly.
   [T_S] Popped Block 1 -> CANCELLATION DETECTED via cpromise_is_active()!
   [T_S] Skipped expensive FatFS write completely!
   
   --- Scenario 3: Late Timeout Discard ---
   [T_late] Dispatched Block 2 (Timeout: 25 ms, Write takes 65 ms)...
   [T_S] Started writing 16 KB to USB drive...
   [T_late] TIMEOUT! Deadline expired. Stack frame unwound cleanly.
   [T_S] Write completed at 65 ms. Invoked cpromise_set_value().
   [T_S] Late completion detected: Payload discarded safely without memory corruption!
   
   --- Scenario 4: Queue ABA Slot Isolation ---
   [T_aba] Claimed slot immediately after timeout.
   [T_aba] Assigned Slot 1 (Slot 0 remained locked until T_S finished).
   [T_aba] ABA Hazard PREVENTED: Zero slot collision!
   
   ==============================================================
     All Scenarios Verified: Residual Pool Mask = 0x0 (0 Leaks!)
   ==============================================================
   ```
4. **Git Branch Independence**: Each branch builds independently and cleanly checks out.
