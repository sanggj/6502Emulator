; STARFIELD - parallax warp starfield with tiny trails
; Three depth layers and a central warp point. 30 stars total.
.org $8000

start:
    JSR init
main:
    JSR waitFrame
    JSR clearVRAM
    JSR moveStars
    JSR drawStars
    JMP main

init:
    LDX #$00
seed:
    TXA
    ASL A
    ADC #$11
    AND #$3F
    STA starX,X
    TXA
    ASL A
    ADC #$1D
    AND #$3F
    STA starY,X
    TXA
    AND #$03
    STA depth,X
    INX
    CPX #$1E
    BNE seed
    LDA #$00
    STA lastFrame
    RTS

waitFrame:
    LDA $12F6
    CMP lastFrame
    BEQ waitFrame
    STA lastFrame
    RTS

moveStars:
    LDX #$00
mv:
    LDA starX,X
    CLC
    ADC depth,X
    ADC #$01
    STA starX,X
    LDA starY,X
    CLC
    ADC depth,X
    AND #$3F
    STA starY,X
    INX
    CPX #$1E
    BNE mv
    RTS

drawStars:
    LDX #$00
dr:
    LDA starX,X
    STA px
    LDA starY,X
    STA py
    LDA depth,X
    CLC
    ADC #$03
    AND #$0F
    STA color
    JSR plotPixel
    ; short tail for deeper stars
    LDA depth,X
    BEQ noTail
    LDA px
    BEQ noTail
    DEC px
    JSR plotPixel
noTail:
    INX
    CPX #$1E
    BNE dr
    ; center warp point
    LDA #$0F
    STA color
    LDA #$20
    STA px
    STA py
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

starX: .res 30
starY: .res 30
depth: .res 30
px: .byte 0
py: .byte 0
color: .byte 0
lastFrame: .byte 0

.org $FFFC
.word start
