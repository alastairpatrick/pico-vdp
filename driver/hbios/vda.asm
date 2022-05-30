.MODULE PVDP

; Display RAM bank A layout
; Word address  Nibble address  Pages   Description
; $0000-$07FF   $0000-$3FFF     0-4     32 text row scroll area
; $1000-$11FF   $8000-$8FFF     8       256 scanlines
; $1200-$13FF   $9000-$9FFF     9       256 character bitmaps
; $1400-$1403   $A000-$A020     10      16 color palette

TERMENABLE      	.SET	TRUE
#DEFINE USEFONT8X8

_WIDTH                  .EQU    64
_HEIGHT                 .EQU    24
_ROWS                   .EQU    32

_PORT_RSEL              .EQU    $B1
_PORT_RDAT              .EQU    $B0
_PORT_BLIT              .EQU    $B2

_REG_FONT_PG            .EQU    $21
_REG_LEDS               .EQU    $25
_REG_LINES_PG           .EQU    $20
_REG_KEY_ROWS           .EQU    $80
_REG_SPRITE_BM          .EQU    $30
_REG_SPRITE_DUT         .EQU    $2E
_REG_SPRITE_PRD         .EQU    $2D
_REG_SPRITE_RGB         .EQU    $2F
_REG_SPRITE_X           .EQU    $2B
_REG_SPRITE_Y           .EQU    $2C
_REG_START_LINE         .EQU    $24

_BCMD_SET_COUNT         .EQU    $03
_BCMD_SET_DADDR         .EQU    $00
_BCMD_SET_DADDR2        .EQU    $06
_BCMD_DCLEAR            .EQU    $34
_BCMD_DDCOPY            .EQU    $32
_BCMD_DSTREAM           .EQU    $30

_KEY_BUF_SIZE           .EQU    8
_FONT_SIZE              .EQU    $800

PVDP_FNTBL:
	.DW	PVDP_INIT
	.DW	PVDP_QUERY
	.DW	PVDP_RESET
	.DW	PVDP_DEVICE
	.DW	PVDP_SET_CURSOR_STYLE
	.DW	PVDP_SET_CURSOR_POS
	.DW	PVDP_SET_CHAR_ATTR
	.DW	PVDP_SET_CHAR_COLOR
	.DW	PVDP_WRITE_CHAR
	.DW	PVDP_FILL
	.DW	PVDP_COPY
	.DW	PVDP_SCROLL
	.DW	PVDP_KEYBOARD_STATUS
	.DW	PVDP_KEYBOARD_FLUSH
	.DW	PVDP_KEYBOARD_READ
#IF (($ - PVDP_FNTBL) != (VDA_FNCNT * 2))
	.ECHO	"*** INVALID PVDP FUNCTION TABLE ***\n"
#ENDIF

; Entry:
;  E: Video Mode
; Exit:
;  A: 0

PVDP_INIT:
        PUSH    BC
        PUSH    DE
        PUSH    HL
        
        ; No idea what this does
        LD	IY, PVDP_IDAT

        ; Initialize lines.
        LD      DE, $8000
        LD      C, _BCMD_SET_DADDR
        CALL    _BLIT_CMD_DE

        LD      DE, 512
        LD      C, _BCMD_SET_COUNT
        CALL    _BLIT_CMD_DE

        LD      C, _BCMD_DSTREAM
        CALL    _BLIT_CMD

        LD      HL, 0
