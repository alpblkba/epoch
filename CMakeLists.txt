cmake_minimum_required(VERSION 3.20)
project(epoch C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Empty, "address,undefined" or "thread".
set(EPOCH_SANITIZE "" CACHE STRING "Sanitizers passed to -fsanitize")
option(EPOCH_BUILD_EXPERIMENTAL "Build the provisional IPC core, simulator and host client" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
if(EPOCH_SANITIZE)
  add_compile_options(-fsanitize=${EPOCH_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${EPOCH_SANITIZE})
endif()

find_package(Threads REQUIRED)
enable_testing()

# The public build contains primitives that are also used by the embedded UART receive path.
add_library(epoch_primitives STATIC core/src/ring.c core/src/crc32.c)
target_include_directories(epoch_primitives PUBLIC core/include)

foreach(t ring crc32)
  add_executable(test_${t} tests/${t}/test_${t}.c)
  target_include_directories(test_${t} PRIVATE tests)
  target_link_libraries(test_${t} PRIVATE epoch_primitives Threads::Threads)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()

if(EPOCH_BUILD_EXPERIMENTAL)
  # Opt-in build of the provisional IPC core, simulator, board client and their tests.
  add_library(epoch_core STATIC core/src/msg.c core/src/journal.c core/src/controller.c core/src/host.c core/src/wire.c)
  target_link_libraries(epoch_core PUBLIC epoch_primitives)

  add_library(epoch_simlib STATIC host/simulator/sim.c)
  target_include_directories(epoch_simlib PUBLIC host/simulator)
  target_link_libraries(epoch_simlib PUBLIC epoch_core)

  add_executable(epoch_sim host/simulator/main.c)
  target_link_libraries(epoch_sim PRIVATE epoch_simlib)

  # Board client; needs a POSIX serial port, so it is built only where termios exists.
  if(APPLE OR UNIX)
    add_executable(epoch_client host/client/epoch_client.c host/client/trial_verdict.c)
    target_compile_definitions(epoch_client PRIVATE _DARWIN_C_SOURCE _DEFAULT_SOURCE)
    target_link_libraries(epoch_client PRIVATE epoch_core)
  endif()

  foreach(t journal journal_model state_machine ep1_model fault_campaign wire)
    add_executable(test_${t} tests/${t}/test_${t}.c)
    target_include_directories(test_${t} PRIVATE tests)
    target_link_libraries(test_${t} PRIVATE epoch_simlib Threads::Threads)
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()

  add_executable(test_trial_verdict tests/trial_verdict/test_trial_verdict.c host/client/trial_verdict.c)
  target_include_directories(test_trial_verdict PRIVATE tests host/client)
  target_link_libraries(test_trial_verdict PRIVATE epoch_core)
  add_test(NAME trial_verdict COMMAND test_trial_verdict)

  # The fault build's reset-injection hooks, compiled natively from the firmware source.
  add_executable(test_fault_hooks tests/fault_hooks/test_fault_hooks.c firmware/stm32u585/src/fault_hooks.c)
  target_include_directories(test_fault_hooks PRIVATE tests firmware/stm32u585/src)
  target_link_libraries(test_fault_hooks PRIVATE epoch_core)
  add_test(NAME fault_hooks COMMAND test_fault_hooks)
  # The same hooks as the corrupted-slot image (corrupt.conf) compiles them.
  add_executable(test_fault_hooks_corrupt tests/fault_hooks/test_fault_hooks.c firmware/stm32u585/src/fault_hooks.c)
  target_include_directories(test_fault_hooks_corrupt PRIVATE tests firmware/stm32u585/src)
  target_compile_definitions(test_fault_hooks_corrupt PRIVATE CONFIG_EPOCH_FAULT_CORRUPT)
  target_link_libraries(test_fault_hooks_corrupt PRIVATE epoch_core)
  add_test(NAME fault_hooks_corrupt COMMAND test_fault_hooks_corrupt)
endif()
