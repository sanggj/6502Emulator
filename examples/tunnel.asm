; TUNNEL - perspective tunnel with rotating portals
; Nested diamonds, side rails, center light, and pulse. 16 rings.
.org $8000

start:
    LDA #$00
    STA phase
    STA lastFrame
main:
    JSR waitFrame
    JSR clearVRAM
    JSR drawTunnel
    INC phase
    JMP main

waitFrame:
    LDA $12F6
    CMP lastFrame
    BEQ waitFrame
    STA lastFrame
    RTS

drawTunnel:
    LDX #$03
ring:
    TXA
    ASL A
    CLC
    ADC phase
    AND #$1F
    STA off
    LDA #$20
    SEC
    SBC off
    STA top
    LDA #$20
    CLC
    ADC off
    STA bot
    LDA #$20
    SEC
    SBC off
    STA left
    LDA #$20
    CLC
    ADC off
    STA right
    TXA
    CLC
    ADC phase
    AND #$0F
    STA color
    JSR diamond
    DEX
    BNE ring
    ; central glow / portal
    LDA #$0F
    STA color
    LDA #$20
    STA px
    STA py
    JSR plotPixel
    LDA #$04
    STA color
    LDA #$1F
    STA px
    LDA #$20
    STA py
    JSR plotPixel
    LDA #$21
    STA px
    JSR plotPixel
    RTS

diamond:
    ; four corners with a few interpolation points
    LDA left
    STA px
    LDA #$20
    STA py
    JSR plotPixel
    LDA right
    STA px
    JSR plotPixel
    LDA #$20
    STA px
    LDA top
    STA py
    JSR plotPixel
    LDA bot
    STA py
    JSR plotPixel
    ; diagonals toward center
    LDA left
    STA px
    LDA top
    STA py
    JSR plotPixel
    LDA right
    STA px
    JSR plotPixel
    LDA left
    STA px
    LDA bot
    STA py
    JSR plotPixel
    LDA right
    STA px
    JSR plotPixel
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

phase: .byte 0
lastFrame: .byte 0
off: .byte 0
top: .byte 0
bot: .byte 0
left: .byte 0
right: .byte 0
color: .byte 0
px: .byte 0
py: .byte 0

.org $FFFC
.word start
