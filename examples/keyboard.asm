; Upgraded 6502 emulator demo
; 64x64 framebuffer: $0200-$11FF (4096 bytes)
; Input:
;   $12E0 buttons: bit0 Up, bit1 Down, bit2 Left, bit3 Right
;                 bit4 A/Z, bit5 B/X, bit6 Start/Enter, bit7 Select/Shift
;   $12E1 mouse X ($FF = outside)
;   $12E2 mouse Y ($FF = outside)
;   $12E3 mouse buttons: bit0 left, bit1 right
; Frame counter: $12F6
;
; The helper routines below use zero-page $00-$02 as a framebuffer pointer/temp.

; Smooth keyboard cursor demo:
; D-pad movement, A changes color, B paints a 3x3 stamp, Start clears.
.org $8000

start:
    JSR init

mainLoop:
    JSR waitFrame
    JSR updateInput
    JSR drawScene
    JMP mainLoop

init:
    LDA #$20
    STA cursorX
    STA cursorY
    LDA #$0F
    STA color
    LDA #$00
    STA lastFrame
    JSR clearVRAM
    RTS

waitFrame:
    LDA $12F6
    CMP lastFrame
    BEQ waitFrame
    STA lastFrame
    RTS

updateInput:
    LDA $12E0
    AND #$01
    BEQ down
    LDA cursorY
    BEQ down
    DEC cursorY
down:
    LDA $12E0
    AND #$02
    BEQ left
    LDA cursorY
    CMP #$3F
    BEQ left
    INC cursorY
left:
    LDA $12E0
    AND #$04
    BEQ right
    LDA cursorX
    BEQ right
    DEC cursorX
right:
    LDA $12E0
    AND #$08
    BEQ actions
    LDA cursorX
    CMP #$3F
    BEQ actions
    INC cursorX
actions:
    LDA $12E0
    AND #$10
    BEQ checkB
    INC color
    LDA color
    AND #$0F
    STA color
checkB:
    LDA $12E0
    AND #$20
    BEQ checkStart
    JSR stamp
checkStart:
    LDA $12E0
    AND #$40
    BEQ done
    JSR clearVRAM
done:
    RTS

drawScene:
    JSR drawCursor
    RTS

stamp:
    LDX cursorX
    LDY cursorY
    JSR plotXY
    LDX cursorX
    INX
    CPX #$40
    BCS stampMiddle
    JSR plotAtCurrentY
stampMiddle:
    LDX cursorX
    LDY cursorY
    INY
    CPY #$40
    BCS stampDone
    JSR plotAtCurrentX
stampDone:
    RTS

plotAtCurrentY:
    STX px
    STY py
    JSR plotPixel
    RTS

plotAtCurrentX:
    STX px
    STY py
    JSR plotPixel
    RTS

plotXY:
    STX px
    STY py
    JSR plotPixel
    RTS

drawCursor:
    LDX cursorX
    LDY cursorY
    STX px
    STY py
    LDA #$0F
    STA color
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

cursorX:   .byte 32
cursorY:   .byte 32
color:     .byte 15
px:        .byte 0
py:        .byte 0
lastFrame: .byte 0

.org $FFFC
.word start
