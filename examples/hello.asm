; HELLO - unmistakable HI splash screen
; Large centered block letters on a dark field. Runs once and stops.
.org $8000

start:
    JSR clearVRAM
    LDA #$0F
    STA color
    ; H
    LDA #$0A
    STA x0
    LDA #$10
    STA y0
    LDA #$05
    STA w
    LDA #$2C
    STA h
    JSR rectFill
    LDA #$25
    STA x0
    JSR rectFill
    LDA #$0A
    STA x0
    LDA #$24
    STA y0
    LDA #$20
    STA w
    LDA #$05
    STA h
    JSR rectFill
    ; I
    LDA #$30
    STA x0
    LDA #$10
    STA y0
    LDA #$16
    STA w
    LDA #$05
    STA h
    JSR rectFill
    LDA #$38
    STA x0
    LDA #$15
    STA y0
    LDA #$05
    STA w
    LDA #$22
    STA h
    JSR rectFill
    LDA #$30
    STA x0
    LDA #$37
    STA y0
    LDA #$16
    STA w
    LDA #$05
    STA h
    JSR rectFill
    ; little accent dots
    LDA #$0E
    STA color
    LDA #$08
    STA px
    LDA #$08
    STA py
    JSR plotPixel
    LDA #$37
    STA px
    LDA #$3B
    STA py
    JSR plotPixel
halt:
    JMP halt

rectFill:
    LDX #$00
rfY:
    STX rowOff
    LDY #$00
rfX:
    TYA
    CLC
    ADC x0
    STA px
    LDA rowOff
    CLC
    ADC y0
    STA py
    JSR plotPixel
    INY
    CPY w
    BNE rfX
    INX
    CPX h
    BNE rfY
    RTS

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

color: .byte 15
px: .byte 0
py: .byte 0
x0: .byte 0
y0: .byte 0
w: .byte 0
h: .byte 0
rowOff: .byte 0

.org $FFFC
.word start
