# OUI Database



## Categorized by Manufacturer

<details>

<summary><b>RING</b> <code>11 OUIs</code></summary>

- `18:7F:88`

- `24:2B:D6`

- `34:3E:A4`

- `54:E0:19`

- `5C:47:5E`

- `64:9A:63`

- `90:48:6C`

- `9C:76:13`

- `AC:9F:C3`

- `C4:DB:AD`

- `CC:3B:FB`

**Copy OUIs:**

```
18:7F:88
24:2B:D6
34:3E:A4
54:E0:19
5C:47:5E
64:9A:63
90:48:6C
9C:76:13
AC:9F:C3
C4:DB:AD
CC:3B:FB
```

> **Category:** Doorbell/Security Camera  

> **Detection Range:** Typical WiFi/BLE range  

> **Common Devices:** Ring Doorbell, Ring Camera, Ring Chime

</details>

<details>

<summary><b>FLOCK SAFETY</b> <code>1 OUI</code></summary>

- `B4:1E:52`

**Copy OUIs:**

```
B4:1E:52
```

> **Category:** Automated License Plate Reader (ALPR) / Security Camera  

> **Detection Range:** WiFi/Cellular  

> **Common Devices:** Flock Safety Camera, Falcon Camera, Raven Camera

</details>

<details>

<summary><b>FLOCK SAFETY (WiFi Promiscuous — @NitekryDPaul research)</b> <code>30 OUIs</code></summary>

- `70:C9:4E`

- `3C:91:80`

- `D8:F3:BC`

- `80:30:49`

- `B8:35:32`

- `14:5A:FC`

- `74:4C:A1`

- `08:3A:88`

- `9C:2F:9D`

- `C0:35:32`

- `94:08:53`

- `E4:AA:EA`

- `F4:6A:DD`

- `F8:A2:D6`

- `24:B2:B9`

- `00:F4:8D`

- `D0:39:57`

- `E8:D0:FC`

- `E0:4F:43`

- `B8:1E:A4`

- `70:08:94`

- `58:8E:81`

- `EC:1B:BD`

- `3C:71:BF`

- `58:00:E3`

- `90:35:EA`

- `5C:93:A2`

- `64:6E:69`

- `48:27:EA`

- `A4:CF:12`

**Copy OUIs:**

```
70:C9:4E
3C:91:80
D8:F3:BC
80:30:49
B8:35:32
14:5A:FC
74:4C:A1
08:3A:88
9C:2F:9D
C0:35:32
94:08:53
E4:AA:EA
F4:6A:DD
F8:A2:D6
24:B2:B9
00:F4:8D
D0:39:57
E8:D0:FC
E0:4F:43
B8:1E:A4
70:08:94
58:8E:81
EC:1B:BD
3C:71:BF
58:00:E3
90:35:EA
5C:93:A2
64:6E:69
48:27:EA
A4:CF:12
```

> **Category:** Automated License Plate Reader (ALPR) / Security Camera  

> **Detection Range:** WiFi 2.4 GHz (promiscuous mode, addr1 + addr2)  

> **Common Devices:** Flock Safety infrastructure (cameras, uplinks, peripherals)  

> **Research Credit:** ØяĐöØцяöЪöяцฐ / **@NitekryDPaul** — identified these prefixes through 2.4 GHz promiscuous-mode analysis, including the addr1-receiver detection technique that catches Flock stations during their burst-sleep duty cycle

</details>

<details>

<summary><b>AXON (BLE)</b> <code>1 OUI</code></summary>

- `00:25:DF`

**Copy OUIs:**

```
00:25:DF
```

> **Category:** Body Camera / Law Enforcement

> **Detection Range:** BLE range — field-validated, stable non-randomised MAC in advertisements

> **Common Devices:** Axon Body 3/4, Fleet dash cam, Taser 7/10 — same IEEE block as the WiFi side; BT SIG company ID `0x034D` and service UUID `0xFC81` complete the signature (AXON preset installs all five)

