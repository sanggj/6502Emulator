; PLASMA - animated multi-wave plasma with 16-color palette
; 32-entry wave table; full-screen 64x64 animation.
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
    STX rowTmp
    LDY #$00
col:
    TYA
    CLC
    ADC phase
    AND #$1F
    TAX
    LDA wave,X
    STA tmp
    LDA rowTmp
    CLC
    ADC phase
    AND #$1F
    TAX
    LDA wave,X
    CLC
    ADC tmp
    ADC rowTmp
    AND #$0F
    STA ($00),Y
    INY
    CPY #$40
    BNE col
    LDA $00
    CLC
    ADC #$40
    STA $00
    BCC nextRow
    INC $01
nextRow:
    LDX rowTmp
    INX
    CPX #$40
    BNE row
    RTS

wave:
.byte 0,1,2,4,6,8,10,12,14,15,14,13,11,9,7,5,3,1,0,1,3,5,7,9,11,13,14,15,13,9,5,2
phase: .byte 0
lastFrame: .byte 0
rowTmp: .byte 0
tmp: .byte 0

.org $FFFC
.word start
