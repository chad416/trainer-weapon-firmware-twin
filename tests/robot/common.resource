*** Settings ***
Documentation       Shared setup for trainer_fw integration tests in Renode.
Resource            ${RENODEKEYWORDS}

*** Variables ***
# Override from the command line: --variable ELF:/path/to/zephyr.elf
${ELF}              /root/trainer-ws/build/app/zephyr/zephyr.elf
${SIM_DIR}          ${CURDIR}/../../sim
${UART}             sysbus.usart2
${LED_RED}          sysbus.gpioPortD.redRgbLed
${LED_GREEN}        sysbus.gpioPortD.greenRgbLed
${LED_BLUE}         sysbus.gpioPortD.blueRgbLed
${EMITTER}          sysbus.gpioPortE.emitterGate
${VALVE}            sysbus.gpioPortE.valveGate

# CAN function codes (protocol/trainer.dbc): ID = (function << 7) | node
${FN_SYNC}          1
${FN_SHOT_EVENT}    2
${FN_SHOT_DETAIL}   3
${FN_STATUS}        4
${FN_EVENT}         5
${FN_BIT_RESULT}    6
${FN_CMD}           7
${FN_CMD_ACK}       8
${FN_INFO}          9

*** Keywords ***
Create CAN Testers
    ${rx}=    Create CAN Tester    canHub    defaultTimeout=2
    Set Test Variable    ${CAN_RX}    ${rx}

Create Trainer
    Execute Command             $elf=@${ELF}
    Execute Command             include @${SIM_DIR}/single.resc
    Create Terminal Tester      ${UART}    defaultPauseEmulation=True
    Create CAN Testers

Boot Trainer
    Create Trainer
    Start Emulation
    Wait For Line On Uart       READY state=SAFE

Advance
    [Documentation]    Run the emulation for a fixed virtual time. Pauses first, because
    ...                some testers (Should Not Be On Uart) leave the emulation running.
    [Arguments]    ${seconds}
    Execute Command             pause
    Execute Command             emulation RunFor "${seconds}"

Press
    [Documentation]    Hold the button longer than the gpio-keys debounce (30 ms).
    [Arguments]    ${button}
    Execute Command             sysbus.gpioPortG.${button} Press
    Advance                     0.05
    Execute Command             sysbus.gpioPortG.${button} Release
    Advance                     0.05

Set Trigger
    [Arguments]    ${percent}
    Execute Command             sysbus.adcM1S2.potentiometer SetPercentage ${percent}

Squeeze
    [Documentation]    Full trigger pull and release. Waits for the given line after the pull.
    [Arguments]    ${expect}
    Set Trigger                 100
    Wait For Line On Uart       ${expect}
    Set Trigger                 0
    Advance                     0.01

Shell
    [Arguments]    ${command}
    Write Line To Uart          ${command}

Load Semi
    [Documentation]    SAFE -> SEMI, magazine in, rack: READY with a full magazine.
    Press                       sw1
    Wait For Line On Uart       SELECTOR SEMI
    Press                       sw2
    Press                       sw3
    Wait For Line On Uart       STATE READY sel=SEMI mag=in

Log Time Ms
    [Documentation]    Firmware time of a Zephyr log line "[hh:mm:ss.mmm,uuu] ..." in ms.
    ...                Uses the firmware's own clock, not the time the line reached the UART.
    [Arguments]    ${line}
    ${m}=    Evaluate    re.search(r"\\[(\\d+):(\\d+):(\\d+)\\.(\\d+),(\\d+)\\]", $line)    modules=re
    Should Not Be Equal    ${m}    ${None}    no log timestamp in: ${line}
    ${ms}=   Evaluate    ((int($m.group(1)) * 60 + int($m.group(2))) * 60 + int($m.group(3))) * 1000 + int($m.group(4)) + int($m.group(5)) / 1000
    RETURN    ${ms}

# ---- CAN helpers -------------------------------------------------------------

Can Id
    [Arguments]    ${function}    ${node}=1
    ${id}=    Evaluate    (int(${function}) << 7) | int(${node})
    RETURN    ${id}

Wait For Frame
    [Documentation]    Next frame with this function code from the node, as a list of byte values.
    [Arguments]    ${function}    ${node}=1    ${timeout}=2
    ${id}=      Can Id    ${function}    ${node}
    ${data}=    Wait For Frame With Id    ${id}    timeout=${timeout}    testerId=${CAN_RX}
    ${bytes}=   Evaluate    list($data)
    RETURN    ${bytes}

