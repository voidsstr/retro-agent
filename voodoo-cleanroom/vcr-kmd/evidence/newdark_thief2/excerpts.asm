; Thief2.exe  md5 1109955d4d0a7855592b33d954abcb76

; ==== display_select: use_d3d_display -> D3D9 enumeration (0x601d60) or DX6  [005e93b0-005e9538]
005e93b0: sub      esp, 0x188
005e93b6: mov      eax, dword ptr [0x785400]
005e93bb: xor      eax, esp
005e93bd: mov      dword ptr [esp + 0x184], eax
005e93c4: mov      eax, dword ptr [0x78a404]
005e93c9: cmp      eax, -1
005e93cc: push     esi
005e93cd: jge      0x5e9522
005e93d3: lea      eax, [esp + 4]
005e93d7: push     eax
005e93d8: lea      ecx, [esp + 0xc]
005e93dc: push     ecx
005e93dd: mov      esi, 0x991a04
005e93e2: mov      dword ptr [esp + 0x10], 0x6fb99c  ; "use_d3d_display"
005e93ea: call     0x6a4ea0
005e93ef: add      esp, 8
005e93f2: test     al, al
005e93f4: je       0x5e945f
005e93f6: mov      eax, dword ptr [0x991a0c]
005e93fb: imul     eax, dword ptr [esp + 4]
005e9400: add      eax, dword ptr [0x991a20]
005e9406: je       0x5e945f
005e9408: call     0x688ce0
005e940d: test     eax, eax
005e940f: je       0x5e9444
005e9411: cmp      dword ptr [0x78a40c], 0
005e9418: jne      0x5e9429
005e941a: call     0x5e9540
005e941f: mov      dword ptr [0x78a40c], 1
005e9429: call     0x601d60
005e942e: pop      esi
005e942f: mov      ecx, dword ptr [esp + 0x184]
005e9436: xor      ecx, esp
005e9438: call     0x6b1d98
005e943d: add      esp, 0x188
005e9443: ret      
005e9444: mov      eax, dword ptr [0x78a404]
005e9449: pop      esi
005e944a: mov      ecx, dword ptr [esp + 0x184]
005e9451: xor      ecx, esp
005e9453: call     0x6b1d98
005e9458: add      esp, 0x188
005e945e: ret      
005e945f: cmp      dword ptr [0x78a40c], 1
005e9466: jne      0x5e9477
005e9468: call     0x5e9540
005e946d: mov      dword ptr [0x78a40c], 0
005e9477: call     0x67f4e0
005e947c: test     eax, eax
005e947e: jne      0x5e94a3
005e9480: mov      dword ptr [0x78a404], 0xffffffff
005e948a: or       eax, 0xffffffff
005e948d: pop      esi
005e948e: mov      ecx, dword ptr [esp + 0x184]
005e9495: xor      ecx, esp
005e9497: call     0x6b1d98
005e949c: add      esp, 0x188
005e94a2: ret      
005e94a3: push     0x72e8cc  ; "Enumerating DX6 adapters..."
005e94a8: mov      dword ptr [esp + 0x24], 0
005e94b0: mov      dword ptr [esp + 0x17e], 0
005e94bb: call     0x659c90
005e94c0: call     0x659cd0
005e94c5: cmp      dword ptr [0x7e01dc], 0
005e94cc: jne      0x5e94d8
005e94ce: mov      esi, 0x7e01dc
005e94d3: call     0x6604e0
005e94d8: lea      edx, [esp + 0x1c]
005e94dc: push     edx
005e94dd: push     0x5e9260
005e94e2: call     dword ptr [0x7e01dc]
005e94e8: call     0x659d10
005e94ed: push     0x72e834  ; "Enumeration done"
005e94f2: call     0x659c90
005e94f7: mov      eax, dword ptr [esp + 0x20]
005e94fb: test     eax, eax
005e94fd: jne      0x5e950d
005e94ff: push     0x72e8e8  ; "** failed to find any supported hardware 3D devices, make sure hardware acceleration is en"
005e9504: call     0x659c90
005e9509: mov      eax, dword ptr [esp + 0x20]
005e950d: cmp      dword ptr [0x78a408], 0
005e9514: je       0x5e951d
005e9516: test     eax, eax
005e9518: jne      0x5e951d
005e951a: or       eax, 0xffffffff
005e951d: mov      dword ptr [0x78a404], eax
005e9522: mov      ecx, dword ptr [esp + 0x188]
005e9529: pop      esi
005e952a: xor      ecx, esp
005e952c: call     0x6b1d98
005e9531: add      esp, 0x188
005e9537: ret      

; ==== dx6_device_validation: what the legacy DX6 display asks of a HAL device  [005e8be2-005e8e53]
005e8be2: mov      esi, dword ptr [esp + 0x28]
005e8be6: mov      eax, dword ptr [esi + 8]
005e8be9: xor      ebp, ebp
005e8beb: cmp      eax, ebp
005e8bed: jne      0x5e8c59
005e8bef: cmp      dword ptr [esp + 0x14], ebp
005e8bf3: je       0x5e8fa2
005e8bf9: call     0x659cd0
005e8bfe: call     0x659cd0
005e8c03: push     0x72e634  ; "** failed, no HW support"
005e8c08: call     0x659c90
005e8c0d: push     0x9919d0
005e8c12: call     edi
005e8c14: mov      esi, 1
005e8c19: sub      dword ptr [0x9db364], esi
005e8c1f: jns      0x5e8c27
005e8c21: mov      dword ptr [0x9db364], ebp
005e8c27: push     0x9919d0
005e8c2c: call     ebx
005e8c2e: push     0x9919d0
005e8c33: call     edi
005e8c35: sub      dword ptr [0x9db364], esi
005e8c3b: jns      0x5e8c43
005e8c3d: mov      dword ptr [0x9db364], ebp
005e8c43: push     0x9919d0
005e8c48: call     ebx
005e8c4a: pop      edi
005e8c4b: pop      esi
005e8c4c: pop      ebp
005e8c4d: mov      eax, 1
005e8c52: pop      ebx
005e8c53: add      esp, 8
005e8c56: ret      0x18
005e8c59: test     dword ptr [esi + 0x9c], 0x500
005e8c63: jne      0x5e8c83
005e8c65: cmp      dword ptr [esp + 0x14], ebp
005e8c69: je       0x5e8fa2
005e8c6f: call     0x659cd0
005e8c74: call     0x659cd0
005e8c79: push     0x72e650  ; "** failed, no 16-bit/32-bit support"
005e8c7e: jmp      0x5e8f93
005e8c83: mov      edx, 2
005e8c88: test     dl, al
005e8c8a: jne      0x5e8caa
005e8c8c: cmp      dword ptr [esp + 0x14], ebp
005e8c90: je       0x5e8fa2
005e8c96: call     0x659cd0
005e8c9b: call     0x659cd0
005e8ca0: push     0x72e674  ; "** failed, no RGB support"
005e8ca5: jmp      0x5e8f93
005e8caa: mov      ecx, dword ptr [esi + 0x80]
005e8cb0: test     cl, 8
005e8cb3: jne      0x5e8cd3
005e8cb5: cmp      dword ptr [esp + 0x14], ebp
005e8cb9: je       0x5e8fa2
005e8cbf: call     0x659cd0
005e8cc4: call     0x659cd0
005e8cc9: push     0x72e690  ; "** failed color gouraud shading"
005e8cce: jmp      0x5e8f93
005e8cd3: test     byte ptr [esi + 0x84], 4
005e8cda: jne      0x5e8cfa
005e8cdc: cmp      dword ptr [esp + 0x14], ebp
005e8ce0: je       0x5e8fa2
005e8ce6: call     0x659cd0
005e8ceb: call     0x659cd0
005e8cf0: push     0x72e6b0  ; "** failed alpha blending"
005e8cf5: jmp      0x5e8f93
005e8cfa: test     dword ptr [esi + 0xc], 0x1300
005e8d01: jne      0x5e8d21
005e8d03: cmp      dword ptr [esp + 0x14], ebp
005e8d07: je       0x5e8fa2
005e8d0d: call     0x659cd0
005e8d12: call     0x659cd0
005e8d17: push     0x72e6cc  ; "** failed texture mapping"
005e8d1c: jmp      0x5e8f93
005e8d21: mov      eax, dword ptr [esi + 0x6c]
005e8d24: test     eax, 0x8000
005e8d29: je       0x5e8d33
005e8d2b: and      eax, 0xfffffeff
005e8d30: mov      dword ptr [esi + 0x6c], eax
005e8d33: mov      eax, dword ptr [esi + 0x6c]
005e8d36: mov      ebx, dword ptr [esp + 0x30]
005e8d3a: test     eax, 0x100
005e8d3f: je       0x5e8dbf
005e8d41: mov      dword ptr [esp + 0x10], 0x80000
005e8d49: test     al, al
005e8d4b: jns      0x5e8de6
005e8d51: or       dword ptr [esp + 0x10], 0x100000
005e8d59: test     ecx, 0x4000
005e8d5f: je       0x5e8d69
005e8d61: or       dword ptr [esp + 0x10], 0x8000000
005e8d69: cmp      dword ptr [0x94e108], ebp
005e8d6f: je       0x5e8d88
005e8d71: or       dword ptr [esp + 0x10], 0x10000
005e8d79: test     eax, 0x40000
005e8d7e: je       0x5e8d88
005e8d80: or       dword ptr [esp + 0x10], 0x40000
005e8d88: cmp      dword ptr [0x94e104], ebp
005e8d8e: je       0x5e8d98
005e8d90: or       dword ptr [esp + 0x10], 0x4000000
005e8d98: cmp      word ptr [esi + 0xf8], dx
005e8d9f: jb       0x5e8e11
005e8da1: cmp      word ptr [esi + 0xfa], dx
005e8da8: jb       0x5e8e11
005e8daa: movzx    eax, word ptr [esi + 0xf0]
005e8db1: cmp      eax, edx
005e8db3: jb       0x5e8e11
005e8db5: or       dword ptr [esp + 0x10], 0x2000000
005e8dbd: jmp      0x5e8e35
005e8dbf: test     byte ptr [ebx + 0x15e], 0x10
005e8dc6: je       0x5e8d49
005e8dc8: cmp      dword ptr [esp + 0x14], ebp
005e8dcc: je       0x5e8fa2
005e8dd2: call     0x659cd0
005e8dd7: call     0x659cd0
005e8ddc: push     0x72e6e8  ; "** failed table fog"
005e8de1: jmp      0x5e8f93
005e8de6: test     byte ptr [ebx + 0x15e], 0x20
005e8ded: je       0x5e8d59
005e8df3: cmp      dword ptr [esp + 0x14], ebp
005e8df7: je       0x5e8fa2
005e8dfd: call     0x659cd0
005e8e02: call     0x659cd0
005e8e07: push     0x72e6fc  ; "** failed vertex fog"
005e8e0c: jmp      0x5e8f93
005e8e11: cmp      dword ptr [esp + 0x14], ebp
005e8e15: je       0x5e8e35
005e8e17: call     0x659cd0
005e8e1c: call     0x659cd0
005e8e21: push     0x72e714  ; "no multi-texture"
005e8e26: call     0x659c90
005e8e2b: call     0x659d10
005e8e30: call     0x659d10
005e8e35: mov      eax, dword ptr [esi + 0x6c]
005e8e38: test     al, 1
005e8e3a: je       0x5e8e44
005e8e3c: or       dword ptr [esp + 0x10], 0x200000
005e8e44: test     eax, 0x800
005e8e49: je       0x5e8e53
005e8e4b: or       dword ptr [esp + 0x10], 0x400000

