*** Settings ***
Documentation       trainer_fw integration tests (Renode). IDs refer to SPEC.md §9.2.
...                 Checks UART log lines, simulated LEDs and CAN frames on the lane bus.
Resource            common.resource
Library             Collections
Test Setup          Reset Emulation
Test Teardown       Test Teardown

*** Variables ***
${STRESS_MAGS}      3

*** Keywords ***
Wait For Event Type
    [Documentation]    Skip EVENT frames until one of the given type; returns its value field.
    [Arguments]    ${type}    ${node}=1    ${max_frames}=20
    FOR    ${i}    IN RANGE    ${max_frames}
        ${f}=    Wait For Frame    ${FN_EVENT}    ${node}
        IF    ${f}[0] == ${type}
            ${v}=    U16    ${f}    6
            RETURN    ${v}
        END
    END
    Fail    no EVENT of type ${type} in ${max_frames} frames

Strobe Stats
    [Documentation]    The firmware's "trainer strobe" line as a dictionary of integers (v0.6).
    Shell                       trainer strobe
    ${l}=                       Wait For Line On Uart    strobe slots=
    ${d}=                       Evaluate    {k: (1 if v == "yes" else 0 if v == "no" else int(v)) for k, v in re.findall(r"(\\w+)=(-?\\w+)", $l["Line"])}    modules=re
    RETURN    ${d}

On Machine
    [Documentation]    Run a monitor command on one machine of the lane.
    [Arguments]    ${machine}    ${command}
    Execute Command             mach set "${machine}"
    Execute Command             ${command}

Press On
    [Arguments]    ${machine}    ${button}
    On Machine                  ${machine}    sysbus.gpioPortG.${button} Press
    Advance                     0.05
    On Machine                  ${machine}    sysbus.gpioPortG.${button} Release
    Advance                     0.05

Trigger On
    [Arguments]    ${machine}    ${percent}
    On Machine                  ${machine}    sysbus.adcM1S2.potentiometer SetPercentage ${percent}

Reload Both
    [Documentation]    Magazine out, new magazine in, rack, on both weapons.
    FOR    ${m}    IN    weapon1    weapon2
        Press On                ${m}    sw2
        Press On                ${m}    sw2
        Press On                ${m}    sw3
    END

Expect Shot Sequence
    [Documentation]    SHOT_EVENT frames of one node, in order 1..count, on that node's own tester.
    [Arguments]    ${tester}    ${node}    ${count}
    ${id}=                      Can Id    ${FN_SHOT_EVENT}    ${node}
    FOR    ${n}    IN RANGE    1    ${count} + 1
        ${data}=                Wait For Frame With Id    ${id}    timeout=1    testerId=${tester}
        ${b}=                   Evaluate    list($data)
        ${shot}=                U16    ${b}    0
        Should Be Equal As Integers    ${shot}    ${n}    node ${node}: expected shot ${n}, got ${shot}
    END

Load Semi On Current Machine
    [Documentation]    Load Semi for the machine selected with "mach set", watching its own UART tester.
    [Arguments]    ${tester}
    Press                       sw1
    Wait For Line On Uart       SELECTOR SEMI    testerId=${tester}
    Press                       sw2
    Press                       sw3
    Wait For Line On Uart       STATE READY    testerId=${tester}

