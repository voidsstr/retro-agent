3dfx Control Panel 2.0 on .124 (V5 6000, our stack), 2026-09-28 07:17-07:21

ctl1.jpg          Overview: card, 4 x VSA-100 / 64 MB (256 MB VBIOS mode), our
                  stack (vcr-kmd, h5 Glide, MesaFX 0.1.76), HP P1120 EDID limits,
                  the Glide key it writes. "boot not yet confirmed" = the vcrmp
                  BootAttempts counter a minute after a reboot (it went to 0,
                  GoodBoots 31, shortly after).
ctl_glide.jpg     3D & Glide tab, current values read from the stack.
ctl_display.jpg   THE BUG: Display & 2D showed "Pattern fills" / "Straight lines"
                  UNTICKED while the driver reported them ON (default-on rows
                  ticked only for choices[1], which is their OFF value "0";
                  ticking would have written 0).
ctl_display2.jpg  After the fix (ctl_check_index / ctl_value_matches): both ticked.
ctl_applied.jpg   Apply without a reboot: Mipmap dithering -> FX_GLIDE_LOD_DITHER=1
                  written and read back (REGREAD independently: absent -> "1");
                  then unticked and applied again -> absent. Panel closed clean.