; ==== scrn_loop: requested mode, fallback mode, then the dialog  [005c87c9-005c8970]
005c87c9: mov      eax, dword ptr [esi + 8]
005c87cc: mov      ecx, dword ptr [esi]
005c87ce: push     eax
005c87cf: mov      eax, dword ptr [esi + 4]
005c87d2: push     ecx
005c87d3: call     0x5ca0d0
005c87d8: add      esp, 8
005c87db: neg      eax
005c87dd: sbb      eax, eax
005c87df: inc      eax
005c87e0: xor      ebx, ebx
005c87e2: mov      dword ptr [0x9d7530], eax
005c87e7: cmp      eax, ebx
005c87e9: je       0x5c896f
005c87ef: xor      eax, eax
005c87f1: mov      dword ptr [esp + 0x28], eax
005c87f5: mov      dword ptr [esp + 0x2c], eax
005c87f9: mov      dword ptr [esp + 0x30], eax
005c87fd: mov      dword ptr [esp + 0x34], eax
005c8801: push     7
005c8803: mov      edx, 0x78c934
005c8808: lea      eax, [esp + 0x28]
005c880c: mov      dword ptr [esp + 0x28], ebx
005c8810: call     0x5ca520
005c8815: mov      edx, dword ptr [esi]
005c8817: push     6
005c8819: call     0x5ca520
005c881e: mov      edx, dword ptr [0x7d2cb4]
005c8824: mov      eax, dword ptr [0x7d2cb8]
005c8829: mov      ecx, dword ptr [0x7d2cbc]
005c882f: mov      dword ptr [esp + 0x18], edx
005c8833: mov      edx, dword ptr [0x7d2cc0]
005c8839: mov      dword ptr [esp + 0x24], edx
005c883d: mov      edx, dword ptr [esp + 0x34]
005c8841: mov      dword ptr [esp + 0x1c], eax
005c8845: mov      eax, dword ptr [0x7d2cc4]
005c884a: mov      dword ptr [esp + 0x20], ecx
005c884e: mov      ecx, dword ptr [esp + 0x30]
005c8852: mov      dword ptr [esp + 0x20], edx
005c8856: lea      edx, [esp + 0x18]
005c885a: mov      dword ptr [esp + 0x28], eax
005c885e: mov      dword ptr [esp + 0x1c], ecx
005c8862: call     0x4262b0
005c8867: mov      eax, dword ptr [esi + 8]
005c886a: push     eax
005c886b: mov      eax, dword ptr [esi + 4]
005c886e: lea      ecx, [esp + 0x30]
005c8872: mov      ebp, 1
005c8877: push     ecx
005c8878: mov      dword ptr [0x9d7538], ebp
005c887e: call     0x5ca0d0
005c8883: add      esp, 0x10
005c8886: test     eax, eax
005c8888: jne      0x5c896f
005c888e: mov      edx, dword ptr [0x7d2cb4]
005c8894: mov      eax, dword ptr [0x7d2cb8]
005c8899: mov      ecx, dword ptr [0x7d2cbc]
005c889f: mov      dword ptr [esp + 0x10], edx
005c88a3: mov      edx, dword ptr [0x7d2cc0]
005c88a9: mov      dword ptr [esp + 0x1c], edx
005c88ad: mov      edx, dword ptr [0x78c93c]
005c88b3: mov      dword ptr [esp + 0x14], eax
005c88b7: mov      eax, dword ptr [0x7d2cc4]
005c88bc: mov      dword ptr [esp + 0x18], ecx
005c88c0: mov      ecx, dword ptr [0x78c938]
005c88c6: mov      dword ptr [esp + 0x18], edx
005c88ca: lea      edx, [esp + 0x10]
005c88ce: mov      dword ptr [esp + 0x20], eax
005c88d2: mov      dword ptr [esp + 0x14], ecx
005c88d6: call     0x4262b0
005c88db: push     ebx
005c88dc: push     0x78c934
005c88e1: xor      eax, eax
005c88e3: mov      dword ptr [0x9d7538], ebp
005c88e9: call     0x5ca0d0
005c88ee: mov      edi, eax
005c88f0: add      esp, 8
005c88f3: cmp      edi, ebx
005c88f5: jne      0x5c896f
005c88f7: push     ebp
005c88f8: mov      eax, 0x78c934
005c88fd: call     0x5c8660
005c8902: add      esp, 4
005c8905: test     eax, eax
005c8907: je       0x5c891b
005c8909: push     ebx
005c890a: push     0x78c934
005c890f: xor      eax, eax
005c8911: call     0x5ca0d0
005c8916: add      esp, 8
005c8919: mov      edi, eax
005c891b: cmp      edi, ebx
005c891d: jne      0x5c896f
005c891f: mov      ecx, 0x12
005c8924: mov      esi, 0x72ce18  ; "Your video hardware is not supported or no supported video mode was found"
005c8929: lea      edi, [esp + 0x38]
005c892d: rep movsd dword ptr es:[edi], dword ptr [esi]
005c892f: xor      eax, eax
005c8931: movsw    word ptr es:[edi], word ptr [esi]
005c8933: mov      dword ptr [esp + 0x82], eax
005c893a: mov      word ptr [esp + 0x86], ax
005c8942: lea      eax, [esp + 0x38]
005c8946: push     eax
005c8947: mov      edi, 0x50
005c894c: mov      eax, 0x72ce64  ; "scrn_loop_fail_msg"
005c8951: call     0x664a60
005c8956: add      esp, 4
005c8959: push     0x30
005c895b: push     ebx
005c895c: lea      ecx, [esp + 0x40]
005c8960: push     ecx
005c8961: push     ebx
005c8962: call     dword ptr [0x6dd578]  ; "n98"
005c8968: push     ebx
005c8969: call     dword ptr [0x6dd3f4]

