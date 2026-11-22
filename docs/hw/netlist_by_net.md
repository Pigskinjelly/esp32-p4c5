# v1 网表（按网络分组）

> 由 `_tools/gen_hw.py` 生成，与 `netlist_v1.csv` 同源；`NC` 引脚已剔除。

> IC 引脚按**引脚名**给出（具体脚号以数据手册为准），连接器/阻容按脚号给出。

**`3V3_EXT`**（4 点）：J6.1、R10.2、R8.2、R9.2

**`ALRT_N`**（3 点）：J6.6、R10.1、U4.ALRT

**`BAT_C_N`**（4 点）：BT1.2、C7.2、Q1.S1、U3.VSS

**`BAT_C_P`**（2 点）：BT1.1、F2.1

**`BAT_MID`**（2 点）：Q1.D1、Q1.S2

**`BAT_P`**（8 点）：C6.1、C8.1、F2.2、R5.1、TP3.1、U2.BAT、U4.VDD、U4.CELL

**`BST_5V`**（7 点）：C10.1、C14.2、Q3.S、R12.1、TP4.1、U5.VOUT、U7.ANODE

**`BTST`**（2 点）：C3.1、U2.BTST

**`CC1`**（3 点）：J1.A5、R1.1、U1.IO1

**`CC2`**（3 点）：J1.B5、R2.1、U1.IO2

**`COMP_BST`**（2 点）：R14.1、U5.COMP

**`FB_BST`**（3 点）：R12.2、R13.1、U5.FB

**`FET_CO`**（2 点）：Q1.G2、U3.CO

**`FET_DO`**（2 点）：Q1.G1、U3.DO

**`GATE_BST`**（2 点）：Q3.G、U7.GATE

**`GATE_USB`**（2 点）：Q2.G、U6.GATE

**`GND`**（44 点）：C1.2、C10.2、C11.2、C12.2、C15.2、C2.2、C4.2、C5.2、C6.2、C8.2、C9.2、D1.2、D2.2、J1.A1、J1.A12、J1.B1、J1.B12、J1.SH1、J1.SH2、J5.3、J5.4、LED2.K、NTC1.2、Q1.D2、R1.2、R11.2、R13.2、R17.2、R2.2、R3.2、R4.2、R6.2、TP6.1、U1.GND、U2.CE、U2.GND、U2.EP、U4.GND、U4.QSTRT、U5.MODE、U5.GND、U5.EP、U6.GND、U7.GND

**`ICHG`**（2 点）：R4.1、U2.ICHG

**`ILIM`**（2 点）：R3.1、U2.ILIM

**`ILIM_BST`**（2 点）：R11.1、U5.ILIM

**`PG_N`**（4 点）：R15.2、R16.1、U2.PG、U5.EN

**`PG_STAT`**（3 点）：J6.5、R16.2、R17.1

**`PMID`**（2 点）：C2.1、U2.PMID

**`PROT_VDD`**（3 点）：C7.1、R5.2、U3.VDD

**`PROT_VM`**（2 点）：R6.1、U3.VM

**`PWR_LED`**（2 点）：LED2.A、R20.2

**`RC_COMP`**（2 点）：C12.1、R14.2

**`REGN`**（3 点）：C5.1、R7.1、U2.REGN

**`SCL`**（3 点）：J6.3、R9.1、U4.SCL

**`SDA`**（3 点）：J6.2、R8.1、U4.SDA

**`SS_BST`**（2 点）：C11.1、U5.SS

**`STAT_3V3`**（2 点）：J6.4、R18.2

**`STAT_LED`**（2 点）：LED1.A、R19.2

**`STAT_N`**（3 点）：LED1.K、R18.1、U2.STAT

**`SW_BST`**（2 点）：L2.2、U5.SW

**`SW_CHG`**（3 点）：C3.2、L1.1、U2.SW

**`SYS_5V`**（10 点）：C15.1、D2.1、J5.1、J5.2、Q2.D、Q3.D、R20.1、TP5.1、U6.CATHODE、U7.CATHODE

**`SYS_V`**（9 点）：C4.1、C9.1、L1.2、L2.1、R15.1、R19.1、TP2.1、U2.SYS、U5.VIN

**`TS_NET`**（3 点）：NTC1.1、R7.2、U2.TS

**`USB_DN`**（3 点）：J1.A7、J1.B7、U2.D-

**`USB_DP`**（3 点）：J1.A6、J1.B6、U2.D+

**`VBUS_IN`**（9 点）：C1.1、C13.2、D1.1、F1.2、Q2.S、TP1.1、U1.VBUS、U2.VBUS、U6.ANODE

**`VBUS_TC`**（5 点）：F1.1、J1.A4、J1.A9、J1.B4、J1.B9

**`VC_BST`**（2 点）：C14.1、U7.VC

**`VC_USB`**（2 点）：C13.1、U6.VC