*** Test Cases ***
IT01 Boot Is Safe And Quiet
    [Documentation]    F03, F14, F16, R04: boots into SAFE, clean BIT, INFO frame, blue LED, no shot outputs.
    Create Trainer
    ${emitter}=                 Create LED Tester    ${EMITTER}
    ${valve}=                   Create LED Tester    ${VALVE}
    ${blue}=                    Create LED Tester    ${LED_BLUE}
    ${red}=                     Create LED Tester    ${LED_RED}
    Start Emulation
    Wait For Line On Uart       READY state=SAFE
    Wait For Line On Uart       BIT mask=0x0000 critical=no
    # The CAN tester consumes every frame up to a match, so wait in send order:
    # BIT_RESULT, then INFO, then the next periodic STATUS.
    ${bit}=                     Wait For Frame    ${FN_BIT_RESULT}
    ${mask}=                    U16    ${bit}    0
    Should Be Equal As Integers    ${mask}    0
    Should Be Equal As Integers    ${bit}[2]    0    not critical
    ${info}=                    Wait For Frame    ${FN_INFO}
    Should Be Equal As Integers    ${info}[0]    0    version major
    Should Be Equal As Integers    ${info}[1]    6    version minor
    ${boots}=                   U32    ${info}    4
    Should Be Equal As Integers    ${boots}    1
    ${status}=                  Wait For Frame    ${FN_STATUS}
    Should Be Equal As Integers    ${status}[0]    0    state SAFE
    Assert And Hold LED State   false    0    0.5    testerId=${emitter}    pauseEmulation=true
    Assert LED State            false    0    testerId=${valve}    pauseEmulation=true
    Assert LED State            true     0    testerId=${blue}     pauseEmulation=true
    Assert LED State            false    0    testerId=${red}      pauseEmulation=true
    Shell                       trainer version
    Wait For Line On Uart       trainer_fw 0.6.0

IT01b Trigger In Safe Does Nothing
    [Documentation]    F03: a full squeeze in SAFE fires nothing, even with a round chambered.
    Boot Trainer
    Press                       sw2
    Press                       sw3
    ${emitter}=                 Create LED Tester    ${EMITTER}
    Set Trigger                 100
    Assert And Hold LED State   false    0    0.2    testerId=${emitter}    pauseEmulation=true
    Shell                       trainer status
    # content= stops Robot reading "state=..." as a named argument.
    Wait For Line On Uart       content=state=SAFE sel=SAFE mag=in mag_rounds=29 chambered=yes total=30 shots=0

IT02 Full Magazine Cycle
    [Documentation]    F04-F06, F09: 30 squeezes fire 30 shots, each reported as SHOT_EVENT, then EMPTY.
    Boot Trainer
    Load Semi
    FOR    ${n}    IN RANGE    1    31
        Squeeze                 SHOT n=${n} rounds=${{30 - ${n}}} sel=SEMI
        ${f}=                   Wait For Frame    ${FN_SHOT_EVENT}
        ${shot_no}=             U16    ${f}    0
        Should Be Equal As Integers    ${shot_no}    ${n}
        Should Be Equal As Integers    ${f}[7]    ${{30 - ${n}}}    rounds_left
        Should Be Equal As Integers    ${{${f}[6] & 3}}    1    mode SEMI
    END
    Wait For Line On Uart       STATE EMPTY
    Squeeze                     DRY_FIRE
    Wait For Event Type         1

IT03 Semi Needs Trigger Reset
    [Documentation]    F02: partial release above the reset threshold does not allow a second shot.
    Boot Trainer
    Load Semi
    Set Trigger                 100
    Wait For Line On Uart       SHOT n=1
    Set Trigger                 60
    Advance                     0.05
    Set Trigger                 100
    Should Not Be On Uart       SHOT n=2    timeout=0.2
    Set Trigger                 0
    Advance                     0.05
    Set Trigger                 100
    Wait For Line On Uart       SHOT n=2

IT04 Auto Cadence
    [Documentation]    F06: AUTO at 750 rpm fires every 80 ms while the trigger is held.
    Boot Trainer
    Load Semi
    Press                       sw1
    Wait For Line On Uart       SELECTOR AUTO
    Set Trigger                 100
    Wait For Line On Uart       SHOT n=1
    ${l2}=                      Wait For Line On Uart    SHOT n=2
    ${l10}=                     Wait For Line On Uart    SHOT n=10
    Set Trigger                 0
    ${t2}=                      Log Time Ms    ${l2}[Line]
    ${t10}=                     Log Time Ms    ${l10}[Line]
    ${period_ms}=               Evaluate    (${t10} - ${t2}) / 8
    Should Be True              79 <= ${period_ms} <= 81    period was ${period_ms} ms
    Should Not Be On Uart       SHOT n=11    timeout=0.3