; ==== d3d9_device_validation (D3DCAPS9 in esi)  [00601150-006012e4]
00601150: push     ecx
00601151: push     ebx
00601152: mov      ebx, dword ptr [esp + 0xc]
00601156: push     esi
00601157: mov      esi, eax
00601159: mov      ecx, dword ptr [esi + 0x38]
0060115c: mov      dword ptr [esp + 8], 0
00601164: test     cl, 8
00601167: jne      0x6011a1
00601169: cmp      dword ptr [0x9db360], 0
00601170: je       0x6013f9
00601176: push     0x9919d0
0060117b: call     dword ptr [0x6dd0bc]  ; "868"
00601181: push     0
00601183: push     0
00601185: push     0x72e690  ; "** failed color gouraud shading"
0060118a: call     dword ptr [0x9db360]
00601190: push     0x9919d0
00601195: call     dword ptr [0x6dd0b4]
0060119b: pop      esi
0060119c: xor      eax, eax
0060119e: pop      ebx
0060119f: pop      ecx
006011a0: ret      
006011a1: test     byte ptr [esi + 0x3c], 4
006011a5: jne      0x6011b7
006011a7: push     0x72e6b0  ; "** failed alpha blending"
006011ac: call     0x659c90
006011b1: pop      esi
006011b2: xor      eax, eax
006011b4: pop      ebx
006011b5: pop      ecx
006011b6: ret      
006011b7: test     dword ptr [esi + 0x1c], 0x300
006011be: jne      0x6011d0
006011c0: push     0x72f5c0  ; "** failed texture wrapping"
006011c5: call     0x659c90
006011ca: pop      esi
006011cb: xor      eax, eax
006011cd: pop      ebx
006011ce: pop      ecx
006011cf: ret      
006011d0: mov      eax, dword ptr [esi + 0x24]
006011d3: test     eax, 0x8000
006011d8: je       0x6011e2
006011da: and      eax, 0xfffffeff
006011df: mov      dword ptr [esi + 0x24], eax
006011e2: mov      eax, dword ptr [esi + 0x24]
006011e5: test     eax, 0x100
006011ea: je       0x601273
006011f0: mov      dword ptr [esp + 8], 0x80000
006011f8: test     al, al
006011fa: jns      0x601293
00601200: or       dword ptr [esp + 8], 0x100000
00601208: test     ecx, 0x4000
0060120e: je       0x601218
00601210: or       dword ptr [esp + 8], 0x8000000
00601218: cmp      dword ptr [0x98f250], 0
0060121f: je       0x601238
00601221: or       dword ptr [esp + 8], 0x10000
00601229: test     eax, 0x40000
0060122e: je       0x601238
00601230: or       dword ptr [esp + 8], 0x40000
00601238: cmp      dword ptr [0x98f24c], 0
0060123f: je       0x601249
00601241: or       dword ptr [esp + 8], 0x4000000
00601249: mov      eax, 2
0060124e: cmp      dword ptr [esi + 0x94], eax
00601254: jb       0x6012b3
00601256: cmp      dword ptr [esi + 0x98], eax
0060125c: jb       0x6012b3
0060125e: movzx    ecx, word ptr [esi + 0x8c]
00601265: cmp      ecx, eax
00601267: jb       0x6012b3
00601269: or       dword ptr [esp + 8], 0x2000000
00601271: jmp      0x6012bd
00601273: test     byte ptr [ebx + 0x15e], 0x10
0060127a: je       0x6011f8
00601280: push     0x72e6e8  ; "** failed table fog"
00601285: call     0x659c90
0060128a: pop      esi
0060128b: mov      eax, 1
00601290: pop      ebx
00601291: pop      ecx
00601292: ret      
00601293: test     byte ptr [ebx + 0x15e], 0x20
0060129a: je       0x601208
006012a0: push     0x72e6fc  ; "** failed vertex fog"
006012a5: call     0x659c90
006012aa: pop      esi
006012ab: mov      eax, 1
006012b0: pop      ebx
006012b1: pop      ecx
006012b2: ret      
006012b3: push     0x72e714  ; "no multi-texture"
006012b8: call     0x659c90
006012bd: test     byte ptr [esi + 0x24], 1
006012c1: je       0x6012cb
006012c3: or       dword ptr [esp + 8], 0x200000
006012cb: cmp      dword ptr [ebx + 4], 0xa
006012cf: jl       0x6012e4
006012d1: push     0x72e728  ; "** too many devices enumerated, can't add this"
006012d6: call     0x659c90
006012db: pop      esi
006012dc: mov      eax, 1
006012e1: pop      ebx
006012e2: pop      ecx
006012e3: ret      

