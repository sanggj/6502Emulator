; PAINT - minimal mouse painter
; LEFT CLICK/HOLD = BLACK, RIGHT CLICK/HOLD = WHITE.
; Moving the cursor never paints. Cursor is always black.
; Mouse X/Y: $12E1/$12E2, buttons $12E3 bit0 left/bit1 right.
; Start clears the canvas.
.org $8000

start:
    JSR clearVRAM
    LDA #$20
    STA cursorX
    STA cursorY
    LDA #$00
    STA under
    LDA #$00
    STA lastFrame
    JSR readUnder
    JSR drawCursor

main:
    JSR waitFrame
    JSR restoreCursor
    JSR readMouse
    JSR paintHeld
    JSR readUnder
    JSR drawCursor
    JMP main

waitFrame:
    LDA $12F6
    CMP lastFrame
    BEQ waitFrame
    STA lastFrame
    RTS

readMouse:
    LDA $12E1
    CMP #$FF
    BEQ mouseDone
    CMP #$40
    BCS mouseDone
    STA cursorX
    LDA $12E2
    CMP #$FF
    BEQ mouseDone
    CMP #$40
    BCS mouseDone
    STA cursorY
mouseDone:
    LDA $12E0
    AND #$40
    BEQ readDone
    JSR clearVRAM
readDone:
    RTS

paintHeld:
    LDA $12E3
    AND #$01
    BEQ tryRight
    LDA #$00
    STA color
    JSR writeCursorPixel
tryRight:
    LDA $12E3
    AND #$02
    BEQ paintDone
    LDA #$0F
    STA color
    JSR writeCursorPixel
paintDone:
    RTS

restoreCursor:
    LDA under
    STA color
    JSR writeCursorPixel
    RTS

readUnder:
    LDA cursorY
    ASL A
    ASL A
    ASL A
    ASL A
    ASL A
    ASL A
    STA $00
    LDA cursorY
    LSR A
    LSR A
    CLC
    ADC #$02
    STA $01
    LDY cursorX
    LDA ($00),Y
    STA under
    RTS

drawCursor:
    LDA #$00
    STA color
    JSR writeCursorPixel
    RTS

writeCursorPixel:
    LDA cursorY
    ASL A
    ASL A
    ASL A
    ASL A
    ASL A
    ASL A
    STA $00
    LDA cursorY
    LSR A
    LSR A
    CLC
    ADC #$02
    STA $01
    LDY cursorX
    LDA color
    STA ($00),Y
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

cursorX: .byte 32
cursorY: .byte 32
under: .byte 0
color: .byte 0
lastFrame: .byte 0

.org $FFFC
.word start
