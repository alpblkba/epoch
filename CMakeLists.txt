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
  # This opt-in build requires the additional IPC development sources, which are not in the initial public source set.
  add_library(epoch_core STATIC core/src/msg.c core/src/journal.c core/src/controller.c core/src/host.c core/src/wire.c)
  target_link_libraries(epoch_core PUBLIC epoch_primitives)

  add_library(epoch_simlib STATIC host/simulator/sim.c)
  target_include_directories(epoch_simlib PUBLIC host/simulator)
  target_link_libraries(epoch_simlib PUBLIC epoch_core)

  add_executable(epoch_sim host/simulator/main.c)
  target_link_libraries(epoch_sim PRIVATE epoch_simlib)

  # Board client; needs a POSIX serial port, so it is built only where termios exists.
  if(APPLE OR UNIX)
    add_executable(epoch_client host/client/epoch_client.c)
    target_compile_definitions(epoch_client PRIVATE _DARWIN_C_SOURCE _DEFAULT_SOURCE)
    target_link_libraries(epoch_client PRIVATE epoch_core)
  endif()

  foreach(t journal state_machine fault_campaign wire)
    add_executable(test_${t} tests/${t}/test_${t}.c)
    target_include_directories(test_${t} PRIVATE tests)
    target_link_libraries(test_${t} PRIVATE epoch_simlib Threads::Threads)
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()
endif()