_LINE_LOOP:
        CALL    _BLIT_SYNC

        XOR     A               ; palette address
        OUT     (_PORT_BLIT), A
        LD      A, $14
        OUT     (_PORT_BLIT), A

        LD      A, L            ; pixel addr = (line_idx * WIDTH / 8) & ~(WIDTH-1)
        AND     ~(_WIDTH-1)
        OUT     (_PORT_BLIT), A
        LD      A, H            
        OUT     (_PORT_BLIT), A
        
        CALL    _BLIT_SYNC

        LD      A, $7F
        OUT     (_PORT_BLIT), A
        LD      A, $0F
        OUT     (_PORT_BLIT), A

        LD      A, $00
        OUT     (_PORT_BLIT), A
        OUT     (_PORT_BLIT), A

        LD      BC, _WIDTH/8
        ADD     HL, BC
        LD      A, H
        CP      8
        JR      NZ, _LINE_LOOP

        CAll    _COPY_FONT
        
        ; Copy palette.
        LD      DE, $A000
        LD      HL, _PALETTE
        LD      BC, _PALETTE_END - _PALETTE
        CALL    _BLIT_COPY

        CALL    PVDP_RESET

        ; Lines start in page 8
        LD      C, _REG_LINES_PG
        LD      D, 8
        CALL    _SET_REG_D

        ; Character bitmaps in page 9
        LD      C, _REG_FONT_PG
        LD      D, 9
        CALL    _SET_REG_D

        ; Initialize cursor sprite
        LD      D, 60
        LD      C, _REG_SPRITE_PRD
        CALL    _SET_REG_D
        LD      D, 30
        LD      C, _REG_SPRITE_DUT
        CALL    _SET_REG_D
        LD      D, $FF
        LD      C, _REG_SPRITE_RGB
        CALL    _SET_REG_D
        
        ; Add to VDA dispatch table
        LD      BC, PVDP_FNTBL
        LD      DE, PVDP_IDAT
        CALL    VDA_ADDENT

        ; Initialize terminal emulation
        LD      BC, PVDP_FNTBL
        LD      DE, PVDP_IDAT
        CALL    TERM_ATTACH

        POP     HL
        POP     DE
        POP     BC
        XOR     A
        RET

_COPY_FONT:
        PUSH    BC
        PUSH    DE
        PUSH    HL

#IF USELZSA2
        ; Allocate buffer on stack
        LD      HL, -_FONT_SIZE
        ADD     HL, SP
	LD	SP, HL
        PUSH    HL

        ; Decompress font bitmaps
	EX	DE, HL
	LD	HL, FONT8X8
	CALL	DLZSA2

	POP	HL
#ELSE
	LD	HL, FONT8X8		; START OF FONT DATA
#ENDIF

        LD      DE, $9000
        LD      BC, $_FONT_SIZE
        CALL    _BLIT_COPY

#IF USELZSA2
        ; Free stack buffer
        LD      HL, $_FONT_SIZE
        ADD     HL, SP
	LD	SP, HL
#ENDIF

        POP     HL
        POP     DE
        POP     BC
        RET

; Exit:
;  A: 0
;  C: Video Mode (0)
;  D: Row Count
;  E: Column Count
;  HL: 0

PVDP_QUERY:
        LD      C, 0
        LD      D, _HEIGHT
        LD      E, _WIDTH
        LD      HL, 0
        XOR     A
        RET


; Exit:
;  A: 0

PVDP_RESET:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        ; Clear display area
        LD      DE, 0
        LD      C, _BCMD_SET_DADDR
        CALL    _BLIT_CMD_DE

        LD      DE, $1000
        LD      C, _BCMD_SET_COUNT
        CALL    _BLIT_CMD_DE

        LD      D, $00
        LD      C, _BCMD_DCLEAR
        CALL    _BLIT_CMD_D

        ; Initial VDA state
        LD      D, $0F
        CALL    PVDP_SET_CURSOR_STYLE

        LD      E, $0F
        CALL    PVDP_SET_CHAR_COLOR

        LD      DE, $0000
        CALL    PVDP_SET_CURSOR_POS
        
        CALL    PVDP_KEYBOARD_FLUSH

        POP     HL
        POP     DE
        POP     BC
        XOR     A
        RET


; Exit
;  A: 0
;  D: Device Type
;  E: Device Number (0)

PVDP_DEVICE:
        LD      D, $77
        LD      E, 0
        XOR     A
        RET


; Entry:
;  D: Start (top nibble) / End (bottom nibble) Pixel Row
;  E: Style (undefined)
; Exit:
;  A: 0

