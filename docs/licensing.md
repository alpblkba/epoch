# Licensing scope

Original EPOCH software is available under Apache License 2.0. This includes the portable C11 sources, host-side tools, tests, build scripts, project documentation and the original embedded firmware written for Zephyr. The full text is in the repository's root [`LICENSE`](../LICENSE).

Original hardware design sources are distinct from firmware. Future EPOCH-authored RTL, schematics, PCB design files or similar hardware source will be released under CERN Open Hardware Licence Version 2 - Permissive (`CERN-OHL-P-2.0`) and explicitly identified as covered when those files are published. The initial public source set contains no such original hardware design files. The definitive [CERN licence text](https://ohwr.org/cern_ohl_p_v2.txt) is maintained by CERN; a verbatim copy should accompany a future hardware-source release.

Board support packages, the Zephyr RTOS, STM32Cube HAL/LL components, SDKs and other third-party material are not relicensed by EPOCH. Their original copyright notices and applicable license terms govern each redistributed component. The STM32 target build references an external Zephyr workspace rather than vendoring the STM32 HAL/BSP source tree.

The software and hardware scopes are separate: MCU firmware remains software under Apache-2.0, while EPOCH-authored hardware design source may use CERN-OHL-P-2.0. This description does not grant rights to third-party material beyond its existing licenses.