U16
    [Arguments]    ${bytes}    ${offset}
    ${v}=    Evaluate    $bytes[${offset}] | ($bytes[${offset} + 1] << 8)
    RETURN    ${v}

U32
    [Arguments]    ${bytes}    ${offset}
    ${v}=    Evaluate    int.from_bytes(bytes($bytes[${offset}:${offset} + 4]), "little")
    RETURN    ${v}

Send Cmd
    [Documentation]    CMD as an ISO-TP single frame; returns (status, value) from CMD_ACK.
    [Arguments]    ${opcode}    ${key}=0    ${value}=0    ${node}=1
    ${cmd_id}=     Can Id    ${FN_CMD}    ${node}
    ${ack_id}=     Can Id    ${FN_CMD_ACK}    ${node}
    ${payload}=    Evaluate    bytes([int(${opcode}), int(${key})]).hex() + int(${value}).to_bytes(4, "little").hex()
    Send ISOTP Message    ${cmd_id}    ${ack_id}    ${payload}    testerId=${CAN_RX}
    # Renode 1.17 CANTester race: SendFrame resumes a paused emulation, so the ACK
    # can arrive after the wait has searched its queue but before it registers its
    # filter. That wait times out with the ACK left in the queue, so a second short
    # wait finds it. If the firmware never answered, the second wait fails too.
    ${ok}    ${ack}=    Run Keyword And Ignore Error
    ...            Wait For ISOTP Message Hex    ${cmd_id}    ${ack_id}    testerId=${CAN_RX}
    IF    '${ok}' == 'FAIL'
        Log    CAN tester race: ACK picked up by the second wait    WARN
        ${ack}=    Wait For ISOTP Message Hex    ${cmd_id}    ${ack_id}    timeout=0.1    testerId=${CAN_RX}
    END
    ${b}=          Evaluate    list(bytes.fromhex($ack))
    Should Be Equal As Integers    ${b}[0]    ${opcode}    ACK for the wrong opcode: ${ack}
    ${status}=     Set Variable    ${b}[2]
    ${v}=          U32    ${b}    3
    RETURN    ${status}    ${v}

Cmd Should Succeed
    [Arguments]    ${opcode}    ${key}=0    ${value}=0    ${node}=1
    ${status}    ${v}=    Send Cmd    ${opcode}    ${key}    ${value}    ${node}
    Should Be Equal As Integers    ${status}    0    CMD ${opcode}/${key} failed with status ${status}
    RETURN    ${v}

Virtual Time Us
    [Documentation]    Elapsed virtual time of the current machine, in microseconds.
    ${s}=     Execute Command    machine ElapsedVirtualTime
    ${m}=     Evaluate    re.search(r"(\\d+):(\\d+):(\\d+)\\.(\\d+)", $s)    modules=re
    Should Not Be Equal    ${m}    ${None}    cannot parse virtual time: ${s}
    ${us}=    Evaluate    ((int($m.group(1)) * 60 + int($m.group(2))) * 60 + int($m.group(3))) * 1000000 + int(($m.group(4) + "000000")[:6])
    RETURN    ${us}

Send Sync
    [Documentation]    SYNC frame from the lane (ISO-TP single frame) carrying lane_time_us.
    [Arguments]    ${lane_time_us}
    ${id}=         Can Id    ${FN_SYNC}    0
    ${payload}=    Evaluate    (int(${lane_time_us}) & 0xFFFFFFFF).to_bytes(4, "little").hex()
    Send ISOTP Message    ${id}    0x7FF    ${payload}    testerId=${CAN_RX}

I8
    [Arguments]    ${bytes}    ${offset}
    ${v}=    Evaluate    $bytes[${offset}] - 256 if $bytes[${offset}] > 127 else $bytes[${offset}]
    RETURN    ${v}

Replay IMU
    [Documentation]    Start replaying an IMU recording (sim/data/imu_<profile>.resd) from now.
    [Arguments]    ${profile}
    ${f}=    Set Variable    ${SIM_DIR}/data/imu_${profile}.resd
    Execute Command    sysbus.i2c1.lsm6dso FeedAccelerationSamplesFromRESD @${f} 0 0 CurrentVirtualTime
    Execute Command    sysbus.i2c1.lsm6dso FeedAngularRateSamplesFromRESD @${f} 0 0 CurrentVirtualTime