PVDP_SET_CURSOR_STYLE:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        LD      A, D
        AND     $0F
        LD      L, A

        SRL     D
        SRL     D
        SRL     D
        SRL     D
        LD      H, D

        LD      B, 8
        LD      C, _REG_SPRITE_BM+14

_CURSOR_LOOP:
        LD      D, $FF
        CALL    _SET_REG_D

        LD      D, $00
        LD      A, B
        CP      H
        CALL    M, _SET_REG_D
        LD      A, B
        CP      L
        CALL    P, _SET_REG_D

        DEC     C
        DEC     C
        DJNZ    _CURSOR_LOOP

        XOR     A
        POP     HL
        POP     DE
        POP     BC
        RET


; Entry:
;  D: Row (0 indexed)
;  E: Column (0 indexed)
; Exit:
;  A: 0

PVDP_SET_CURSOR_POS:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        LD      (_POS), DE
        CALL    _UPDATE_SPRITE

        POP     HL
        POP     DE
        POP     BC
        RET

_UPDATE_SPRITE:
        LD      DE, (_POS)

        ; SPRITE_Y = row*8
        SLA     D
        SLA     D
        SLA     D
        LD      C, _REG_SPRITE_Y
        CALL    _SET_REG_D

        ; SPRITE_X = col*4
        SLA     E
        SLA     E
        LD      C, _REG_SPRITE_X
        JP     _SET_REG_E



; Entry:
;  E: Character Attribute
; Exit
;  A: 0

PVDP_SET_CHAR_ATTR:
        LD      A, E
        LD      (_ATTRS), A
        XOR     A
        RET


; Entry:
;  E: Foreground Color (bottom nibble), Background Color (top nibble)
; Exit:
;  A: 0

PVDP_SET_CHAR_COLOR:
        LD      A, E
        LD      (_COLORS), A
        XOR     A
        RET


; Entry:
;  E: Character
; Exit:
;  A: 0

PVDP_WRITE_CHAR:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        CALL    _WRITE_CHAR
        CALL    _UPDATE_SPRITE

        POP     HL
        POP     DE
        POP     BC
        XOR     A
        RET

_WRITE_CHAR:
        LD      HL, (_POS)
        CALL    PVDP_CALC_DADDR
        LD      C, _BCMD_SET_DADDR
        CALL    _BLIT_CMD_HL

        ; COUNT = 1
        LD      HL, 1
        LD      C, _BCMD_SET_COUNT
        CALL    _BLIT_CMD_HL

        ; OUT char, colors & attr
        LD      C, _BCMD_DSTREAM
        CALL    _BLIT_CMD

        CALL    _BLIT_SYNC
        LD      A, E
        OUT     (_PORT_BLIT), A
        LD      A, (_COLORS)
        OUT     (_PORT_BLIT), A
        LD      A, (_ATTRS)
        OUT     (_PORT_BLIT), A
        OUT     (_PORT_BLIT), A

        ; Update character position
        LD      A, (_POS)
        INC     A
        AND     _WIDTH-1
        LD      (_POS), A
        JR      NZ, _WC_SKIP_NEWLINE
        LD      A, (_POS+1)
        INC     A
        LD      (_POS+1), A
_WC_SKIP_NEWLINE:

        RET


; Entry:
;  HL: character position
; Exit:
;  HL: DADDR nibble address in display RAM, accounting for scroll
;  A: 0

PVDP_CALC_DADDR:
        ; DADDR = (row + scroll) * WIDTH * 8 + col * 8
        LD      A, (_SCROLL)
        ADD     A, H
        SLA     L
        SLA     L
        SLA     L
        RL      A
        AND     $3F
        LD      H, A
        XOR     A
        RET


; Entry:
;  E: Character
;  HL: Count
; Exit
;  A: 0

PVDP_FILL:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        JR      _FILL_TEST
_FILL_LOOP:
        PUSH    HL
        CALL    _WRITE_CHAR
        POP     HL
        DEC     HL