; ==== d3d9_enum_adapter: GetDeviceCaps, modes of R5G6B5 + X8R8G8B8, validation  [006014c0-00601855]
006014c0: mov      edx, dword ptr [esp + 8]
006014c4: sub      esp, 0x150
006014ca: push     ebx
006014cb: push     ebp
006014cc: push     esi
006014cd: push     edi
006014ce: lea      ecx, [esp + 0x30]
006014d2: push     ecx
006014d3: mov      esi, eax
006014d5: mov      eax, dword ptr [esi]
006014d7: mov      eax, dword ptr [eax + 0x38]
006014da: push     1
006014dc: push     edx
006014dd: push     esi
006014de: call     eax
006014e0: mov      edi, eax
006014e2: xor      ebp, ebp
006014e4: cmp      edi, ebp
006014e6: jge      0x6015aa
006014ec: mov      esi, dword ptr [0x6dd0bc]  ; "868"
006014f2: mov      ebx, dword ptr [0x6dd0b4]
006014f8: cmp      dword ptr [0x9db360], ebp
006014fe: je       0x60151b
00601500: push     0x9919d0
00601505: call     esi
00601507: push     ebp
00601508: push     ebp
00601509: push     0x72e620  ; "Device Validation"
0060150e: call     dword ptr [0x9db360]
00601514: push     0x9919d0
00601519: call     ebx
0060151b: push     0x9919d0
00601520: call     esi
00601522: mov      eax, dword ptr [0x9db364]
00601527: inc      eax
00601528: cmp      eax, 0xfa
0060152d: mov      dword ptr [0x9db364], eax
00601532: jle      0x60153e
00601534: mov      dword ptr [0x9db364], 0xfa
0060153e: push     0x9919d0
00601543: call     ebx
00601545: cmp      edi, 0x8876086a
0060154b: jne      0x601554
0060154d: push     0x72f5e0  ; "** failed, no hardware 3D support, make sure hardware acceleration is enabled in the drive"
00601552: jmp      0x601579
00601554: cmp      edi, 0x8876017c
0060155a: jne      0x601563
0060155c: push     0x72f640  ; "** failed, device out of video memory"
00601561: jmp      0x601579
00601563: mov      eax, edi
00601565: call     0x679d10
0060156a: push     eax
0060156b: push     0x72f668  ; "** failed, could not query device caps (%s)"
00601570: call     0x6599d0
00601575: add      esp, 8
00601578: push     eax
00601579: call     0x659c90
0060157e: push     0x9919d0
00601583: call     esi
00601585: sub      dword ptr [0x9db364], 1
0060158c: jns      0x601843
00601592: push     0x9919d0
00601597: mov      dword ptr [0x9db364], ebp
0060159d: call     ebx
0060159f: pop      edi
006015a0: pop      esi
006015a1: pop      ebp
006015a2: pop      ebx
006015a3: add      esp, 0x150
006015a9: ret      
006015aa: mov      edx, dword ptr [esp + 0x164]
006015b1: xor      ecx, ecx
006015b3: cmp      dword ptr [esp + 0x58], ebp
006015b7: mov      dword ptr [edx + 0x15a], ebp
006015bd: setne    cl
006015c0: mov      dword ptr [0x98f24c], 1
006015ca: mov      dword ptr [0x789cc4], ebp
006015d0: mov      dword ptr [0x98f254], 0xffffffff
006015da: mov      dword ptr [esp + 0x18], 0x17
006015e2: mov      dword ptr [esp + 0x1c], 0x16
006015ea: mov      dword ptr [0x98f250], ecx
006015f0: cmp      dword ptr [0x9db360], ebp
006015f6: je       0x60161b
006015f8: push     0x9919d0
006015fd: call     dword ptr [0x6dd0bc]  ; "868"
00601603: push     ebp
00601604: push     ebp
00601605: push     0x72e810  ; "Modes"
0060160a: call     dword ptr [0x9db360]
00601610: push     0x9919d0
00601615: call     dword ptr [0x6dd0b4]
0060161b: push     0x9919d0
00601620: call     dword ptr [0x6dd0bc]  ; "868"
00601626: mov      eax, dword ptr [0x9db364]
0060162b: inc      eax
0060162c: cmp      eax, 0xfa
00601631: mov      dword ptr [0x9db364], eax
00601636: jle      0x601642
00601638: mov      dword ptr [0x9db364], 0xfa
00601642: push     0x9919d0
00601647: call     dword ptr [0x6dd0b4]
0060164d: mov      dword ptr [esp + 0x10], ebp
00601651: mov      eax, dword ptr [esp + 0x10]
00601655: mov      ebp, dword ptr [esp + eax*4 + 0x18]
00601659: mov      edx, dword ptr [esp + 0x168]
00601660: mov      ecx, dword ptr [esi]
00601662: mov      eax, dword ptr [ecx + 0x18]
00601665: push     ebp
00601666: push     edx
00601667: push     esi
00601668: call     eax
0060166a: mov      edi, eax
0060166c: xor      ebx, ebx
0060166e: mov      dword ptr [esp + 0x14], edi
00601672: test     edi, edi
00601674: jbe      0x6016b4
00601676: jmp      0x601680
00601678: lea      esp, [esp]
0060167f: nop      
00601680: mov      eax, dword ptr [esp + 0x168]
00601687: mov      ecx, dword ptr [esi]
00601689: mov      ecx, dword ptr [ecx + 0x1c]
0060168c: lea      edx, [esp + 0x20]
00601690: push     edx
00601691: push     ebx
00601692: push     ebp
00601693: push     eax
00601694: push     esi
00601695: call     ecx
00601697: test     eax, eax
00601699: jl       0x6016af
0060169b: mov      eax, dword ptr [esp + 0x164]
006016a2: lea      edi, [esp + 0x20]
006016a6: call     0x601400
006016ab: mov      edi, dword ptr [esp + 0x14]
006016af: inc      ebx
006016b0: cmp      ebx, edi
006016b2: jb       0x601680
006016b4: mov      eax, dword ptr [esp + 0x10]
006016b8: inc      eax
006016b9: mov      dword ptr [esp + 0x10], eax
006016bd: cmp      eax, 2
006016c0: jb       0x601651
006016c2: mov      esi, dword ptr [0x6dd0bc]  ; "868"
006016c8: push     0x9919d0
006016cd: call     esi
006016cf: mov      ebp, 1
006016d4: sub      dword ptr [0x9db364], ebp
006016da: jns      0x6016e6
006016dc: mov      dword ptr [0x9db364], 0
006016e6: mov      ebx, dword ptr [0x6dd0b4]
006016ec: push     0x9919d0
006016f1: call     ebx
006016f3: mov      edi, dword ptr [esp + 0x164]
006016fa: cmp      dword ptr [edi + 0x15a], 0
00601701: jle      0x6017b3
00601707: call     0x5e8920
0060170c: cmp      dword ptr [0x9db360], 0
00601713: mov      esi, dword ptr [0x6dd0bc]  ; "868"
00601719: je       0x60173c
0060171b: push     0x9919d0
00601720: call     esi
00601722: push     0
00601724: push     0
00601726: push     0x72e620  ; "Device Validation"
0060172b: call     dword ptr [0x9db360]
00601731: push     0x9919d0
00601736: mov      edi, ebx
00601738: call     edi
0060173a: jmp      0x601742
0060173c: mov      edi, dword ptr [0x6dd0b4]
00601742: push     0x9919d0
00601747: call     esi
00601749: mov      eax, dword ptr [0x9db364]
0060174e: inc      eax
0060174f: cmp      eax, 0xfa
00601754: mov      dword ptr [0x9db364], eax
00601759: jle      0x601765
0060175b: mov      dword ptr [0x9db364], 0xfa
00601765: push     0x9919d0
0060176a: call     edi
0060176c: mov      edx, dword ptr [esp + 0x16c]
00601773: mov      eax, dword ptr [esp + 0x164]
0060177a: push     edx
0060177b: push     eax
0060177c: lea      eax, [esp + 0x38]
00601780: call     0x601150
00601785: add      esp, 8
00601788: push     0x9919d0
0060178d: call     esi
0060178f: sub      dword ptr [0x9db364], ebp
00601795: jns      0x6017a1
00601797: mov      dword ptr [0x9db364], 0
006017a1: push     0x9919d0
006017a6: call     edi
006017a8: pop      edi
006017a9: pop      esi
006017aa: pop      ebp
006017ab: pop      ebx
006017ac: add      esp, 0x150
006017b2: ret      
006017b3: cmp      dword ptr [0x9db360], 0
006017ba: je       0x6017d9
006017bc: push     0x9919d0
006017c1: call     esi
006017c3: push     0
006017c5: push     0
006017c7: push     0x72e620  ; "Device Validation"
006017cc: call     dword ptr [0x9db360]
006017d2: push     0x9919d0
006017d7: call     ebx
006017d9: push     0x9919d0
006017de: call     esi
006017e0: mov      eax, dword ptr [0x9db364]
006017e5: add      eax, ebp
006017e7: cmp      eax, 0xfa
006017ec: mov      dword ptr [0x9db364], eax
006017f1: jle      0x6017fd
006017f3: mov      dword ptr [0x9db364], 0xfa
006017fd: push     0x9919d0
00601802: call     ebx
00601804: cmp      dword ptr [0x9db360], 0
0060180b: je       0x60182a
0060180d: push     0x9919d0
00601812: call     esi
00601814: push     0
00601816: push     0
00601818: push     0x72e848  ; "** failed, no supported display modes found"
0060181d: call     dword ptr [0x9db360]
00601823: push     0x9919d0
00601828: call     ebx
0060182a: push     0x9919d0
0060182f: call     esi
00601831: sub      dword ptr [0x9db364], ebp
00601837: jns      0x601843
00601839: mov      dword ptr [0x9db364], 0
00601843: push     0x9919d0
00601848: call     ebx
0060184a: pop      edi
0060184b: pop      esi
0060184c: pop      ebp
0060184d: pop      ebx
0060184e: add      esp, 0x150
00601854: ret      

; ==== provider caps: TextureCaps POW2 / NONPOW2CONDITIONAL -> flag 8  [00681681-00681734]
00681681: mov      eax, dword ptr [ebx + 0x14]
00681684: mov      edx, dword ptr [eax]
00681686: mov      edx, dword ptr [edx + 0x38]
00681689: add      esp, 0xc
0068168c: lea      ecx, [esp + 8]
00681690: push     ecx
00681691: mov      ecx, dword ptr [ebx + 0x18]
00681694: push     1
00681696: push     ecx
00681697: push     eax
00681698: call     edx
0068169a: mov      ecx, dword ptr [esp + 0x44]
0068169e: mov      eax, ecx
006816a0: and      eax, 0x100
006816a5: jne      0x6816ac
006816a7: test     cl, 2
006816aa: jne      0x6816b3
006816ac: or       dword ptr [esi + 0xc0], 8
006816b3: test     dword ptr [esp + 0x2c], 0x8000000
006816bb: je       0x6816c4
006816bd: or       dword ptr [esi + 0xc0], 0x20
006816c4: mov      ecx, dword ptr [esp + 0xfc]
006816cb: test     ecx, 0x200
006816d1: je       0x6816da
006816d3: or       dword ptr [esi + 0xc0], 0x40
006816da: test     ecx, 0x2000000
006816e0: je       0x6816ec
006816e2: or       dword ptr [esi + 0xc0], 0x80
006816ec: cmp      eax, edi
006816ee: je       0x68171b
006816f0: cmp      dword ptr [0x9db360], edi
006816f6: je       0x68171b
006816f8: push     0x9919d0
006816fd: call     dword ptr [0x6dd0bc]  ; "868"
00681703: push     edi
00681704: push     edi
00681705: push     0x738278  ; "D3DProvider: D3D device only supports limited non-POW2 textures"
0068170a: call     dword ptr [0x9db360]
00681710: push     0x9919d0
00681715: call     dword ptr [0x6dd0b4]
0068171b: mov      eax, esi
0068171d: cmp      dword ptr [0x9d8d40], edi
00681723: jne      0x68172b
00681725: mov      dword ptr [0x9d8d40], esi
0068172b: pop      edi
0068172c: pop      ebx
0068172d: add      esp, 0x130

