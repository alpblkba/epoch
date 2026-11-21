cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(epoch_uart_echo)

set(EPOCH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Polling UART echo by default; irq.conf selects the interrupt-driven echo, which links the portable EPOCH ring;
# retention.conf with boards/retention.overlay selects the retention probe, controller.conf with the same overlay the EPOCH controller,
# and fault.conf added to controller.conf the build with the test-only fault-injection endpoint.
if(CONFIG_EPOCH_RETENTION_PROBE)
  target_sources(app PRIVATE src/retention_probe.c ${EPOCH_ROOT}/core/src/crc32.c)
  target_include_directories(app PRIVATE ${EPOCH_ROOT}/core/include)
  target_compile_options(app PRIVATE -Wall -Wextra -Wshadow -Werror)
elseif(CONFIG_EPOCH_CONTROLLER_APP)
  set(EPOCH_CORE controller.c journal.c msg.c ring.c crc32.c wire.c)
  list(TRANSFORM EPOCH_CORE PREPEND ${EPOCH_ROOT}/core/src/)
  target_sources(app PRIVATE src/controller_app.c ${EPOCH_CORE})
  target_include_directories(app PRIVATE ${EPOCH_ROOT}/core/include)
  target_compile_options(app PRIVATE -Wall -Wextra -Wshadow -Werror)
  set_source_files_properties(${EPOCH_CORE} PROPERTIES COMPILE_OPTIONS "-Wpedantic;-Wconversion;-Wsign-conversion")
elseif(CONFIG_UART_INTERRUPT_DRIVEN)
  target_sources(app PRIVATE src/echo_irq.c ${EPOCH_ROOT}/core/src/ring.c)
  target_include_directories(app PRIVATE ${EPOCH_ROOT}/core/include)
  target_compile_options(app PRIVATE -Wall -Wextra -Wshadow -Werror)
  # Zephyr headers are not -Wconversion clean, so the native project's conversion warnings apply to the portable ring only.
  set_source_files_properties(${EPOCH_ROOT}/core/src/ring.c PROPERTIES COMPILE_OPTIONS "-Wpedantic;-Wconversion;-Wsign-conversion")
else()
  target_sources(app PRIVATE src/main.c)
endif()
