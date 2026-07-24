# JLCPCB Fab Order Settings

Recorded JLCPCB order configuration for the GameBoy HiFi boards, so reorders stay
consistent. The gerbers, BOM, and CPL come from the export in
[HARDWARE.md → Ordering the boards](HARDWARE.md#ordering-the-boards); this file
captures the fab-side options you pick in the JLCPCB order form after upload.

Most settings are board-agnostic (material, finish, stackup, coverlay, stiffener)
and carry across revisions. The **dimension** and **gerber file name** change per
revision — update those to match the package you are uploading.

## Flex — agb-hifi-audio-fpc

The flex is the fiddly one: a 2-layer polyimide flex with ENIG gold-finger
contacts, a white coverlay, and a polyimide stiffener under the connector lead.
The values below were captured from an order of this board.

### Board & material

| Setting         | Value                      |
| --------------- | -------------------------- |
| Base Material   | Flex                       |
| Layers          | 2                          |
| PCB Thickness   | 0.11 mm                    |
| Substrate Type  | 25 µm dielectric thickness |
| Specify Stackup | No                         |
| Layer Sequence  | (default)                  |

### Copper & finish

| Setting             | Value                      |
| ------------------- | -------------------------- |
| Copper Type         | Electro-deposited          |
| Outer Copper Weight | 1/3 oz                     |
| Surface Finish      | ENIG (gold thickness 1 U") |
| Gold Fingers        | Yes                        |
| Gold Fingers bevel  | 0.3 mm                     |

### Coverlay & silkscreen

| Setting                 | Value                 |
| ----------------------- | --------------------- |
| Coverlay Color          | White                 |
| Coverlay Thickness      | PI 12.5 µm / AD 15 µm |
| Silkscreen              | Black                 |
| Silkscreen on Stiffener | No                    |

### Stiffener

| Setting               | Value     |
| --------------------- | --------- |
| Stiffener             | Polyimide |
| Polyimide Thickness   | 0.225 mm  |
| PI Stiffener Lift Tab | No        |

### EMI shielding

| Setting            | Value   |
| ------------------ | ------- |
| EMI Shielding Film | Without |

## Main board — agb-hifi-audio-pcb

4-layer rigid FR-4, assembled (JLCPCB places most parts, saving the QFN reflow).
Settings not yet captured — fill in from the order form when the main board is
next ordered.