_FILL_TEST:
        LD      A, H
        OR      L
        JR      NZ, _FILL_LOOP

        CALL    _UPDATE_SPRITE

        POP     HL
        POP     DE
        POP     BC
        XOR     A
        RET


; Entry:
;  D: Source Row
;  E: Source Column
;  L: Count
; Exit:
;  A: 0

PVDP_COPY:
        PUSH    HL
        PUSH    DE
        PUSH    BC

        LD      HL, (_POS)
        CALL    PVDP_CALC_DADDR
        LD      C, _BCMD_SET_DADDR
        CALL    _BLIT_CMD_HL

        LD      H, D
        LD      L, E
        CALL    PVDP_CALC_DADDR
        LD      C, _BCMD_SET_DADDR2
        CALL    _BLIT_CMD_HL

        LD      E, L
        LD      D, 1
        LD      C, _BCMD_SET_COUNT
        CALL    _BLIT_CMD_D

        LD      C, _BCMD_DDCOPY
        CALL    _BLIT_CMD

        POP     BC
        POP     DE
        POP     HL
        XOR     A
        RET


; Entry
;  E: Scroll Distance (signed)
; Exit:
;  A: 0

PVDP_SCROLL:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        LD      B, E
        JP      M, _NEG_SCROLL

_FORWARD_LOOP:
        PUSH    BC
        CALL    _SCROLL_FORWARD
        POP     BC
        DJNZ    _FORWARD_LOOP
        JR      _SCROLL_DONE

_NEG_SCROLL
        LD      A, 0
        SUB     B
        LD      B, A

_BACKWARD_LOOP:
        PUSH    BC
        CALL    _SCROLL_BACKWARD
        POP     BC
        DJNZ    _BACKWARD_LOOP

_SCROLL_DONE

        POP     HL
        POP     DE
        POP     BC
        XOR     A
        RET

_SCROLL_FORWARD:
        ; Update _SCROLL
        LD      A, (_SCROLL)
        INC     A
        AND     _ROWS-1
        LD      (_SCROLL), A

        ; Scroll VDP
        SLA     A
        SLA     A
        SLA     A
        LD      D, A
        LD      C, _REG_START_LINE
        CALL    _SET_REG_D

        ; Bottom row
        LD      HL, (_HEIGHT-1)*256
        JP      _SCROLL_CLEAR

_SCROLL_BACKWARD:
        ; Update SCROLL
        LD      A, (_SCROLL)
        DEC     A
        AND     _ROWS-1
        LD      (_SCROLL), A

        ; Scroll VDP
        SLA     A
        SLA     A
        SLA     A
        LD      D, A
        LD      C, _REG_START_LINE
        CALL    _SET_REG_D

        ; Top row
        LD      HL, 0

_SCROLL_CLEAR:
        CALL    PVDP_CALC_DADDR
        LD      C, _BCMD_SET_DADDR
        CALL    _BLIT_CMD_HL

        LD      DE, _WIDTH
        LD      C, _BCMD_SET_COUNT
        CALL    _BLIT_CMD_DE

        LD      C, _BCMD_DSTREAM
        CALL    _BLIT_CMD

        LD      B, _WIDTH
        LD      C, _PORT_BLIT
        LD      A, (_COLORS)
        LD      D, A
        LD      A, (_ATTRS)
        LD      E, A
_CLEAR_LOOP:
        CALL    _BLIT_SYNC      ; zeroes A
        OUT     (C), A
        OUT     (C), D
        OUT     (C), E
        OUT     (C), A
        DJNZ    _CLEAR_LOOP

        RET


; Exit:
;  A: 0

PVDP_KEYBOARD_FLUSH:
        XOR     A
        LD      (_KEY_BUF_BEGIN), A
        LD      (_KEY_BUF_END), A
        RET


; Exit:
;  A: Count

PVDP_KEYBOARD_STATUS:
        PUSH    DE

        CALL    _GET_MODIFIER_KEYS
        CALL    _SCAN_ROWS

        LD      A, (_KEY_BUF_BEGIN)
        LD      D, A
        LD      A, (_KEY_BUF_END)
        SUB     D
        AND     _KEY_BUF_SIZE-1

        POP     DE
        RET


