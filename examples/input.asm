; INPUT - input tester / cursor playground
; Up/Down/Left/Right move, A draws, B erases, Start clears, Select cycles color.
; Mouse moves; left/right mouse buttons draw/erase.
; 64x64 VRAM: $0200-$11FF, frame counter $12F6
.org $8000

start:
    JSR clearVRAM
    LDA #$20
    STA cursorX
    STA cursorY
    LDA #$0F
    STA color
    LDA #$00
    STA lastFrame

main:
    JSR waitFrame
    JSR moveCursor
    JSR handleActions
    JSR drawCursor
    JMP main

waitFrame:
    LDA $12F6
    CMP lastFrame
    BEQ waitFrame
    STA lastFrame
    RTS

moveCursor:
    LDA $12E1
    CMP #$FF
    BEQ keyboardMove
    CMP #$40
    BCS keyboardMove
    STA cursorX
    LDA $12E2
    CMP #$FF
    BEQ keyboardMove
    CMP #$40
    BCS keyboardMove
    STA cursorY
    RTS
keyboardMove:
    LDA $12E0
    AND #$01
    BEQ md
    LDA cursorY
    BEQ md
    DEC cursorY
md:
    LDA $12E0
    AND #$02
    BEQ ml
    LDA cursorY
    CMP #$3F
    BEQ ml
    INC cursorY
ml:
    LDA $12E0
    AND #$04
    BEQ mr
    LDA cursorX
    BEQ mr
    DEC cursorX
mr:
    LDA $12E0
    AND #$08
    BEQ moveDone
    LDA cursorX
    CMP #$3F
    BEQ moveDone
    INC cursorX
moveDone:
    RTS

handleActions:
    LDA $12E0
    AND #$40
    BEQ actionA
    JSR clearVRAM
actionA:
    LDA $12E0
    AND #$10
    BEQ actionB
    JSR drawCurrentWhite
actionB:
    LDA $12E0
    AND #$20
    BEQ actionSelect
    JSR drawCurrentBlack
actionSelect:
    LDA $12E0
    AND #$80
    BEQ mouseActions
    INC color
    LDA color
    AND #$0F
    STA color
mouseActions:
    LDA $12E3
    AND #$01
    BEQ mouseRight
    JSR drawCurrentWhite
mouseRight:
    LDA $12E3
    AND #$02
    BEQ actionDone
    JSR drawCurrentBlack
actionDone:
    RTS

drawCurrentWhite:
    LDA #$0F
    STA color
    JMP drawCurrent

drawCurrentBlack:
    LDA #$00
    STA color
drawCurrent:
    LDA cursorX
    STA px
    LDA cursorY
    STA py
    JSR plotPixel
    RTS

drawCursor:
    LDA #$08
    STA color
    LDA cursorX
    STA px
    LDA cursorY
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
c:
    STA ($00),Y
    INY
    BNE c
    INC $01
    DEX
    BNE c
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

cursorX: .byte 32
cursorY: .byte 32
color: .byte 15
lastFrame: .byte 0
px: .byte 0
py: .byte 0

.org $FFFC
.word start