; ==== StartMode: d3d_disp_2d_surf_mode (default 3, forced to 0 without flag 8)  [00681d25-00681dac]
00681d25: mov      eax, dword ptr [ebx + 4]
00681d28: and      dword ptr [ebx + 0xc0], 0xffe8ffef
00681d32: lea      ecx, [esp + 0x14]
00681d36: push     ecx
00681d37: mov      dword ptr [ebx + 0x114], esi
00681d3d: mov      esi, dword ptr [eax + 0x30]
00681d40: lea      edx, [esp + 0x1c]
00681d44: push     edx
00681d45: and      esi, 2
00681d48: push     1
00681d4a: mov      ecx, 0x7382d4  ; "d3d_disp_2d_surf_mode"
00681d4f: mov      dword ptr [esp + 0x2c], esi
00681d53: mov      dword ptr [esp + 0x24], 3
00681d5b: mov      dword ptr [esp + 0x20], 1
00681d63: call     0x664860
00681d68: mov      edi, dword ptr [esp + 0x24]
00681d6c: add      esp, 0xc
00681d6f: test     al, al
00681d71: je       0x681d8a
00681d73: cmp      dword ptr [esp + 0x14], 0
00681d78: jle      0x681d8a
00681d7a: test     edi, edi
00681d7c: jl       0x681da5
00681d7e: cmp      edi, 3
00681d81: jle      0x681d8d
00681d83: mov      edi, 3
00681d88: jmp      0x681d8f
00681d8a: cmp      edi, 3
00681d8d: jne      0x681da7
00681d8f: test     esi, esi
00681d91: jne      0x681d9c
00681d93: test     byte ptr [ebx + 0xd9], 0x10
00681d9a: je       0x681da5
00681d9c: test     byte ptr [ebx + 0xc0], 8
00681da3: jne      0x681da7
00681da5: xor      edi, edi

; ==== StartMode: back buffer format, depth, multisample, CreateDevice HW then SW VP  [0068218e-006825f7]
0068218e: mov      eax, dword ptr [ebx + 0xc0]
00682194: mov      esi, dword ptr [esp + 0x20]
00682198: shr      eax, 0x15
0068219b: not      eax
0068219d: and      eax, 1
006821a0: xor      ecx, ecx
006821a2: test     esi, esi
006821a4: sete     cl
006821a7: xor      edx, edx
006821a9: mov      dword ptr [esp + 0x18], 0
006821b1: cmp      ecx, eax
006821b3: mov      eax, dword ptr [ebx + 0x38]
006821b6: mov      ecx, dword ptr [eax + 0x18]
006821b9: mov      eax, dword ptr [eax + 8]
006821bc: setne    dl
006821bf: push     eax
006821c0: mov      dword ptr [esp + 0x18], ecx
006821c4: mov      dword ptr [esp + 0x2c], edx
006821c8: mov      edx, dword ptr [eax]
006821ca: mov      eax, dword ptr [edx + 0xc]
006821cd: call     eax
006821cf: mov      dword ptr [esp + 0x1c], eax
006821d3: mov      eax, dword ptr [0x787414]
006821d8: test     esi, esi
006821da: je       0x6821e1
006821dc: and      eax, 1
006821df: jmp      0x6821e4
006821e1: and      eax, 2
006821e4: push     0x38
006821e6: lea      ecx, [esp + 0x3c]
006821ea: push     0
006821ec: push     ecx
006821ed: mov      dword ptr [esp + 0x30], eax
006821f1: call     0x6b2310
006821f6: mov      eax, dword ptr [ebp + 0xc]
006821f9: movzx    esi, byte ptr [ebx + 0xd8]
00682200: movsx    edx, word ptr [ebx + 0xd4]
00682207: movsx    ecx, word ptr [ebx + 0xd6]
0068220e: shr      eax, 2
00682211: and      eax, 1
00682214: add      esp, 0xc
00682217: mov      dword ptr [esp + 0x58], eax
0068221b: mov      dword ptr [esp + 0x38], edx
0068221f: mov      dword ptr [esp + 0x3c], ecx
00682223: cmp      esi, 0x10
00682226: je       0x682242
00682228: cmp      esi, 0x18
0068222b: je       0x68223b
0068222d: xor      ecx, ecx
0068222f: cmp      esi, 0x20
00682232: setne    cl
00682235: dec      ecx
00682236: and      ecx, 0x16
00682239: jmp      0x682247
0068223b: mov      ecx, 0x14
00682240: jmp      0x682247
00682242: mov      ecx, 0x17
00682247: mov      dword ptr [esp + 0x40], ecx
0068224b: test     ecx, ecx
0068224d: jne      0x6822ee
00682253: push     0x9919d0
00682258: call     edi
0068225a: mov      eax, dword ptr [0x9db364]
0068225f: inc      eax
00682260: cmp      eax, 0xfa
00682265: mov      dword ptr [0x9db364], eax
0068226a: jle      0x682276
0068226c: mov      dword ptr [0x9db364], 0xfa
00682276: push     0x9919d0
0068227b: call     dword ptr [0x6dd0b4]
00682281: movzx    edx, byte ptr [ebx + 0xd8]
00682288: push     edx
00682289: push     0x7383a0  ; "failed, unsupported backbuffer depth (%d)"
0068228e: call     0x6599d0
00682293: add      esp, 8
00682296: cmp      dword ptr [0x9db360], 0
0068229d: mov      esi, eax
0068229f: je       0x6822be
006822a1: push     0x9919d0
006822a6: call     edi
006822a8: push     0
006822aa: push     0
006822ac: push     esi
006822ad: call     dword ptr [0x9db360]
006822b3: push     0x9919d0
006822b8: call     dword ptr [0x6dd0b4]
006822be: push     0x9919d0
006822c3: call     edi
006822c5: sub      dword ptr [0x9db364], 1
006822cc: jns      0x6822d8
006822ce: mov      dword ptr [0x9db364], 0
006822d8: push     0x9919d0
006822dd: call     dword ptr [0x6dd0b4]
006822e3: xor      eax, eax
006822e5: pop      edi
006822e6: pop      esi
006822e7: pop      ebx
006822e8: mov      esp, ebp
006822ea: pop      ebp
006822eb: ret      0xc
006822ee: cmp      ecx, 0x16
006822f1: jne      0x682317
006822f3: test     dword ptr [ebx + 0xc0], 0x80000
006822fd: jne      0x682317
006822ff: push     ebx
00682300: mov      edi, eax
00682302: call     0x683080
00682307: test     eax, eax
00682309: mov      eax, dword ptr [esp + 0x58]
0068230d: je       0x682317
0068230f: mov      dword ptr [esp + 0x40], 0x23
00682317: mov      edx, dword ptr [esp + 0x40]
0068231b: mov      ecx, dword ptr [esp + 0x10]
0068231f: push     eax
00682320: push     edx
00682321: push     ebx
00682322: mov      dword ptr [esp + 0x50], ecx
00682326: call     0x682f10
0068232b: cmp      dword ptr [esp + 0x58], 0
00682330: mov      dword ptr [esp + 0x48], eax
00682334: mov      eax, dword ptr [esp + 0x1c]
00682338: mov      dword ptr [esp + 0x50], 1
00682340: mov      dword ptr [esp + 0x54], eax
00682344: jne      0x68235f
00682346: mov      ecx, dword ptr [esp + 0x40]
0068234a: mov      edx, dword ptr [esp + 0x3c]
0068234e: mov      eax, dword ptr [esp + 0x38]
00682352: push     ecx
00682353: push     edx
00682354: push     eax
00682355: push     ebx
00682356: call     0x682d60
0068235b: mov      dword ptr [esp + 0x68], eax
0068235f: mov      eax, dword ptr [esp + 0x24]
00682363: mov      ecx, eax
00682365: neg      ecx
00682367: sbb      ecx, ecx
00682369: and      ecx, 0x80000001
0068236f: add      ecx, 0x80000000
00682375: mov      dword ptr [esp + 0x6c], ecx
00682379: test     eax, eax
0068237b: je       0x6823ba
0068237d: cmp      dword ptr [esp + 0x58], 0
00682382: jne      0x6823ba
00682384: mov      eax, dword ptr [0x787414]
00682389: shr      eax, 2
0068238c: and      eax, 3
0068238f: sub      eax, 1
00682392: je       0x6823b2
00682394: sub      eax, 1
00682397: je       0x6823a8
00682399: sub      eax, 1
0068239c: jne      0x6823ba
0068239e: mov      dword ptr [esp + 0x6c], 8
006823a6: jmp      0x6823ba
006823a8: mov      dword ptr [esp + 0x6c], 4
006823b0: jmp      0x6823ba
006823b2: mov      dword ptr [esp + 0x6c], 2
006823ba: cmp      dword ptr [esp + 0x20], 0
006823bf: jne      0x6823ca
006823c1: cmp      dword ptr [ebx + 0xe0], 0
006823c8: je       0x6823f1
006823ca: mov      edi, dword ptr [esp + 0x40]
006823ce: push     ebx
006823cf: call     0x682fc0
006823d4: mov      esi, eax
006823d6: mov      dword ptr [esp + 0x60], esi
006823da: test     esi, esi
006823dc: je       0x6822e3
006823e2: or       dword ptr [esp + 0x64], 2
006823e7: mov      dword ptr [esp + 0x5c], 1
006823ef: jmp      0x6823f5
006823f1: mov      esi, dword ptr [esp + 0x60]
006823f5: cmp      dword ptr [ebx + 0x3c], 0
006823f9: jne      0x682608
006823ff: mov      edx, dword ptr [ebx + 0x38]
00682402: mov      eax, dword ptr [edx + 0x14]
00682405: mov      edx, dword ptr [esp + 0x14]
00682409: mov      ecx, dword ptr [eax]
0068240b: push     edx
0068240c: push     eax
0068240d: mov      eax, dword ptr [ecx + 0x3c]
00682410: mov      dword ptr [esp + 0x20], 1
00682418: call     eax
0068241a: cmp      dword ptr [esp + 0x58], 0
0068241f: mov      ecx, dword ptr [ebx + 0x38]
00682422: mov      dword ptr [0x9d8d50], eax
00682427: mov      eax, dword ptr [ecx + 8]
0068242a: mov      edx, dword ptr [eax]
0068242c: push     eax
0068242d: mov      eax, dword ptr [edx + 0xc]
00682430: je       0x682440
00682432: call     eax
00682434: push     eax
00682435: mov      eax, dword ptr [esp + 0x40]
00682439: call     0x689420
0068243e: jmp      0x682459
00682440: call     eax
00682442: mov      esi, eax
00682444: push     -0x10
00682446: push     esi
00682447: call     dword ptr [0x6dd534]
0068244d: cmp      eax, 0x82000000
00682452: je       0x682459
00682454: call     0x6893f0
00682459: fld1     
0068245b: lea      edi, [ebx + 0x88]
00682461: mov      ecx, 0xe
00682466: fstp     dword ptr [0x78a180]
0068246c: lea      esi, [esp + 0x38]
00682470: rep movsd dword ptr es:[edi], dword ptr [esi]
00682472: mov      ecx, 0xe
00682477: lea      esi, [esp + 0x38]
0068247b: lea      edi, [esp + 0x78]
0068247f: rep movsd dword ptr es:[edi], dword ptr [esi]
00682481: mov      ecx, dword ptr [ebx + 0x38]
00682484: mov      eax, dword ptr [ecx + 0x14]
00682487: mov      edx, dword ptr [eax]
00682489: mov      edx, dword ptr [edx + 0x40]
0068248c: lea      ecx, [ebx + 0x3c]
0068248f: push     ecx
00682490: lea      ecx, [esp + 0x3c]
00682494: push     ecx
00682495: mov      ecx, dword ptr [esp + 0x24]
00682499: push     0x42
0068249b: push     ecx
0068249c: mov      ecx, dword ptr [esp + 0x24]
006824a0: push     1
006824a2: push     ecx
006824a3: push     eax
006824a4: call     edx
006824a6: mov      esi, eax
006824a8: push     esi
006824a9: push     0x7383cc  ; "CreateDevice"
006824ae: lea      eax, [esp + 0x40]
006824b2: call     0x680bf0
006824b7: add      esp, 8
006824ba: test     esi, esi
006824bc: jge      0x6826f8
006824c2: mov      eax, dword ptr [ebx + 0x38]
006824c5: mov      ecx, 0xe
006824ca: lea      esi, [ebx + 0x88]
006824d0: lea      edi, [esp + 0x38]
006824d4: rep movsd dword ptr es:[edi], dword ptr [esi]
006824d6: mov      eax, dword ptr [eax + 0x14]
006824d9: mov      edx, dword ptr [eax]
006824db: mov      edx, dword ptr [edx + 0x40]
006824de: lea      ecx, [ebx + 0x3c]
006824e1: push     ecx
006824e2: lea      ecx, [esp + 0x3c]
006824e6: push     ecx
006824e7: mov      ecx, dword ptr [esp + 0x24]
006824eb: push     0x22
006824ed: push     ecx
006824ee: mov      ecx, dword ptr [esp + 0x24]
006824f2: push     1
006824f4: push     ecx
006824f5: push     eax
006824f6: call     edx
006824f8: mov      esi, eax
006824fa: push     esi
006824fb: push     0x7383dc  ; "CreateDevice (SW VP)"
00682500: lea      eax, [esp + 0x40]
00682504: call     0x680bf0
00682509: add      esp, 8
0068250c: test     esi, esi
0068250e: jge      0x6825f7
00682514: cmp      dword ptr [ebx + 0xa0], 2
0068251b: jne      0x682539
0068251d: mov      dword ptr [ebx + 0xa0], 1
00682527: mov      dword ptr [ebx + 0x94], 0
00682531: lea      esi, [ebx + 0x88]
00682537: jmp      0x682591
00682539: cmp      dword ptr [esp + 0x40], 0x23
0068253e: jne      0x6825e2
00682544: cmp      dword ptr [esp + 0x98], 0
0068254c: mov      dword ptr [esp + 0x80], 0x16
00682557: jne      0x682572
00682559: mov      eax, dword ptr [esp + 0x7c]
0068255d: mov      ecx, dword ptr [esp + 0x78]
00682561: push     0x16
00682563: push     eax
00682564: push     ecx
00682565: push     ebx
00682566: call     0x682d60
0068256b: mov      dword ptr [esp + 0xa8], eax
00682572: or       dword ptr [ebx + 0xc0], 0x80000
0068257c: mov      ecx, 0xe
00682581: lea      esi, [esp + 0x78]
00682585: lea      edi, [ebx + 0x88]
0068258b: rep movsd dword ptr es:[edi], dword ptr [esi]
0068258d: lea      esi, [esp + 0x78]
00682591: mov      edx, dword ptr [ebx + 0x38]
00682594: mov      ecx, 0xe
00682599: lea      edi, [esp + 0x38]
0068259d: rep movsd dword ptr es:[edi], dword ptr [esi]
0068259f: mov      eax, dword ptr [edx + 0x14]
006825a2: mov      edx, dword ptr [eax]
006825a4: mov      edx, dword ptr [edx + 0x40]
006825a7: lea      ecx, [ebx + 0x3c]
006825aa: push     ecx
006825ab: lea      ecx, [esp + 0x3c]
006825af: push     ecx
006825b0: mov      ecx, dword ptr [esp + 0x24]
006825b4: push     0x42
006825b6: push     ecx
006825b7: mov      ecx, dword ptr [esp + 0x24]
006825bb: push     1
006825bd: push     ecx
006825be: push     eax
006825bf: call     edx
006825c1: mov      esi, eax
006825c3: push     esi
006825c4: push     0x7383cc  ; "CreateDevice"
006825c9: lea      eax, [esp + 0x40]
006825cd: call     0x680bf0
006825d2: add      esp, 8
006825d5: test     esi, esi
006825d7: jl       0x6824c2
006825dd: jmp      0x6826f8
006825e2: mov      dword ptr [0x9d8d50], 0
006825ec: xor      eax, eax
006825ee: pop      edi
006825ef: pop      esi
006825f0: pop      ebx
006825f1: mov      esp, ebp
006825f3: pop      ebp
006825f4: ret      0xc