; Exit:
;  A: 0
;  C: AT Scancode
;  D: Modifier State
;  E: ASCII Code

PVDP_KEYBOARD_READ:
        PUSH    HL

        ; Keep scanning until a key is available in the buffer
_KEYBOARD_READ_EMPTY:
        CALL    _GET_MODIFIER_KEYS
        CALL    _SCAN_ROWS

        LD      A, (_KEY_BUF_BEGIN)
        LD      E, A
        LD      A, (_KEY_BUF_END)
        CP      E
        JR      Z, _KEYBOARD_READ_EMPTY

        ; Advance buffer begin pointer
        LD      HL, _KEY_BUF
        LD      D, 0
        ADD     HL, DE
        INC     E
        INC     E
        LD      A, E
        AND     _KEY_BUF_SIZE-1
        LD      (_KEY_BUF_BEGIN), A

        ; Get ASCII code and modifier state from buffer
        LD      E, (HL)
        INC     HL
        LD      D, (HL)

        ; Lookup scancode from ASCII code
        LD      HL, _SCAN_CODE_LOOKUP
        LD      C, E
        LD      B, 0
        ADD     HL, BC
        LD      C, (HL)

        XOR     A
        POP     HL
        RET

_GET_MODIFIER_KEYS:
        PUSH    BC

        ; All modifier keys are on row 6
        LD      C, _REG_KEY_ROWS+6
        CALL    _GET_REG

        ; Rotate SHIFT, CTRL, ALT into C
        SRL     A
        RR      C
        SRL     A
        RR      C
        SRL     A
        RR      C

        ; Skip CAPS
        SRL     A

        ; Rotate ALT into C
        SRL     A
        RR      C

        ; Rotate C into place
        SRL     C
        SRL     C
        SRL     C
        SRL     C

        LD      A, (_MODIFIER_KEYS)
        AND     $F0     ; keep lock keys, zero rest
        OR      C
        LD      (_MODIFIER_KEYS), A

        POP     BC
        RET

_SCAN_ROWS:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        LD      B, 10
        LD      HL, _LAST_KEY_STATE+11
_SCAN_ROWS_LOOP:
        ; Get current row state
        LD      A, B
        ADD     A, _REG_KEY_ROWS
        LD      C, A
        CALL    _GET_REG
        LD      E, A

        ; Get last row state
        DEC     HL
        LD      D, (HL)
        LD      (HL), E

        ; Find newly pressed keys
        XOR     D
        AND     E
        LD      D, A
        CALL    NZ, _SCAN_COLS

        DEC     B
        JP      P, _SCAN_ROWS_LOOP

        POP     HL
        POP     DE
        POP     BC
        RET

; Entry:
;  B: Row
;  D: Newly pressed keys
; Exit:
;  A: Mask with one bit set corresponding to buffered key
_SCAN_COLS:
        PUSH    BC
        PUSH    DE

        LD      C, 0
_SCAN_ROW_LOOP:
        SRL     D
        CALL    C, _INSERT_KEY
        INC     C
        LD      A, D
        AND     A
        JR      NZ, _SCAN_ROW_LOOP

        POP     DE
        POP     BC
        RET

; Entry:
;  B: Row
;  C: Column
_INSERT_KEY:
        CALL    _MSX_CODE_TO_ASCII
        AND     A
        RET     Z

        CP      8
        JP      M, _TOGGLE_LOCK_KEY

        PUSH    BC
        PUSH    DE
        PUSH    HL

        LD      C, A

        ; Advance END ptr if no overflow
        LD      A, (_KEY_BUF_END)
        LD      E, A
        ADD     A, 2
        AND     _KEY_BUF_SIZE-1
        LD      D, A

        LD      A, (_KEY_BUF_BEGIN)
        CP      D
        JR      Z, _KEY_PRESSED_DONE

        LD      A, D
        LD      (_KEY_BUF_END), A
        LD      HL, _KEY_BUF
        LD      D, 0
        ADD     HL, DE

        ; Insert ASCII into buffer
        LD      A, C
        LD      (HL), A

        ; Add keypad modifier - keypad is rows 9 & 10
        LD      A, B
        CP      9
        LD      A, (_MODIFIER_KEYS)
        JP      M, _NOT_KEYPAD
        OR      $80
