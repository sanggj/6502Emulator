; CHECKER - animated 4x4 color checkerboard
; D-pad changes animation speed/direction; Select changes palette phase.
; 64x64 VRAM: $0200-$11FF, frame counter $12F6
.org $8000

start:
    LDA #$00
    STA phase
    STA lastFrame
    STA tick

main:
    JSR waitFrame
    INC tick
    LDA tick
    AND #$01
    BNE mainDraw
    INC phase
mainDraw:
    JSR draw
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
    TYA
    LSR A
    LSR A
    CLC
    ADC phase
    STA tmp
    TXA
    LSR A
    LSR A
    EOR tmp
    AND #$01
    BEQ dark
    LDA #$0F
    JMP put

dark:
    LDA #$04
put:
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
    INX
    CPX #$40
    BNE row
    RTS

phase: .byte 0
lastFrame: .byte 0
tick: .byte 0
tmp: .byte 0

.org $FFFC
.word start