</details>

<details>

<summary><b>DJI</b> <code>10 OUIs</code></summary>

- `0C:9A:E6`

- `8C:58:23`

- `04:A8:5A`

- `58:B8:58`

- `E4:7A:2C`

- `60:60:1F`

- `48:1C:B9`

- `34:D2:62`

- `4C:43:F6`

- `88:29:85`

**Copy OUIs:**

```
0C:9A:E6
8C:58:23
04:A8:5A
58:B8:58
E4:7A:2C
60:60:1F
48:1C:B9
34:D2:62
4C:43:F6
88:29:85
```

> **Category:** Consumer & Commercial Drones  

> **Detection Range:** WiFi/OcuSync up to several km  

> **Common Devices:** Mavic, Phantom, Inspire, Mini series

</details>

<details>

<summary><b>PARROT</b> <code>5 OUIs</code></summary>

- `00:12:1C`

- `00:26:7E`

- `90:03:B7`

- `90:3A:E6`

- `A0:14:3D`

**Copy OUIs:**

```
00:12:1C
00:26:7E
90:03:B7
90:3A:E6
A0:14:3D
```

> **Category:** Consumer & Commercial Drones  

> **Detection Range:** WiFi/BLE range  

> **Common Devices:** Parrot Anafi, Parrot Bebop, Parrot AR.Drone

</details>

<details>

<summary><b>SKYDIO</b> <code>1 OUI</code></summary>

- `38:1D:14`

**Copy OUIs:**

```
38:1D:14
```

> **Category:** Commercial & Enterprise Drones  

> **Detection Range:** WiFi range  

> **Common Devices:** Skydio 2, Skydio X2, Skydio 3

</details>

<details>

<summary><b>META/RAYBAN SMARTGLASSES</b> <code>4 OUIs</code></summary>

