; BOUNCE - colorful bouncing orb with trail and border
; 64x64 VRAM: $0200-$11FF
; Frame counter: $12F6
.org $8000

start:
    JSR init
main:
    JSR waitFrame
    JSR shiftTrail
    JSR moveBall
    JSR clearVRAM
    JSR drawBorder
    JSR drawTrail
    JSR drawBall
    JMP main

init:
    LDA #$20
    STA ballX
    LDA #$18
    STA ballY
    LDA #$01
    STA velX
    LDA #$FF
    STA velY
    LDA #$00
    STA phase
    STA lastFrame
    LDX #$00
initTrail:
    LDA ballX
    STA trailX,X
    LDA ballY
    STA trailY,X
    INX
    CPX #$0C
    BNE initTrail
    RTS

waitFrame:
    LDA $12F6
    CMP lastFrame
    BEQ waitFrame
    STA lastFrame
    RTS

shiftTrail:
    LDX #$0A
shift:
    LDA trailX,X
    STA trailX+1,X
    LDA trailY,X
    STA trailY+1,X
    DEX
    BPL shift
    LDA ballX
    STA trailX
    LDA ballY
    STA trailY
    RTS

moveBall:
    LDA ballX
    CLC
    ADC velX
    STA ballX
    CMP #$03
    BCS xRight
    LDA #$03
    STA ballX
    LDA #$01
    STA velX
xRight:
    LDA ballX
    CMP #$3D
    BCC yMove
    LDA #$3C
    STA ballX
    LDA #$FF
    STA velX
yMove:
    LDA ballY
    CLC
    ADC velY
    STA ballY
    CMP #$03
    BCS yBottom
    LDA #$03
    STA ballY
    LDA #$01
    STA velY
yBottom:
    LDA ballY
    CMP #$3D
    BCC moveDone
    LDA #$3C
    STA ballY
    LDA #$FF
    STA velY
moveDone:
    INC phase
    RTS

drawBorder:
    LDA #$06
    STA color
    LDX #$00
borderTop:
    STX px
    LDA #$00
    STA py
    JSR plotPixel
    LDA #$3F
    STA py
    JSR plotPixel
    INX
    CPX #$40
    BNE borderTop
    LDX #$01
borderSide:
    STX py
    LDA #$00
    STA px
    JSR plotPixel
    LDA #$3F
    STA px
    JSR plotPixel
    INX
    CPX #$3F
    BNE borderSide
    RTS

drawTrail:
    LDX #$0B
trailLoop:
    STX color
    LDA trailX,X
    STA px
    LDA trailY,X
    STA py
    JSR plotPixel
    DEX
    BPL trailLoop
    RTS

drawBall:
    LDA #$0F
    STA color
    LDA ballX
    STA px
    LDA ballY
    STA py
    JSR plotPixel
    INC px
    JSR plotPixel
    INC py
    JSR plotPixel
    DEC px
    JSR plotPixel
    LDA #$0E
    STA color
    INC px
    INC py
    JSR plotPixel
    RTS

clearVRAM:
    LDA #$00
    STA $00
    LDA #$02
    STA $01
    LDX #$10
    LDY #$00
clearPage:
    STA ($00),Y
    INY
    BNE clearPage
    INC $01
    DEX
    BNE clearPage
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

ballX: .byte 0
ballY: .byte 0
velX: .byte 0
velY: .byte 0
phase: .byte 0
lastFrame: .byte 0
px: .byte 0
py: .byte 0
color: .byte 0
trailX: .res 12
trailY: .res 12

.org $FFFC
.word start