; ==== StartMode: the 2D layer - pow2(screen) 32-bit texture, failure returns 0  [00682853-00682b30]
00682853: mov      ecx, dword ptr [ebx + 0x104]
00682859: mov      eax, dword ptr [ebx + 0x100]
0068285f: mov      edi, dword ptr [ebp + 8]
00682862: mov      dword ptr [esp + 0x2c], ecx
00682866: mov      cl, byte ptr [edi + 8]
00682869: mov      dword ptr [esp + 0x28], eax
0068286d: movzx    eax, cl
00682870: cmp      eax, 0x10
00682873: je       0x6828b7
00682875: cmp      eax, 0x18
00682878: je       0x6828ad
0068287a: cmp      eax, 0x20
0068287d: jne      0x6828c1
0068287f: mov      dword ptr [esp + 0x10], 0x16
00682887: cmp      dword ptr [esp + 0x20], 0
0068288c: jne      0x682897
0068288e: cmp      dword ptr [ebx + 0xe0], 0
00682895: je       0x6828f6
00682897: cmp      cl, 0x20
0068289a: je       0x6828ee
0068289c: call     0x659cd0
006828a1: movzx    eax, byte ptr [edi + 8]
006828a5: push     eax
006828a6: push     0x738444  ; "failed, unsupported bit depth (%d) for current mode"
006828ab: jmp      0x6828d0
006828ad: mov      dword ptr [esp + 0x10], 0x14
006828b5: jmp      0x682887
006828b7: mov      dword ptr [esp + 0x10], 0x17
006828bf: jmp      0x682887
006828c1: call     0x659cd0
006828c6: movzx    edx, byte ptr [edi + 8]
006828ca: push     edx
006828cb: push     0x738420  ; "failed, unsupported bit depth (%d)"
006828d0: call     0x6599d0
006828d5: add      esp, 8
006828d8: push     eax
006828d9: call     0x659c90
006828de: call     0x659d10
006828e3: xor      eax, eax
006828e5: pop      edi
006828e6: pop      esi
006828e7: pop      ebx
006828e8: mov      esp, ebp
006828ea: pop      ebp
006828eb: ret      0xc
006828ee: mov      dword ptr [esp + 0x10], 0x15
006828f6: test     byte ptr [ebx + 0xc0], 8
006828fd: movsx    edx, word ptr [edi + 6]
00682901: movsx    esi, word ptr [edi + 4]
00682905: mov      dword ptr [esp + 0x14], edx
00682909: mov      dword ptr [ebx + 0xf8], esi
0068290f: mov      dword ptr [ebx + 0xfc], edx
00682915: jne      0x682945
00682917: mov      eax, esi
00682919: call     0x680d90
0068291e: mov      esi, eax
00682920: mov      eax, edx
00682922: call     0x680d90
00682927: mov      edx, eax
00682929: mov      dword ptr [esp + 0x14], edx
0068292d: cmp      esi, 0x1000
00682933: ja       0x6822e3
00682939: cmp      edx, 0x1000
0068293f: ja       0x6822e3
00682945: mov      eax, dword ptr [ebx + 0xe4]
0068294b: mov      dword ptr [ebx + 0x100], esi
00682951: mov      dword ptr [ebx + 0x104], edx
00682957: cmp      eax, 2
0068295a: je       0x682a4a
00682960: cmp      eax, 3
00682963: je       0x682a4a
00682969: cmp      dword ptr [ebx + 0x40], 0
0068296d: lea      edi, [ebx + 0x40]
00682970: je       0x68298f
00682972: mov      ecx, dword ptr [esp + 0x10]
00682976: cmp      dword ptr [ebx + 0x68], ecx
00682979: jne      0x68298f
0068297b: cmp      esi, dword ptr [esp + 0x28]
0068297f: jne      0x68298f
00682981: mov      edx, dword ptr [esp + 0x2c]
00682985: cmp      dword ptr [esp + 0x14], edx
00682989: je       0x682ad0
0068298f: mov      ecx, dword ptr [ebx + 0x150]
00682995: test     ecx, ecx
00682997: je       0x6829ac
00682999: mov      eax, dword ptr [ecx]
0068299b: mov      edx, dword ptr [eax + 0x74]
0068299e: push     1
006829a0: call     edx
006829a2: mov      dword ptr [ebx + 0x150], 0
006829ac: mov      eax, dword ptr [ebx + 0x108]
006829b2: test     eax, eax
006829b4: je       0x6829be
006829b6: mov      ecx, dword ptr [eax]
006829b8: mov      edx, dword ptr [ecx + 8]
006829bb: push     eax
006829bc: call     edx
006829be: mov      eax, dword ptr [ebx + 0x44]
006829c1: mov      dword ptr [ebx + 0x108], 0
006829cb: test     eax, eax
006829cd: je       0x6829d7
006829cf: mov      ecx, dword ptr [eax]
006829d1: mov      edx, dword ptr [ecx + 8]
006829d4: push     eax
006829d5: call     edx
006829d7: mov      eax, dword ptr [edi]
006829d9: mov      dword ptr [ebx + 0x44], 0
006829e0: test     eax, eax
006829e2: je       0x6829ec
006829e4: mov      ecx, dword ptr [eax]
006829e6: mov      edx, dword ptr [ecx + 8]
006829e9: push     eax
006829ea: call     edx
006829ec: mov      eax, dword ptr [ebx + 0x3c]
006829ef: xor      ecx, ecx
006829f1: cmp      dword ptr [ebx + 0xe4], ecx
006829f7: push     0
006829f9: setne    cl
006829fc: push     edi
006829fd: mov      dword ptr [edi], 0
00682a03: mov      edx, dword ptr [eax]
00682a05: mov      edx, dword ptr [edx + 0x5c]
00682a08: inc      ecx
00682a09: push     ecx
00682a0a: mov      ecx, dword ptr [esp + 0x1c]
00682a0e: push     ecx
00682a0f: mov      ecx, dword ptr [esp + 0x24]
00682a13: push     0
00682a15: push     1
00682a17: push     ecx
00682a18: push     esi
00682a19: push     eax
00682a1a: call     edx
00682a1c: mov      esi, eax
00682a1e: test     esi, esi
00682a20: je       0x682ad0
00682a26: call     0x659cd0
00682a2b: mov      eax, esi
00682a2d: call     0x679d10
00682a32: push     eax
00682a33: push     0x738478  ; "failed to create secondary surf"
00682a38: push     0x6f713c  ; "%s (%s)"
00682a3d: call     0x6599d0
00682a42: add      esp, 0xc
00682a45: jmp      0x6828d8
00682a4a: mov      eax, dword ptr [ebx + 0x40]
00682a4d: lea      edi, [ebx + 0x40]
00682a50: test     eax, eax
00682a52: je       0x682a5c
00682a54: mov      ecx, dword ptr [eax]
00682a56: mov      edx, dword ptr [ecx + 8]
00682a59: push     eax
00682a5a: call     edx
00682a5c: cmp      dword ptr [ebx + 0x44], 0
00682a60: mov      dword ptr [edi], 0
00682a66: je       0x682a81
00682a68: mov      eax, dword ptr [esp + 0x10]
00682a6c: cmp      dword ptr [ebx + 0x68], eax
00682a6f: jne      0x682a81
00682a71: cmp      esi, dword ptr [esp + 0x28]
00682a75: jne      0x682a81
00682a77: mov      ecx, dword ptr [esp + 0x2c]
00682a7b: cmp      dword ptr [esp + 0x14], ecx
00682a7f: je       0x682ad0
00682a81: mov      ecx, dword ptr [ebx + 0x150]
00682a87: test     ecx, ecx
00682a89: je       0x682a9e
00682a8b: mov      edx, dword ptr [ecx]
00682a8d: mov      eax, dword ptr [edx + 0x74]
00682a90: push     1
00682a92: call     eax
00682a94: mov      dword ptr [ebx + 0x150], 0
00682a9e: mov      eax, dword ptr [ebx + 0x108]
00682aa4: test     eax, eax
00682aa6: je       0x682ab0
00682aa8: mov      ecx, dword ptr [eax]
00682aaa: mov      edx, dword ptr [ecx + 8]
00682aad: push     eax
00682aae: call     edx
00682ab0: mov      eax, dword ptr [ebx + 0x44]
00682ab3: mov      dword ptr [ebx + 0x108], 0
00682abd: test     eax, eax
00682abf: je       0x682ac9
00682ac1: mov      ecx, dword ptr [eax]
00682ac3: mov      edx, dword ptr [ecx + 8]
00682ac6: push     eax
00682ac7: call     edx
00682ac9: mov      dword ptr [ebx + 0x44], 0
00682ad0: cmp      dword ptr [ebx + 0x44], 0
00682ad4: mov      eax, dword ptr [esp + 0x10]
00682ad8: mov      dword ptr [ebx + 0x68], eax
00682adb: mov      dword ptr [ebx + 0xc8], 1
00682ae5: mov      dword ptr [ebx + 0xc4], 0
00682aef: jne      0x682b2b
00682af1: cmp      dword ptr [ebx + 0xe4], 0
00682af8: je       0x682b2b
00682afa: mov      eax, ebx
00682afc: call     0x6831f0
00682b01: mov      esi, eax
00682b03: test     esi, esi
00682b05: je       0x682b2b
00682b07: call     0x659cd0
00682b0c: mov      eax, esi
00682b0e: call     0x679d10
00682b13: push     eax
00682b14: push     0x738498  ; "failed to create secondary hw surf"
00682b19: push     0x6f713c  ; "%s (%s)"
00682b1e: call     0x6599d0
00682b23: add      esp, 0xc
00682b26: jmp      0x6828d8
00682b2b: mov      edi, dword ptr [edi]
00682b2d: test     edi, edi