*sourced from [glass-detect repository](https://github.com/sh4d0wm45k/glass-detect/blob/main/glass-detect/glass-detect.ino#L21)*

Last one from Luxottica Group S.P.A. detected by @konradit. The three Luxottica blocks are IEEE-registered; `7C:2A:9E` is unregistered (community-reported). Blocks registered to Apple and HPE were removed on 2026-11-04. The glasses use rotating random addresses, so the META preset (company ID + service UUID + name) is the reliable signature. The three Luxottica blocks are IEEE-registered; `7C:2A:9E` is unregistered (community-reported). Blocks registered to Apple and HPE were removed on 2026-11-04. The glasses use rotating random addresses, so the META preset (company ID + service UUID + name) is the reliable signature.

- `7C:2A:9E`
- `98:59:49`
- `80:AA:1C`
- `38:47:12`

**Copy OUIs:**

```
7C:2A:9E
98:59:49
80:AA:1C
38:47:12
```

> **Category:** Smartglasses  

> **Detection Range:** WiFi/BLE range  

> **Common Devices:** Meta/Ray-Ban Smartglasses

</details>

<details>

<summary><b>FLIPPER ZERO</b> <code>3 OUIs</code></summary>

- `80:E1:26`

- `80:E1:27`

- `0C:FA:22`

**Copy OUIs:**

```
80:E1:26
80:E1:27
0C:FA:22
```

> **Category:** Pentest / Hacking Tool  

> **Detection Range:** BLE range  

> **Common Devices:** Flipper Zero (80:E1:26/27 are the prefixes its BLE addresses use, community-reported and not registered to anyone; 0C:FA:22 is Flipper Devices' IEEE block; the FLIPPER preset adds its service UUIDs 0x3080-0x3083 and name)

</details>

<details>

<summary><b>ZEBRA TECHNOLOGIES</b> <code>17 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `00:05:12`

- `00:07:4D`

- `00:15:70`

- `00:23:68`

- `00:A0:F8`

- `40:83:DE`

- `48:8E:B7`

- `60:95:32`

- `74:93:A4`

- `78:B8:D6`

- `84:24:8D`

- `88:BC:AC`

- `90:75:DE`

- `94:FB:29`

- `C4:7D:CC`

- `C8:1C:FE`

- `FC:59:7A`

**Copy OUIs:**

```
00:05:12
00:07:4D
00:15:70
00:23:68
00:A0:F8
40:83:DE
48:8E:B7
60:95:32
74:93:A4
78:B8:D6
84:24:8D
88:BC:AC
90:75:DE
94:FB:29
C4:7D:CC
C8:1C:FE
FC:59:7A
```

> **Category:** Rugged handhelds / tablets / printers  

> **Detection Range:** BLE range  

> **Common Devices:** Zebra TC/MC/ET handhelds and tablets, ZQ/ZD Bluetooth printers, ring scanners (enterprise fleets often pin the MAC)

</details>

<details>

<summary><b>HONEYWELL / INTERMEC</b> <code>5 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `00:10:40`

- `00:10:20`

- `0C:23:69`

- `AC:77:13`

- `94:CA:0F`

**Copy OUIs:**

```
00:10:40
00:10:20
0C:23:69
AC:77:13
94:CA:0F
```

> **Category:** Rugged handhelds / scanners  

> **Detection Range:** BLE range  

> **Common Devices:** Honeywell, Intermec and Hand Held Products scanners and mobile computers

</details>

<details>

<summary><b>DATALOGIC</b> <code>3 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `00:07:BE`

- `00:11:83`

- `00:18:67`

**Copy OUIs:**

```
00:07:BE
00:11:83
00:18:67
```

> **Category:** Rugged handhelds / scanners  

> **Detection Range:** BLE range  

> **Common Devices:** Datalogic scanners and mobile computers

</details>

<details>

<summary><b>SONIM</b> <code>2 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `00:24:A3`

- `74:4D:DC`

**Copy OUIs:**

```
00:24:A3
74:4D:DC
```

> **Category:** Rugged phones  

> **Detection Range:** BLE range  

> **Common Devices:** Sonim rugged push-to-talk phones (first-responder carriers)

</details>

<details>

<summary><b>PANASONIC (TOUGHBOOK)</b> <code>6 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `4C:36:4E`

- `B8:20:8E`

- `BC:3E:0B`

- `00:19:87`

- `54:CD:10`

- `D8:B1:2A`

**Copy OUIs:**

```
4C:36:4E
B8:20:8E
BC:3E:0B
00:19:87
54:CD:10
D8:B1:2A
```

> **Category:** Rugged laptops / tablets  

> **Detection Range:** BLE range  

> **Common Devices:** Panasonic Toughbook / Toughpad (common in patrol cars)

</details>

<details>

<summary><b>MOTOROLA SOLUTIONS</b> <code>8 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `00:04:7D`

- `00:18:85`

- `00:1F:92`

- `10:74:6F`

- `4C:CC:34`

- `9C:86:2B`

- `B8:E2:8C`

- `48:2C:EA`

**Copy OUIs:**

```
00:04:7D
00:18:85
00:1F:92
10:74:6F
4C:CC:34
9C:86:2B
B8:E2:8C
48:2C:EA
```

> **Category:** Police radios / body cams / in-car  

> **Detection Range:** BLE range  

> **Common Devices:** APX and business radios, Si-series and V300 body cams, in-car video (the PRESET "police" adds its BLE IDs)

</details>

<details>

<summary><b>AVIGILON</b> <code>1 OUI</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `70:1A:D5`

**Copy OUIs:**

```
70:1A:D5
```

> **Category:** Security Cameras  

> **Detection Range:** BLE range  

> **Common Devices:** Avigilon Alta cloud cameras (Motorola Solutions)

</details>

<details>

<summary><b>META PLATFORMS</b> <code>16 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `2C:26:17`

- `48:05:60`

- `48:57:DD`

- `50:99:03`

- `78:C4:FA`

- `80:F3:EF`

- `84:57:F7`

- `88:25:08`

- `94:F9:29`

- `A4:0E:2B`

- `B4:17:A8`

- `C0:DD:8A`

- `CC:A1:74`

- `D0:B3:C2`

- `D4:D6:59`

- `F4:4E:35`

**Copy OUIs:**

```
2C:26:17
48:05:60
48:57:DD
50:99:03
78:C4:FA
80:F3:EF
84:57:F7
88:25:08
94:F9:29
A4:0E:2B
B4:17:A8
C0:DD:8A
CC:A1:74
D0:B3:C2
D4:D6:59
F4:4E:35
```

> **Category:** Smartglasses / VR  

> **Detection Range:** BLE range  

> **Common Devices:** Meta Quest headsets, Portal, other Meta hardware (Ray-Ban Meta glasses use Luxottica blocks, above)

</details>

<details>

<summary><b>SNAP (SPECTACLES)</b> <code>1 OUI</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `98:A4:0E`

**Copy OUIs:**

```
98:A4:0E
```

> **Category:** Smartglasses  

> **Detection Range:** BLE range  

> **Common Devices:** Snap Spectacles camera glasses

</details>

<details>

<summary><b>TESLA</b> <code>6 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `0C:29:8F`

- `4C:FC:AA`

- `54:F8:F0`

- `90:E6:43`

- `98:ED:5C`

- `D4:4F:14`

**Copy OUIs:**

```
0C:29:8F
4C:FC:AA
54:F8:F0
90:E6:43
98:ED:5C
D4:4F:14
```

> **Category:** Vehicles  

> **Detection Range:** BLE range  

> **Common Devices:** Tesla vehicles (the PRESET "vehicles" adds the per-car BLE name)

</details>

<details>

<summary><b>SQUARE</b> <code>1 OUI</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `44:59:25`

**Copy OUIs:**

```
44:59:25
```

> **Category:** Payment readers  

> **Detection Range:** BLE range  

> **Common Devices:** Square card readers

</details>

<details>

<summary><b>VERIFONE</b> <code>5 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `00:0B:4F`

- `60:C7:98`

- `88:BF:35`

- `A4:60:11`

- `E8:05:DC`

**Copy OUIs:**

```
00:0B:4F
60:C7:98
88:BF:35
A4:60:11
E8:05:DC
```

> **Category:** Payment terminals  

> **Detection Range:** BLE range  

> **Common Devices:** Verifone card terminals

</details>

<details>

<summary><b>INGENICO</b> <code>7 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `00:03:81`

- `10:1E:DA`

- `38:EF:E3`

- `44:D4:7F`

- `54:7F:54`

- `54:E1:40`

- `B4:00:16`

**Copy OUIs:**

```
00:03:81
10:1E:DA
38:EF:E3
44:D4:7F
54:7F:54
54:E1:40
B4:00:16
```

> **Category:** Payment terminals  

> **Detection Range:** BLE range  

> **Common Devices:** Ingenico card terminals

</details>

<details>

<summary><b>KASTLE SYSTEMS</b> <code>1 OUI</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `7C:55:A7`

**Copy OUIs:**

```
7C:55:A7
```

> **Category:** Access control  

> **Detection Range:** BLE range  

> **Common Devices:** Kastle building access readers

</details>

<details>

<summary><b>BRIVO</b> <code>1 OUI</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `00:1D:00`

**Copy OUIs:**

```
00:1D:00
```

> **Category:** Access control  

> **Detection Range:** BLE range  

> **Common Devices:** Brivo access control panels

</details>

<details>

<summary><b>RASPBERRY PI</b> <code>8 OUIs</code></summary>

*IEEE MA-L registry, checked 2026-11-04*

- `28:CD:C1`

- `2C:CF:67`

- `88:A2:9E`

- `98:FE:54`

- `B8:27:EB`

- `D8:3A:DD`

- `DC:A6:32`

- `E4:5F:01`

**Copy OUIs:**

```
28:CD:C1
2C:CF:67
88:A2:9E
98:FE:54
B8:27:EB
D8:3A:DD
DC:A6:32
E4:5F:01
```

> **Category:** Hobbyist / Hacking Boards  

> **Detection Range:** BLE range  

> **Common Devices:** Raspberry Pi boards (Pi-based tools such as Pwnagotchi, Kismet rigs)

</details>



---

## Copy All OUIs for OUI-SPY

```
18:7F:88
24:2B:D6
34:3E:A4
54:E0:19
5C:47:5E
64:9A:63
90:48:6C
9C:76:13
AC:9F:C3
C4:DB:AD
CC:3B:FB
B4:1E:52
70:C9:4E
3C:91:80
D8:F3:BC
80:30:49
B8:35:32
14:5A:FC
74:4C:A1
08:3A:88
9C:2F:9D
C0:35:32
94:08:53
E4:AA:EA
F4:6A:DD
F8:A2:D6
24:B2:B9
00:F4:8D
D0:39:57
E8:D0:FC
E0:4F:43
B8:1E:A4
70:08:94
58:8E:81
EC:1B:BD
3C:71:BF
58:00:E3
90:35:EA
5C:93:A2
64:6E:69
48:27:EA
A4:CF:12
00:25:DF
0C:9A:E6
8C:58:23
04:A8:5A
58:B8:58
E4:7A:2C
60:60:1F
48:1C:B9
34:D2:62
4C:43:F6
88:29:85
00:12:1C
00:26:7E
90:03:B7
90:3A:E6
A0:14:3D
38:1D:14
7C:2A:9E
98:59:49
80:AA:1C
38:47:12
80:E1:26
80:E1:27
0C:FA:22
00:05:12
00:07:4D
00:15:70
00:23:68
00:A0:F8
40:83:DE
48:8E:B7
60:95:32
74:93:A4
78:B8:D6
84:24:8D
88:BC:AC
90:75:DE
94:FB:29
C4:7D:CC
C8:1C:FE
FC:59:7A
00:10:40
00:10:20
0C:23:69
AC:77:13
94:CA:0F
00:07:BE
00:11:83
00:18:67
00:24:A3
74:4D:DC
4C:36:4E
B8:20:8E
BC:3E:0B
00:19:87
54:CD:10
D8:B1:2A
00:04:7D
00:18:85
00:1F:92
10:74:6F
4C:CC:34
9C:86:2B
B8:E2:8C
48:2C:EA
70:1A:D5
2C:26:17
48:05:60
48:57:DD
50:99:03
78:C4:FA
80:F3:EF
84:57:F7
88:25:08
94:F9:29
A4:0E:2B
B4:17:A8
C0:DD:8A
CC:A1:74
D0:B3:C2
D4:D6:59
F4:4E:35
98:A4:0E
0C:29:8F
4C:FC:AA
54:F8:F0
90:E6:43
98:ED:5C
D4:4F:14
44:59:25
00:0B:4F
60:C7:98
88:BF:35
A4:60:11
E8:05:DC
00:03:81
10:1E:DA
38:EF:E3
44:D4:7F
54:7F:54
54:E1:40
B4:00:16
7C:55:A7
00:1D:00
28:CD:C1
2C:CF:67
88:A2:9E
98:FE:54
B8:27:EB
D8:3A:DD
DC:A6:32
E4:5F:01
```

<div align="center">

**Quick Copy:** Click the copy button in the top-right corner of any code block above

</div>

---

## Contributing

Found a new OUI? Help build the database!

1. **Add to categorized list** with appropriate section

2. **Append to OUI block** (one per line)

3. **Update badge counts** in header

4. **Submit PR** with verification source

---

<div align="center">

**Total OUIs:** `154` | **Last Updated:** Auto-generated from categorized lists

Made by the OUI-SPY community

</div>