_NOT_KEYPAD:

        ; Insert modifiers into buffer
        INC     HL
        LD      (HL), A

_KEY_PRESSED_DONE:
        POP     HL
        POP     DE
        POP     BC
        RET

_MSX_CODE_TO_ASCII:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        ; Col in bits 0-2
        ; Row in bits 3-6
        SLA     B
        SLA     B
        SLA     B
        LD      A, C
        OR      B
        LD      E, A

        ; SHIFT state in bit 1 of modifiers
        LD      HL, _ASCII_LOWER
        LD      A, (_MODIFIER_KEYS)
        AND     $01
        JR      Z, _IS_LOWER
        LD      HL, _ASCII_UPPER
_IS_LOWER:
        LD      D, 0
        ADD     HL, DE
        LD      A, (HL)
        LD      D, A
        
        ; Check for letter
        AND     $DF     ; to upper case
        CP      'A'
        JP      M, _NO_CASE_SWAP
        CP      'Z'+1
        JP      P, _NO_CASE_SWAP

        ; Check caps lock is enabled
        LD      A, (_MODIFIER_KEYS)
        AND     $40
        JR      Z, _NO_CASE_SWAP

        ; Swap case
        LD      A, D
        XOR     $20
        LD      D, A

_NO_CASE_SWAP:
        LD      A, D
        POP     HL
        POP     DE
        POP     BC
        RET

_TOGGLE_LOCK_KEY:
        PUSH    BC
        PUSH    DE

        ; Shift bits 0-2 to bits 4-6 then toggle the appropriate lock key modifier.
        SLA     A
        SLA     A
        SLA     A
        SLA     A
        LD      B, A
        LD      A, (_MODIFIER_KEYS)
        XOR     B
        LD      (_MODIFIER_KEYS), A

        ; Update the LEDs.
        LD      D, A
        LD      C, _REG_LEDS
        CALL    _SET_REG_D

        POP     DE
        POP     BC
        RET

_GET_REG
        LD      A, C
        OUT     (_PORT_RSEL), A
        IN      A, (_PORT_RDAT)
        RET

_SET_REG_D:
        LD      A, C
        OUT     (_PORT_RSEL), A
        LD      A, D
        OUT     (_PORT_RDAT), A
        RET

_SET_REG_E:
        LD      A, C
        OUT     (_PORT_RSEL), A
        LD      A, E
        OUT     (_PORT_RDAT), A
        RET

_BLIT_SYNC:
        IN      A, (_PORT_BLIT)
        AND     A
        JR      Z, _BLIT_SYNC
        RET

_BLIT_CMD:
        CALL    _BLIT_SYNC
        LD      A, C
        OUT     (_PORT_BLIT), A
        RET

_BLIT_CMD_D:
        CALL    _BLIT_SYNC
        LD      A, C
        OUT     (_PORT_BLIT), A
        LD      A, D
        OUT     (_PORT_BLIT), A
        RET

_BLIT_CMD_DE:
        CALL    _BLIT_SYNC
        LD      A, C
        OUT     (_PORT_BLIT), A
        LD      A, E
        OUT     (_PORT_BLIT), A
        LD      A, D
        OUT     (_PORT_BLIT), A
        RET

_BLIT_CMD_HL:
        CALL    _BLIT_SYNC
        LD      A, C
        OUT     (_PORT_BLIT), A
        LD      A, L
        OUT     (_PORT_BLIT), A
        LD      A, H
        OUT     (_PORT_BLIT), A
        RET

; Entry:
;  HL: source ptr
;  DE: destination address (of nibble)
;  BC: byte count >=4, multiple of 4 