; ==== hw 2D surface (modes 1-4): CreateTexture DYNAMIC / RENDERTARGET, DEFAULT pool  [006831f0-006832e1]
006831f0: push     esi
006831f1: mov      esi, eax
006831f3: mov      eax, dword ptr [esi + 0xe4]
006831f9: test     eax, eax
006831fb: jne      0x6831ff
006831fd: pop      esi
006831fe: ret      
006831ff: cmp      dword ptr [esi + 0x40], 0
00683203: jne      0x683211
00683205: cmp      eax, 1
00683208: jne      0x683211
0068320a: mov      eax, 0x80004005
0068320f: pop      esi
00683210: ret      
00683211: push     edi
00683212: cmp      eax, 4
00683215: jne      0x6832a0
0068321b: mov      edx, dword ptr [esi + 0x5c]
0068321e: mov      eax, dword ptr [esi + 0x3c]
00683221: mov      ecx, dword ptr [eax]
00683223: push     0
00683225: lea      edi, [esi + 0x108]
0068322b: push     edi
0068322c: push     0
0068322e: push     edx
0068322f: mov      edx, dword ptr [esi + 0x58]
00683232: push     edx
00683233: mov      edx, dword ptr [esi + 0x68]
00683236: push     edx
00683237: mov      edx, dword ptr [esi + 0x104]
0068323d: push     edx
0068323e: mov      edx, dword ptr [esi + 0x100]
00683244: push     edx
00683245: push     eax
00683246: mov      eax, dword ptr [ecx + 0x70]
00683249: call     eax
0068324b: test     eax, eax
0068324d: jl       0x6832de
00683253: mov      eax, dword ptr [esi + 0x3c]
00683256: mov      ecx, dword ptr [eax]
00683258: push     0
0068325a: lea      edx, [esi + 0x44]
0068325d: push     edx
0068325e: mov      edx, dword ptr [esi + 0x68]
00683261: push     0
00683263: push     edx
00683264: mov      edx, dword ptr [esi + 0x104]
0068326a: push     1
0068326c: push     1
0068326e: push     edx
0068326f: mov      edx, dword ptr [esi + 0x100]
00683275: push     edx
00683276: push     eax
00683277: mov      eax, dword ptr [ecx + 0x5c]
0068327a: call     eax
0068327c: mov      esi, eax
0068327e: test     esi, esi
00683280: jge      0x68329b
00683282: mov      eax, dword ptr [edi]
00683284: test     eax, eax
00683286: je       0x683290
00683288: mov      ecx, dword ptr [eax]
0068328a: mov      edx, dword ptr [ecx + 8]
0068328d: push     eax
0068328e: call     edx
00683290: mov      dword ptr [edi], 0
00683296: pop      edi
00683297: mov      eax, esi
00683299: pop      esi
0068329a: ret      
0068329b: pop      edi
0068329c: xor      eax, eax
0068329e: pop      esi
0068329f: ret      
006832a0: xor      ecx, ecx
006832a2: cmp      eax, 2
006832a5: jne      0x6832ae
006832a7: mov      ecx, 0x200
006832ac: jmp      0x6832b6
006832ae: cmp      eax, 3
006832b1: jne      0x6832b6
006832b3: lea      ecx, [eax - 2]
006832b6: mov      eax, dword ptr [esi + 0x3c]
006832b9: mov      edx, dword ptr [eax]
006832bb: mov      edx, dword ptr [edx + 0x5c]
006832be: push     0
006832c0: lea      edi, [esi + 0x44]
006832c3: push     edi
006832c4: mov      edi, dword ptr [esi + 0x68]
006832c7: push     0
006832c9: push     edi
006832ca: push     ecx
006832cb: mov      ecx, dword ptr [esi + 0x104]
006832d1: push     1
006832d3: push     ecx
006832d4: mov      ecx, dword ptr [esi + 0x100]
006832da: push     ecx
006832db: push     eax
006832dc: call     edx
006832de: pop      edi
006832df: pop      esi
006832e0: ret      

