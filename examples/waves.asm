; WAVES - layered interference ribbons
; Four cheap wave bands plus crossing phase offsets and a horizon grid.
.org $8000

start:
    LDA #$00
    STA phase
    STA lastFrame
main:
    JSR waitFrame
    JSR clearVRAM
    JSR drawWaves
    INC phase
    JMP main

waitFrame:
    LDA $12F6
    CMP lastFrame
    BEQ waitFrame
    STA lastFrame
    RTS

drawWaves:
    LDX #$00
wl:
    TXA
    CLC
    ADC phase
    AND #$1F
    TAY
    LDA wave,Y
    CLC
    ADC #$08
    STA py
    STX px
    LDA #$0F
    STA color
    JSR plotPixel
    TXA
    CLC
    ADC phase
    ADC #$08
    AND #$1F
    TAY
    LDA wave,Y
    CLC
    ADC #$1A
    STA py
    STX px
    LDA #$0A
    STA color
    JSR plotPixel
    TXA
    CLC
    ADC phase
    ADC #$10
    AND #$1F
    TAY
    LDA wave,Y
    CLC
    ADC #$2C
    STA py
    STX px
    LDA #$05
    STA color
    JSR plotPixel
    TXA
    CLC
    ADC phase
    ADC #$18
    AND #$1F
    TAY
    LDA wave,Y
    CLC
    ADC #$3B
    STA py
    STX px
    LDA #$0E
    STA color
    JSR plotPixel
    INX
    CPX #$40
    BNE wl
    ; horizon line
    LDA #$02
    STA color
    LDX #$00
h:
    STX px
    LDA #$20
    STA py
    JSR plotPixel
    INX
    CPX #$40
    BNE h
    RTS

wave:
.byte 8,9,11,13,15,17,19,20,19,17,15,13,11,9,8,7,7,8,10,12,14,16,18,19,18,16,14,12,10,9,8,8
phase: .byte 0
lastFrame: .byte 0
px: .byte 0
py: .byte 0
color: .byte 0

clearVRAM:
    LDA #$00
    STA $00
    LDA #$02
    STA $01
    LDX #$10
    LDY #$00
clr:
    STA ($00),Y
    INY
    BNE clr
    INC $01
    DEX
    BNE clr
    RTS

plotPixel:
    LDA py
    ASL A
    ASL A
    ASL A
    ASL A
    ASL A
    ASL A
    STA $00
    LDA py
    LSR A
    LSR A
    CLC
    ADC #$02
    STA $01
    LDY px
    LDA color
    STA ($00),Y
    RTS

.org $FFFC
.word start