IT05 Dry Fire
    [Documentation]    F07: magazine in but not racked: DRY_FIRE event on CAN, outputs stay off.
    Boot Trainer
    Press                       sw1
    Press                       sw2
    Wait For Line On Uart       STATE LOADED
    ${emitter}=                 Create LED Tester    ${EMITTER}
    Set Trigger                 100
    Wait For Line On Uart       DRY_FIRE
    Assert LED State            false    0    testerId=${emitter}    pauseEmulation=true
    Wait For Event Type         1

IT06a Stoppage Drill
    [Documentation]    F08 (part): injected stoppage blocks fire; rack-then-tap fails; tap-then-rack clears.
    Boot Trainer
    Load Semi
    # Inject from the shell: a CAN command's ACK wait would consume the
    # STOPPAGE_START event that goes out just before the ACK.
    Shell                       trainer inject
    Wait For Line On Uart       STOPPAGE
    Wait For Event Type         5
    Set Trigger                 100
    Should Not Be On Uart       SHOT n=1    timeout=0.2
    Set Trigger                 0
    Press                       sw3
    Press                       sw4
    Should Not Be On Uart       STOPPAGE_CLEARED    timeout=0.1
    Press                       sw3
    Wait For Line On Uart       STOPPAGE_CLEARED
    Wait For Line On Uart       STATE READY
    ${clear_ms}=                Wait For Event Type    6
    Should Be True              ${clear_ms} > 0

IT06 Stoppage At Round N
    [Documentation]    F08: with STOPPAGE_MODE = at round 5, the 5th pull stops instead of firing;
    ...                after the drill the next pull fires shot 5 (the dud was ejected).
    Boot Trainer
    Cmd Should Succeed          2    9    1
    Cmd Should Succeed          2    10    5
    Load Semi
    FOR    ${n}    IN RANGE    1    5
        Squeeze                 SHOT n=${n}
    END
    Set Trigger                 100
    Wait For Line On Uart       STOPPAGE
    Should Not Be On Uart       SHOT n=5    timeout=0.1
    Set Trigger                 0
    Advance                     0.01
    Press                       sw4
    Press                       sw3
    Wait For Line On Uart       STOPPAGE_CLEARED
    Squeeze                     SHOT n=5 rounds=24
    Squeeze                     SHOT n=6

IT06b Random Stoppage Extremes
    [Documentation]    F08: random mode with p = 1000 per mille stops every round; p = 0 never.
    Boot Trainer
    Cmd Should Succeed          2    9    2
    Cmd Should Succeed          2    11    1000
    Load Semi
    Set Trigger                 100
    Wait For Line On Uart       STOPPAGE
    Set Trigger                 0
    Advance                     0.01
    Cmd Should Succeed          2    11    0
    Press                       sw4
    Press                       sw3
    Wait For Line On Uart       STOPPAGE_CLEARED
    Squeeze                     SHOT n=1

IT06c Late Clearance Is Reported
    [Documentation]    F08: a clearance slower than CLEAR_WINDOW_MS gives a CLEAR_LATE event.
    Boot Trainer
    Cmd Should Succeed          2    12    500
    Load Semi
    Shell                       trainer inject
    Wait For Line On Uart       STOPPAGE
    Advance                     0.8
    Press                       sw4
    Press                       sw3
    Wait For Line On Uart       CLEAR_LATE
    ${ms}=                      Wait For Event Type    10
    Should Be True              ${ms} > 500

IT07 Flinch Is Detected
    [Documentation]    F10: replaying a recording where the muzzle dips just before the shot
    ...                gives a flinch of about 1.2 deg in SHOT_DETAIL.
    Boot Trainer
    Load Semi
    Replay IMU                  flinch
    Advance                     0.55
    Set Trigger                 100
    Wait For Line On Uart       DETAIL n=1
    ${d}=                       Wait For Frame    ${FN_SHOT_DETAIL}
    ${shot}=                    U16    ${d}    0
    Should Be Equal As Integers    ${shot}    1
    ${flinch}=                  I8    ${d}    7
    Should Be True              10 <= ${flinch} <= 14    flinch ${flinch} x0.1 deg

