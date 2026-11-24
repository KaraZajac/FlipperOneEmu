# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
# Flipper One machine: RP2350 and RK3576 models (FlipperOneEmu overlay)
arm_common_ss.add(when: 'CONFIG_FLIPPER_ONE', if_true: files(
  'flipper_one.c',
  'rp2350.c',
  'rp2350_coproc.c',
  'rp2350_sys.c',
  'rp2350_sio.c',
  'rp2350_qmi.c',
  'rp2350_timer.c',
))