; ==== multisample: steps multisampletype down to NONE  [00682f10-00682fb6]
00682f10: push     ebx
00682f11: mov      ebx, dword ptr [esp + 8]
00682f15: push     ebp
00682f16: mov      ebp, dword ptr [esp + 0x14]
00682f1a: lea      eax, [esp + 0xc]
00682f1e: push     eax
00682f1f: lea      ecx, [esp + 0x18]
00682f23: push     ecx
00682f24: push     1
00682f26: mov      ecx, 0x6fbad0  ; "multisampletype"
00682f2b: mov      dword ptr [esp + 0x20], 0
00682f33: mov      dword ptr [esp + 0x18], 1
00682f3b: call     0x664860
00682f40: add      esp, 0xc
00682f43: test     al, al
00682f45: je       0x682faf
00682f47: cmp      dword ptr [esp + 0xc], 0
00682f4c: jle      0x682faf
00682f4e: mov      edx, dword ptr [ebx + 0x38]
00682f51: push     esi
00682f52: mov      esi, dword ptr [esp + 0x18]
00682f56: cmp      esi, 2
00682f59: push     edi
00682f5a: mov      edi, dword ptr [edx + 0x18]
00682f5d: jl       0x682f9d
00682f5f: cmp      esi, 0x10
00682f62: jle      0x682f6b
00682f64: mov      esi, 0x10
00682f69: jmp      0x682f74
00682f6b: test     esi, esi
00682f6d: je       0x682fa6
00682f6f: cmp      esi, 1
00682f72: jle      0x682f9b
00682f74: mov      edx, dword ptr [esp + 0x18]
00682f78: mov      eax, dword ptr [ebx + 0x38]
00682f7b: mov      eax, dword ptr [eax + 0x14]
00682f7e: mov      ecx, dword ptr [eax]
00682f80: push     0
00682f82: push     esi
00682f83: push     ebp
00682f84: push     edx
00682f85: push     1
00682f87: push     edi
00682f88: push     eax
00682f89: mov      eax, dword ptr [ecx + 0x2c]
00682f8c: call     eax
00682f8e: test     eax, eax
00682f90: jge      0x682f98
00682f92: dec      esi
00682f93: cmp      esi, 1
00682f96: jg       0x682f74
00682f98: cmp      esi, 1
00682f9b: jne      0x682fa6
00682f9d: pop      edi
00682f9e: pop      esi
00682f9f: pop      ebp
00682fa0: xor      eax, eax
00682fa2: pop      ebx
00682fa3: ret      0xc
00682fa6: pop      edi
00682fa7: mov      eax, esi
00682fa9: pop      esi
00682faa: pop      ebp
00682fab: pop      ebx
00682fac: ret      0xc
00682faf: mov      eax, dword ptr [esp + 0x14]
00682fb3: pop      ebp
00682fb4: pop      ebx

; ==== depth format: D32 D24S8 D24X4S4 D24X8 D16 D15S1, format + match  [00682fc0-00683073]
00682fc0: sub      esp, 0x18
00682fc3: push     ebx
00682fc4: mov      ebx, dword ptr [esp + 0x20]
00682fc8: mov      eax, dword ptr [ebx + 0x38]
00682fcb: push     ebp
00682fcc: mov      ebp, dword ptr [eax + 0x18]
00682fcf: push     esi
00682fd0: mov      dword ptr [esp + 0xc], 0x47
00682fd8: mov      dword ptr [esp + 0x10], 0x4b
00682fe0: mov      dword ptr [esp + 0x14], 0x4f
00682fe8: mov      dword ptr [esp + 0x18], 0x4d
00682ff0: mov      dword ptr [esp + 0x1c], 0x50
00682ff8: mov      dword ptr [esp + 0x20], 0x49
00683000: mov      dword ptr [esp + 0x28], 0
00683008: jmp      0x683010
0068300a: lea      ebx, [ebx]
00683010: mov      edx, dword ptr [esp + 0x28]
00683014: mov      esi, dword ptr [esp + edx*4 + 0xc]
00683018: mov      ecx, dword ptr [ebx + 0x38]
0068301b: mov      eax, dword ptr [ecx + 0x14]
0068301e: mov      ecx, dword ptr [eax]
00683020: mov      edx, dword ptr [ecx + 0x28]
00683023: push     esi
00683024: push     1
00683026: push     2
00683028: push     edi
00683029: push     1
0068302b: push     ebp
0068302c: push     eax
0068302d: call     edx
0068302f: test     eax, eax
00683031: jl       0x68304b
00683033: mov      eax, dword ptr [ebx + 0x38]
00683036: mov      eax, dword ptr [eax + 0x14]
00683039: mov      ecx, dword ptr [eax]
0068303b: mov      edx, dword ptr [ecx + 0x30]
0068303e: push     esi
0068303f: push     edi
00683040: push     edi
00683041: push     1
00683043: push     ebp
00683044: push     eax
00683045: call     edx
00683047: test     eax, eax
00683049: jge      0x683064
0068304b: mov      eax, dword ptr [esp + 0x28]
0068304f: inc      eax
00683050: cmp      eax, 6
00683053: mov      dword ptr [esp + 0x28], eax
00683057: jl       0x683010
00683059: pop      esi
0068305a: pop      ebp
0068305b: xor      eax, eax
0068305d: pop      ebx
0068305e: add      esp, 0x18
00683061: ret      4
00683064: mov      eax, dword ptr [esp + 0x28]
00683068: mov      eax, dword ptr [esp + eax*4 + 0xc]
0068306c: pop      esi
0068306d: pop      ebp
0068306e: pop      ebx
0068306f: add      esp, 0x18