IT07b Steady Aim Has No Flinch
    [Documentation]    F10: a steady recording gives near-zero flinch and muzzle rate.
    Boot Trainer
    Load Semi
    Replay IMU                  steady
    Advance                     0.55
    Set Trigger                 100
    ${d}=                       Wait For Frame    ${FN_SHOT_DETAIL}
    ${flinch}=                  I8    ${d}    7
    Should Be True              -2 <= ${flinch} <= 2    flinch ${flinch} x0.1 deg
    Should Be True              ${d}[5] <= 2    muzzle rate ${d}[5] deg/s

IT07c Shaky Aim Raises Muzzle Rate
    [Documentation]    F10: a wobbly recording gives a much higher 300 ms muzzle rate.
    Boot Trainer
    Load Semi
    Replay IMU                  shaky
    Advance                     0.55
    Set Trigger                 100
    ${d}=                       Wait For Frame    ${FN_SHOT_DETAIL}
    Should Be True              ${d}[5] >= 6    muzzle rate ${d}[5] deg/s

IT07d Cant Is Measured
    [Documentation]    F10: a weapon rolled 10 degrees reports cant = 10.
    Boot Trainer
    Load Semi
    Replay IMU                  canted
    Advance                     0.55
    Set Trigger                 100
    ${d}=                       Wait For Frame    ${FN_SHOT_DETAIL}
    ${cant}=                    I8    ${d}    6
    Should Be True              9 <= ${cant} <= 11    cant ${cant} deg

IT10 Fire Path Timing
    [Documentation]    T01-T03 in virtual time: emitter on <= 1.5 ms after the trigger crosses
    ...                the break point (1 kHz sampling + processing), SHOT_EVENT on the bus within 2 ms.
    Boot Trainer
    Load Semi
    ${emitter}=                 Create LED Tester    ${EMITTER}
    Set Trigger                 100
    Assert LED State            true    0.0015    testerId=${emitter}    pauseEmulation=true
    Wait For Frame              ${FN_SHOT_EVENT}    timeout=0.002

IT10b Shot Timestamps Use Lane Time
    [Documentation]    F12: after a SYNC carrying lane time = virtual time + 5 s, the node locks,
    ...                its offset is about +5 s, and SHOT_EVENT carries lane time: the shot's
    ...                local time (from the log) + the reported offset, within 200 us.
    ...                One SYNC makes the fit a pure offset. Renode delivers tester frames after a
    ...                host-dependent delay of up to ~100 ms, which makes a multi-point drift fit
    ...                meaningless here, so drift and absolute accuracy are proven in the unit
    ...                tests (timesync suite), and IT10c checks the drift bound under that jitter.
    [Tags]    sync
    Boot Trainer
    ${vt}=                      Virtual Time Us
    Send Sync                   ${{${vt} + 5000000}}
    Advance                     0.01
    Shell                       trainer sync
    ${l}=                       Wait For Line On Uart    sync synced=yes
    ${offset}=                  Evaluate    int(re.search(r"offset_us=(-?\\d+)", $l["Line"]).group(1))    modules=re
    Should Be True              4500000 <= ${offset} <= 5100000    offset ${offset} us
    Load Semi
    Set Trigger                 100
    ${s}=                       Wait For Line On Uart    SHOT n=1
    # The SHOT line carries the exact local shot time; log timestamps only have ms resolution.
    ${local_us}=                Evaluate    int(re.search(r"t_local=(\\d+)", $s["Line"]).group(1))    modules=re
    ${f}=                       Wait For Frame    ${FN_SHOT_EVENT}
    ${t}=                       U32    ${f}    2
    ${err}=                     Evaluate    ${t} - ((${local_us} + ${offset}) & 0xFFFFFFFF)
    Log To Console              LOG: offset ${offset} us, lane timestamp vs local+offset ${err} us
    Should Be True              abs(${err}) <= 2    timestamp error ${err} us