_BLIT_COPY:
        PUSH    DE

        ; Set DADDR
        PUSH    BC
        LD      C, _BCMD_SET_DADDR
        CALL    _BLIT_CMD_DE
        POP     BC

        ; Set COUNT
        PUSH    BC
        LD      D, B
        LD      E, C
        SRL     D
        RR      E
        SRL     D
        RR      E
        LD      C, _BCMD_SET_COUNT
        CALL    _BLIT_CMD_DE

        LD      C, _BCMD_DSTREAM
        CALL    _BLIT_CMD
        POP     BC        

        CALL    _BLIT_WRITE

        POP     DE
        RET


; Entry:
;  HL: source ptr
;  BC: byte count >=4, multiple of 4 

_BLIT_WRITE:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        LD      D, B
        LD      B, C
        LD      A, C
        AND     A
        JR      Z, _BLIT_WRITE_SKIP
        INC     D
_BLIT_WRITE_SKIP:
        LD      C, _PORT_BLIT
_BLIT_WRITE_LOOP:
        CALL    _BLIT_SYNC
        OUTI
        OUTI
        OUTI
        OUTI
        JR      NZ, _BLIT_WRITE_LOOP
        DEC     D
        JR      NZ, _BLIT_WRITE_LOOP

        POP     HL
        POP     DE
        POP     BC
        RET


_KEY_BUF_BEGIN:         .DB     0
_KEY_BUF_END:           .DB     0
_MODIFIER_KEYS:         .DB     0
_LAST_KEY_STATE:        .FILL   11, 0

_ATTRS                  .DB     0
_COLORS                 .DB     0
_POS                    .DW     0
_SCROLL                 .DB     0

_ASCII_LOWER:           .DB     "01234567"
                        .DB     "89-=\\[];"
                        .DB     $27, $60, $2C, $2E, "/", $F3, "ab"
                        .DB     "cdefghij"
                        .DB     "klmnopqr"
                        .DB     "stuvwxyz"
                        .DB     $00, $00, $00, $04, $00, $E0, $E1, $E2
                        .DB     $E3, $E4, $1B, $09, $F5, $08, $F4, $0D
                        .DB     $20, $F2, $F0, $F1, $F8, $F6, $F7, $F9
                        .DB     "*+/01234"
                        .DB     "56789-,."

_ASCII_UPPER:           .DB     ")!@#$%^&"
                        .DB     "*(_+|{}:"
                        .DB     "\"~<>?", $F3, "AB"
                        .DB     "CDEFGHIJ"
                        .DB     "KLMNOPQR"
                        .DB     "STUVWXYZ"
                        .DB     $00, $00, $00, $04, $00, $E0, $E1, $E2
                        .DB     $E3, $E4, $1B, $09, $F5, $08, $F4, $0D
                        .DB     $20, $F2, $F0, $F1, $F8, $F6, $F7, $F9
                        .DB     "*+/01234"
                        .DB     "56789-,."

_SCAN_CODE_LOOKUP:      .FILL   128, 0  ; TODO

; After the palette data is copied to video memory, it becomes the key buffer.
_KEY_BUF:
_PALETTE:               .DB     %00000000       ; black
                        .DB     %00000111       ; red
                        .DB     %00111000       ; green
                        .DB     %00100100       ; brown
                        .DB     %11000000       ; blue
                        .DB     %10000100       ; magenta
                        .DB     %10100000       ; cyan
                        .DB     %10100100       ; white
                        .DB     %01010010       ; gray
                        .DB     %10100111       ; light red
                        .DB     %10111100       ; light green
                        .DB     %00111111       ; yellow
                        .DB     %10100100       ; light blue
                        .DB     %11100111       ; light magenta
                        .DB     %11111100       ; light cyan
                        .DB     %11111111       ; bright white
_PALETTE_END:

PVDP_IDAT:
        .DB     _PORT_RSEL
        .DB     _PORT_RDAT
        .DB     _PORT_BLIT
