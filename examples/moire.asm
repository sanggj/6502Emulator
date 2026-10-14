; MOIRE - animated interference / moire pattern
; Dense full-screen pattern using only shifts, XOR and phase.
.org $8000

start:
    LDA #$00
    STA phase
    STA lastFrame
main:
    JSR waitFrame
    JSR draw
    INC phase
    JMP main

waitFrame:
    LDA $12F6
    CMP lastFrame
    BEQ waitFrame
    STA lastFrame
    RTS

draw:
    LDA #$00
    STA $00
    LDA #$02
    STA $01
    LDX #$00
row:
    LDY #$00
col:
    STY tmp
    TXA
    ASL A
    EOR tmp
    CLC
    ADC phase
    AND #$0F
    STA value
    TYA
    LSR A
    EOR phase
    AND #$0F
    EOR value
    STA ($00),Y
    INY
    CPY #$40
    BNE col
    LDA $00
    CLC
    ADC #$40
    STA $00
    BCC next
    INC $01
next:
    INX
    CPX #$40
    BNE row
    RTS

phase: .byte 0
lastFrame: .byte 0
tmp: .byte 0
value: .byte 0

.org $FFFC
.word start