IT10c Jittery SYNC Keeps Drift Bounded
    [Documentation]    F12 robustness: 40 SYNC frames with Renode's large delivery jitter still
    ...                give a locked fit whose drift stays inside the +-500 ppm bound.
    [Tags]    sync
    Boot Trainer
    FOR    ${i}    IN RANGE    40
        ${vt}=                  Virtual Time Us
        Send Sync               ${{${vt} + 5000000}}
        Advance                 0.01
    END
    Shell                       trainer sync
    ${l}=                       Wait For Line On Uart    sync synced=yes
    ${ppb}=                     Evaluate    int(re.search(r"drift_ppb=(-?\\d+)", $l["Line"]).group(1))    modules=re
    Log To Console              LOG: drift under jitter ${ppb} ppb
    Should Be True              abs(${ppb}) <= 500000    drift ${ppb} ppb

IT08 Config Persists Across Reboot
    [Documentation]    F13: SET over CAN is saved to flash and survives a reboot command.
    Boot Trainer
    Cmd Should Succeed          2    2    20
    ${v}=                       Cmd Should Succeed    1    2
    Should Be Equal As Integers    ${v}    20
    Cmd Should Succeed          5
    Wait For Line On Uart       NODE id=1 boot=2
    ${v}=                       Cmd Should Succeed    1    2
    Should Be Equal As Integers    ${v}    20    capacity after reboot
    ${boots}=                   Cmd Should Succeed    1    8
    Should Be Equal As Integers    ${boots}    2
    Load Semi
    Shell                       trainer status
    Wait For Line On Uart       content=total=20

IT08b Corrupt Config Falls Back To Defaults
    [Documentation]    R03: a stored config with a bad CRC gives defaults and the CFG_DEFAULTED bit.
    Boot Trainer
    Cmd Should Succeed          2    3    900
    Shell                       trainer cfg corrupt
    Wait For Line On Uart       cfg corrupted
    Cmd Should Succeed          5
    Wait For Line On Uart       stored config corrupt, defaults restored
    Wait For Line On Uart       BIT mask=0x0010 critical=no
    # The reboot ACK wait cleared the tester queue, so this is the new boot's BIT_RESULT.
    ${bit}=                     Wait For Frame    ${FN_BIT_RESULT}
    ${mask}=                    U16    ${bit}    0
    Should Be Equal As Integers    ${mask}    16    CFG_DEFAULTED after the reboot
    ${rpm}=                     Cmd Should Succeed    1    3
    Should Be Equal As Integers    ${rpm}    750    back to the default

IT08c Command Errors
    [Documentation]    F13: invalid commands are rejected with the right status and change nothing.
    Boot Trainer
    ${st}    ${v}=              Send Cmd    1    99
    Should Be Equal As Integers    ${st}    2    BAD_KEY
    ${st}    ${v}=              Send Cmd    2    3    1200
    Should Be Equal As Integers    ${st}    3    BAD_VALUE
    ${st}    ${v}=              Send Cmd    2    8    5
    Should Be Equal As Integers    ${st}    5    READ_ONLY
    ${st}    ${v}=              Send Cmd    127
    Should Be Equal As Integers    ${st}    1    BAD_OPCODE
    ${rpm}=                     Cmd Should Succeed    1    3
    Should Be Equal As Integers    ${rpm}    750

IT09a Two Weapons On One Bus
    [Documentation]    F09: two nodes boot with their own IDs; a shot on node 2 is reported on node 2's ID.
    Execute Command             $elf=@${ELF}
    Execute Command             include @${SIM_DIR}/lane.resc
    ${u1}=                      Create Terminal Tester    ${UART}    machine=weapon1    defaultPauseEmulation=True
    ${u2}=                      Create Terminal Tester    ${UART}    machine=weapon2    defaultPauseEmulation=True
    Create CAN Testers
    Start Emulation
    Wait For Line On Uart       NODE id=1    testerId=${u1}
    Wait For Line On Uart       NODE id=2    testerId=${u2}
    # Both INFO frames go out at the same moment in either order, and the CAN
    # tester consumes frames up to a match, so the IDs are checked on the UARTs.
    Execute Command             mach set "weapon2"
    Load Semi On Current Machine    ${u2}
    Set Trigger                 100
    Wait For Line On Uart       SHOT n=1    testerId=${u2}
    ${f}=                       Wait For Frame    ${FN_SHOT_EVENT}    2
    ${shot_no}=                 U16    ${f}    0
    Should Be Equal As Integers    ${shot_no}    1

IT09b Two Weapons Full Auto Without Frame Loss
    [Documentation]    T05 (scaled): both weapons fire AUTO at 900 rpm, ${STRESS_MAGS} magazines of
    ...                100 rounds each. Node 2's SHOT_EVENT stream arrives complete and in order
    ...                while node 1 shares the bus, and both nodes report every shot and no TX
    ...                overflow. (Renode allows one CAN tester per hub and a wait consumes all
    ...                frames before its match, so only one node's stream can be read in full.)
    ...                Run with --variable STRESS_MAGS:100 for SPEC T05's 10 000 shots each.
    [Tags]    stress
    Execute Command             $elf=@${ELF}
    Execute Command             include @${SIM_DIR}/lane.resc
    ${u1}=                      Create Terminal Tester    ${UART}    machine=weapon1    defaultPauseEmulation=True
    ${u2}=                      Create Terminal Tester    ${UART}    machine=weapon2    defaultPauseEmulation=True
    Create CAN Testers
    Start Emulation
    Wait For Line On Uart       NODE id=1    testerId=${u1}
    Wait For Line On Uart       NODE id=2    testerId=${u2}
    FOR    ${node}    IN    1    2
        Cmd Should Succeed      2    2    100    ${node}
        Cmd Should Succeed      2    3    900    ${node}
    END
    FOR    ${m}    IN    weapon1    weapon2
        Press On                ${m}    sw1
        Press On                ${m}    sw1
        Press On                ${m}    sw2
        Press On                ${m}    sw3
    END
    FOR    ${mag}    IN RANGE    ${STRESS_MAGS}
        Trigger On              weapon1    100
        Trigger On              weapon2    100
        # 100 rounds at 900 rpm = 6.6 s; both run dry
        Advance                 7.2
        Trigger On              weapon1    0
        Trigger On              weapon2    0
        Advance                 0.05
        Reload Both
    END
    ${total}=                   Evaluate    ${STRESS_MAGS} * 100
    Expect Shot Sequence        ${CAN_RX}    2    ${total}
    FOR    ${u}    IN    ${u1}    ${u2}
        Write Line To Uart      trainer status    testerId=${u}
        Wait For Line On Uart   content=shots=${total}    testerId=${u}
        Write Line To Uart      trainer can    testerId=${u}
        Wait For Line On Uart   content=tx_overflow=0    testerId=${u}
    END

IT11 Watchdog Resets A Stalled Thread
    [Documentation]    R01: a control thread that stops feeding its watchdog channel causes a reset.
    Boot Trainer
    Shell                       trainer stall control
    Wait For Line On Uart       WATCHDOG thread=control stalled    timeout=3
    Wait For Line On Uart       NODE id=1 boot=2    timeout=3

IT11b Watchdog Covers The Trigger Thread
    [Documentation]    R01: same for the trigger sampling thread.
    Boot Trainer
    Shell                       trainer stall trigger
    Wait For Line On Uart       WATCHDOG thread=trigger stalled    timeout=3
    Wait For Line On Uart       NODE id=1 boot=2    timeout=3

IT12 Missing IMU Is A Critical Fault
    [Documentation]    F14: with the IMU absent BIT reports a critical fault; the weapon is in FAULT and cannot fire.
    Create Trainer
    Execute Command             machine UnregisterFromParent sysbus.i2c1.lsm6dso
    ${red}=                     Create LED Tester    ${LED_RED}
    Start Emulation
    Wait For Line On Uart       STATE FAULT
    ${bit}=                     Wait For Frame    ${FN_BIT_RESULT}
    ${mask}=                    U16    ${bit}    0
    Should Be Equal As Integers    ${{${mask} & 4}}    4    IMU bit
    Should Be Equal As Integers    ${bit}[2]    1    critical
    Assert LED State            true    0.2    testerId=${red}    pauseEmulation=true
    Press                       sw2
    Press                       sw3
    Set Trigger                 100
    Should Not Be On Uart       SHOT n=1    timeout=0.3
    ${again}=                   Cmd Should Succeed    3
    Should Be Equal As Integers    ${{${again} & 4}}    4    RUN_BIT still sees the fault

IT14 Shot Output Pulses
    [Documentation]    F05: one shot pulses the emitter gate (2 ms) and the recoil valve gate (20 ms).
    Boot Trainer
    Load Semi
    ${emitter}=                 Create LED Tester    ${EMITTER}
    ${valve}=                   Create LED Tester    ${VALVE}
    Set Trigger                 100
    Assert LED State            true     0.1      testerId=${emitter}    pauseEmulation=true
    # The emitter edge pauses the emulation one instruction before the valve is set.
    Assert LED State            true     0.001    testerId=${valve}      pauseEmulation=true
    Assert LED State            false    0.004    testerId=${emitter}    pauseEmulation=true
    Assert And Hold LED State   true     0        0.012    testerId=${valve}    pauseEmulation=true
    Assert LED State            false    0.012    testerId=${valve}      pauseEmulation=true

IT14b Pulse Width Follows Config
    [Documentation]    F13: a new valve pulse width set over CAN applies to the next shot.
    Boot Trainer
    Cmd Should Succeed          2    5    35
    Load Semi
    ${valve}=                   Create LED Tester    ${VALVE}
    Set Trigger                 100
    Assert LED State            true     0.1      testerId=${valve}      pauseEmulation=true
    Assert And Hold LED State   true     0        0.030    testerId=${valve}    pauseEmulation=true
    Assert LED State            false    0.010    testerId=${valve}      pauseEmulation=true

IT16 Shot Log On The Device
    [Documentation]    F15: the last shots are kept on the device and dumped by `trainer log n`,
    ...                oldest first, with the same lane timestamps as the SHOT_EVENT frames;
    ...                `trainer log clear` empties it.
    [Tags]    shotlog
    Boot Trainer
    Load Semi
    FOR    ${n}    IN RANGE    1    4
        Squeeze                 SHOT n=${n}
    END
    ${times}=                   Create List
    FOR    ${n}    IN RANGE    1    4
        ${f}=                   Wait For Frame    ${FN_SHOT_EVENT}
        ${t}=                   U32    ${f}    2
        Append To List          ${times}    ${t}
    END
    Shell                       trainer log 2
    Wait For Line On Uart       content=log count=3 total=3 showing=2
    ${l2}=                      Wait For Line On Uart    log #2
    ${l3}=                      Wait For Line On Uart    log #3
    Should Contain              ${l2}[Line]    t_lane=${times}[1]
    Should Contain              ${l3}[Line]    t_lane=${times}[2]
    Should Contain              ${l3}[Line]    mode=SEMI left=27
    Shell                       trainer log clear
    Wait For Line On Uart       log cleared
    Shell                       trainer log
    Wait For Line On Uart       content=log count=0 total=0

IT15 Status LED Patterns
    [Documentation]    F16: SAFE blue, EMPTY slow green blink, READY steady green.
    Boot Trainer
    ${green}=                   Create LED Tester    ${LED_GREEN}
    Press                       sw1
    Wait For Line On Uart       STATE EMPTY
    Assert LED Is Blinking      2.0    0.5    0.5    tolerance=0.1    testerId=${green}    pauseEmulation=true
    Press                       sw2
    Press                       sw3
    Wait For Line On Uart       STATE READY
    Assert And Hold LED State   true    0.1    0.5    testerId=${green}    pauseEmulation=true

IT17 Camera Mode Strobes In The Weapon's Slot
    [Documentation]    F17 (v0.6): with STROBE_SLOTS = 2 and STROBE_SLOT = 0 set over CAN, the
    ...                emitter flashes 0.5 ms in the middle of every other camera exposure, on the
    ...                node's own lane time: about 10 pulses in 0.2 s, each one starting 4.75 ms
    ...                after a 20 ms lane-time boundary, within 250 us (then the 0.5 ms pulse is
    ...                still fully inside the 1 ms exposure). Direct mode (the default, IT10 and
    ...                IT14) is unchanged.
    [Tags]    sync    strobe
    Boot Trainer
    ${vt}=                      Virtual Time Us
    Send Sync                   ${{${vt} + 5000000}}
    Advance                     0.01
    Cmd Should Succeed          2    14    0
    Cmd Should Succeed          2    13    2
    ${emitter}=                 Create LED Tester    ${EMITTER}
    Assert LED State            true     0.025    testerId=${emitter}    pauseEmulation=true
    Assert LED State            false    0.001    testerId=${emitter}    pauseEmulation=true
    Advance                     0.2
    ${s}=                       Strobe Stats
    Log To Console              LOG: strobe ${s}
    Should Be True              ${s}[pulses] >= 9    ${s}
    Should Be True              ${s}[aim] == ${s}[pulses]
    Should Be True              ${s}[last_k] % 2 == 0    slot 0: even exposures
    ${phase}=                   Evaluate    ${s}[last_lane] % 20000
    Should Be True              4500 <= ${phase} <= 5000    pulse start ${phase} us into the 20 ms cycle
    Should Be True              ${s}[max_err_us] <= 250    worst pulse timing error ${s}[max_err_us] us

IT17b Shot Pulse Goes Into The Shot Frame
    [Documentation]    F17: in camera mode the recoil valve fires at the break (T02 unchanged),
    ...                and the emitter's shot pulse goes into the first exposure whose pulse starts
    ...                at least 2 ms after the break time the SHOT_EVENT carries (the tracker's rule).
    [Tags]    sync    strobe
    Boot Trainer
    ${vt}=                      Virtual Time Us
    Send Sync                   ${{${vt} + 5000000}}
    Advance                     0.01
    Cmd Should Succeed          2    14    1
    Cmd Should Succeed          2    13    2
    Load Semi
    ${valve}=                   Create LED Tester    ${VALVE}
    Set Trigger                 100
    Assert LED State            true     0.1      testerId=${valve}      pauseEmulation=true
    ${f}=                       Wait For Frame    ${FN_SHOT_EVENT}
    ${t}=                       U32    ${f}    2
    Advance                     0.05
    ${s}=                       Strobe Stats
    Log To Console              LOG: break at lane ${t} us, strobe ${s}
    ${k}=                       Evaluate    -((-(${t} + 2000 - 4750)) // 10000)
    Should Be Equal As Integers    ${s}[shot_k]    ${k}
    Should Be Equal As Integers    ${s}[shot_fired_k]    ${k}
    Should Be Equal As Integers    ${s}[shots]    1
    Should Be Equal As Integers    ${s}[late]    0
    ${start}=                   Evaluate    ${k} * 10000 + 4750
    Should Be True              abs(${s}[shot_lane] - ${start}) <= 250    shot pulse at ${s}[shot_lane], planned ${start}
    Should Be True              ${s}[shot_lane] - ${t} >= 2000    at least 2 ms after the break

IT17c Strobe Settings Are Checked
    [Documentation]    F13/F17: one slot is refused (the tracker needs a dark frame between a
    ...                weapon's flashes), so is a slot outside the slot count; both read back.
    [Tags]    strobe
    Boot Trainer
    ${st}    ${v}=              Send Cmd    2    13    1
    Should Be Equal As Integers    ${st}    3
    Cmd Should Succeed          2    13    3
    ${st}    ${v}=              Send Cmd    2    14    3
    Should Be Equal As Integers    ${st}    3
    Cmd Should Succeed          2    14    2
    ${v}=                       Cmd Should Succeed    1    13
    Should Be Equal As Integers    ${v}    3
    ${v}=                       Cmd Should Succeed    1    14
    Should Be Equal As Integers    ${v}    2
    Cmd Should Succeed          2    13    0
