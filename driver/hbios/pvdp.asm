.MODULE PVDP

; Configuration
_WIDTH                  .EQU    80
_HEIGHT                 .EQU    24
TERMENABLE      	.SET	TRUE
_KEY_BUF_SIZE           .EQU    16
_ENABLE_FIFO            .EQU    1
_CURSOR_BLINK_PERIOD    .EQU    8

; Not configuration

_ADDR_FONT              .EQU    $2100
_ADDR_PALETTE           .EQU    $2000
_PORT_RSEL              .EQU    $B1
_PORT_RDAT              .EQU    $B0
_PORT_OP                .EQU    $B2

_FONT_SIZE              .EQU    $800

_OP_STEP_1              .EQU    $10
_OP_STREAM              .EQU    $03
_OP_READ                .EQU    $01

_REG_ADDRESS            .EQU    $2A
_REG_DATA               .EQU    $2C
_REG_DEVICE             .EQU    $29
_REG_KEY_ROWS           .EQU    $80
_REG_LEDS               .EQU    $08
_REG_OPERATION          .EQU    $28
_REG_SCAN_LINE          .EQU    $A0
_REG_VIDEO_FLAGS        .EQU    $20
_REG_WINDOW_X_0         .EQU    $30
_REG_WINDOW_Y_0         .EQU    $31
_REG_WINDOW_X_1         .EQU    $38
_REG_WINDOW_Y_1         .EQU    $39

#IF _WIDTH == 40
_NUM_DEVICES            .EQU    1
#ELSE
_NUM_DEVICES            .EQU    2
#ENDIF

#IF _HEIGHT == 24
#DEFINE USEFONT8X10
#ELSE
#DEFINE USEFONT8X8
#ENDIF

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

        ; Wait until VDP completes reset.
_READY_LOOP:
        LD      A, _REG_LEDS
        OUT     (_PORT_RSEL), A

        LD      A, $55
        OUT     (_PORT_RDAT), A
        IN      A, (_PORT_RDAT)
        CP      $55
        JR      NZ, _READY_LOOP

        LD      A, $AA
        OUT     (_PORT_RDAT), A
        IN      A, (_PORT_RDAT)
        CP      $AA
        JR      NZ, _READY_LOOP

        CALL    PVDP_RESET

        POP     HL
        POP     DE
        POP     BC
        XOR     A
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

        ; Set video flags
        LD      C, _REG_VIDEO_FLAGS
#IF (_WIDTH == 40) & (_HEIGHT == 24)
        LD      D, $15
#ENDIF
#IF (_WIDTH == 40) & (_HEIGHT == 30)
        LD      D, $05
#ENDIF
#IF (_WIDTH == 80) & (_HEIGHT == 24)
        LD      D, $1F
#ENDIF
#IF (_WIDTH == 80) & (_HEIGHT == 30)
        LD      D, $0F
#ENDIF
        CALL _SET_REG_D

        CALL    LPVDP_INIT

        LD      E, _NUM_DEVICES-1
_RESET_DEVICE_LOOP:
        CALL    _RESET_DEVICE
#IF _NUM_DEVICES > 1
        DEC     E
        JP      P, _RESET_DEVICE_LOOP
#ENDIF

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

_RESET_DEVICE:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        CALL    LPVDP_DEVICE

        ; Clear names
        LD      DE, 0
        CALL    LPVDP_ADDRESS
        
        LD      BC, _ADDR_PALETTE
        LD      E, 0
        CALL    LPVDP_WRITE_FILL

        ; Copy palette 0
        LD      DE, _ADDR_PALETTE
        CALL    LPVDP_ADDRESS

        LD      BC, 16
        LD      HL, _PALETTE
        CALL    LPVDP_WRITE_N

        ; Copy font
        LD      DE, _ADDR_FONT
        CALL    LPVDP_ADDRESS
        CALL    _OUTPUT_FONT

        POP     HL
        POP     DE
        POP     BC
        RET

_OUTPUT_FONT:
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
#IF _HEIGHT == 24
	LD	HL, FONT8X10
#ELSE
	LD	HL, FONT8X8
#ENDIF
	CALL	DLZSA2

	POP	HL
#ELSE
#IF _HEIGHT == 24
	LD	HL, FONT8X10
#ELSE
	LD	HL, FONT8X8
#ENDIF
#ENDIF

        LD      C, _PORT_OP
        LD      D, 0
_COPY_FONT_LOOP:

#IF _HEIGHT == 24
        LD      B, 10
#ELSE
        LD      B, 8
#ENDIF
_COPY_CHAR_LOOP:
        CALL    LPVDP_SYNC
        OUTI
        JR      NZ, _COPY_CHAR_LOOP
        CALL    LPVDP_SYNC

#IF _HEIGHT == 24
        LD      B, 6
#ELSE
        LD      B, 8
#ENDIF
_COPY_GAP_LOOP:
        CALL    LPVDP_SYNC
        XOR     A
        OUT     (C), A
        DJNZ    _COPY_GAP_LOOP
        CALL    LPVDP_SYNC

        DEC     D
        JR      NZ, _COPY_FONT_LOOP

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


; Exit
;  A: 0
;  D: Device Type
;  E: Device Number (0)

PVDP_DEVICE:
        LD      D, $77  ; TODO
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
        LD      (_POS), DE
        XOR     A
        RET


; Entry:
;  E: Character Attribute
; Exit
;  A: 1

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
        CALL    _CALC_COLOR
        LD      D, A
        CALL    _WRITE_CHAR
        CALL    _ADVANCE_POS
        XOR     A
        RET

_CALC_COLOR:
        LD      A, (_ATTRS)
        AND     $04                     ; reverse color?
        LD      A, (_COLORS)
        RET     Z

        ; Exchange color nibbles
        RLA
        RLA
        RLA
        RLA
        RET

_WRITE_CHAR:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        LD      HL, (_POS)
#IF _NUM_DEVICES > 1
        CALL    _SELECT_DEVICE
#ENDIF
        CALL    _SELECT_ADDRESS

        LD      A, E
        CALL    LPVDP_WRITE

        LD      A, D
        CALL    LPVDP_WRITE

        POP     HL
        POP     DE
        POP     BC
        RET

#IF _NUM_DEVICES > 1
_SELECT_DEVICE:
        PUSH    DE

        LD      A, L
        AND     $1
        LD      E, A
        CALL    LPVDP_DEVICE

        POP     DE
        RET
#ENDIF

_SELECT_ADDRESS:
        PUSH    DE

        ; DE = ((y + _SCROLL) & 63) * 128 + (x & 0xFE) for 80 cols
        ; DE = ((y + _SCROLL) & 63) * 128 + x * 2      for 40 cols
        LD      A, (_SCROLL)
        ADD     A, H
        AND     $3F
        LD      H, A

#IF _WIDTH == 40
        LD      A, L
        SLA     A
        SLA     A
#ELSE
        LD      A, L
        AND     $FE
        SLA     A
#ENDIF

        SRL     H
        RRA
        LD      E, A
        LD      D, H

        CALL    LPVDP_ADDRESS

        POP     DE
        RET

_ADVANCE_POS
        ; Advance character position
        LD      A, (_POS)
        INC     A
        CP      _WIDTH
        JR      NZ, _WC_SKIP_NEWLINE
        LD      A, (_POS+1)
        INC     A
        LD      (_POS+1), A
        XOR     A
_WC_SKIP_NEWLINE:
        LD      (_POS), A
        RET


; Entry:
;  E: Character
;  HL: Count
; Exit
;  A: 0

PVDP_FILL:
        PUSH    HL

        CALL    _CALC_COLOR
        LD      D, A

        JR      _FILL_TEST
_FILL_LOOP:
        CALL    _WRITE_CHAR
        CALL    _ADVANCE_POS
        DEC     HL
_FILL_TEST:
        LD      A, H
        OR      L
        JR      NZ, _FILL_LOOP

        POP     HL
        XOR     A
        RET


; Entry:
;  D: Source Row
;  E: Source Column
;  L: Count
; Exit:
;  A: 0

PVDP_COPY:
        PUSH    BC
        PUSH    DE

        LD      B, L
_COPY_LOOP
        CALL    _COPY_1_CHAR

        ; Advance source position
        INC     E
        LD      A, E
        CP      _WIDTH
        JR      NZ, _COPY_NO_WRAP
        LD      E, 0
        INC     D

_COPY_NO_WRAP:
        DJNZ    _COPY_LOOP

        POP     DE
        POP     BC
        XOR     A
        RET

_COPY_1_CHAR:
        PUSH    BC
        PUSH    DE
        PUSH    HL

        EX      DE, HL
#IF _NUM_DEVICES > 1
        CALL    _SELECT_DEVICE
#ENDIF
        CALL    _SELECT_ADDRESS
        CALL    LPVDP_READ
        LD      E, A
        CALL    LPVDP_READ_NEXT
        LD      D, A
        CALL    LPVDP_END_READ
        CALL    _WRITE_CHAR
        CALL    _ADVANCE_POS

        POP     HL
        POP     DE
        POP     BC
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
        BIT     7, E
        JP      NZ, _NEG_SCROLL

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

_SCROLL_DONE:

        CALL    _WAIT_NON_DISPLAY

        LD      A, (_SCROLL)    
        LD      D, A
        LD      C, _REG_WINDOW_Y_0
        CALL    _SET_REG_D
        LD      C, _REG_WINDOW_Y_1
        CALL    _SET_REG_D

        POP     HL
        POP     DE
        POP     BC
        XOR     A
        RET

_SCROLL_FORWARD:
        ; Update _SCROLL
        LD      A, (_SCROLL)
        INC     A
        LD      (_SCROLL), A

        ; Bottom row
        LD      HL, (_HEIGHT-1)*256
        JR      _SCROLL_CLEAR

_SCROLL_BACKWARD:
        ; Update SCROLL
        LD      A, (_SCROLL)
        DEC     A
        LD      (_SCROLL), A

        ; Top row
        LD      HL, 0

_SCROLL_CLEAR:
        LD      BC, (_POS)

        LD      (_POS), HL
        LD      HL, _WIDTH
        LD      E, 0
        CALL    PVDP_FILL

        LD      (_POS), BC
        RET

_WAIT_NON_DISPLAY:
        PUSH    BC

_WAIT_NON_DISPLAY_LOOP:        
        LD      C, _REG_SCAN_LINE
        CALL    _GET_REG
        CP      $FF
        JR      NZ, _WAIT_NON_DISPLAY_LOOP

        POP     BC
        RET


LPVDP_INIT:
LPVDP_END_READ:
        LD      A, _REG_OPERATION
        OUT     (_PORT_RSEL), A
        LD      A, _OP_STREAM + _OP_STEP_1
        OUT     (_PORT_RDAT), A
        RET


; Entry:
;  DE: video memory address
LPVDP_ADDRESS:
        PUSH    BC

        LD      C, _PORT_RDAT
        LD      A, _REG_ADDRESS
        OUT     (_PORT_RSEL), A
        OUT     (C), E
        INC     A
        OUT     (_PORT_RSEL), A
        OUT     (C), D

        POP     BC
        RET

; Entry:
;  E: device
LPVDP_DEVICE:
        LD      A, _REG_DEVICE
        OUT     (_PORT_RSEL), A
        LD      A, E
        OUT     (_PORT_RDAT), A
        RET


LPVDP_WRITE:
        OUT     (_PORT_OP), A
LPVDP_SYNC:
        IN      A, (_PORT_OP)
        AND     A
        RET     NZ
        JR      LPVDP_SYNC


; Entry:
;  BC: number of bytes to fill > 0
;  E: fill byte
LPVDP_WRITE_FILL:
        PUSH    BC
        PUSH    DE

        ; Adjust BC if C is 0
        LD      A, C
        AND     A
        JR      Z, _SET_RANGE_NZ
        INC     B
_SET_RANGE_NZ:

        LD      D, B
        LD      B, C
        LD      C, _PORT_OP
_SET_RANGE_LOOP:
        CALL    LPVDP_SYNC
        OUT     (C), E
        DJNZ    _SET_RANGE_LOOP
        DEC     D
        JR      NZ, _SET_RANGE_LOOP
        CALL    LPVDP_SYNC

        POP     DE
        POP     BC
        RET

; Entry:
;  BC: number of bytes to copy > 0
;  HL: address of source data
; Exit:
;  HL: input HL+BC
LPVDP_WRITE_N:
        PUSH    BC
        PUSH    DE

        ; Adjust BC if C is 0
        LD      A, C
        AND     A
        JR      Z, _COPY_RANGE_NZ
        INC     B
_COPY_RANGE_NZ:

        LD      D, B
        LD      B, C
        LD      C, _PORT_OP
_COPY_RANGE_LOOP:
        CALL    LPVDP_SYNC
        OUTI
        JR      NZ, _COPY_RANGE_LOOP
        DEC     D
        JR      NZ, _COPY_RANGE_LOOP
        CALL    LPVDP_SYNC

        POP     DE
        POP     BC
        RET


; Exit:
;  A: read byte
LPVDP_READ:
        LD      A, _REG_OPERATION
        OUT     (_PORT_RSEL), A
        LD      A, _OP_READ + _OP_STEP_1
        OUT     (_PORT_RDAT), A

        LD      A, _REG_DATA
        OUT     (_PORT_RSEL), A
        ; falls through

; Exit:
;  A: read byte
LPVDP_READ_NEXT:
        OUT     (_PORT_OP), A            ; write dummy byte
        CALL    LPVDP_SYNC
        IN      A, (_PORT_RDAT)
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
        SRL     A
        SRL     A

        POP     DE
        JP      Z, CIO_IDLE
        RET


; Exit:
;  A: 0
;  C: AT Scancode
;  D: Modifier State
;  E: ASCII Code

PVDP_KEYBOARD_READ:
        PUSH    HL

        ; Keep scanning until a key is available in the buffer
        LD      HL, -_CURSOR_BLINK_PERIOD
_KEYBOARD_READ_EMPTY:
        CALL    _GET_MODIFIER_KEYS
        CALL    _SCAN_ROWS

        LD      A, (_KEY_BUF_BEGIN)
        LD      E, A
        LD      A, (_KEY_BUF_END)
        CP      E
        JR      NZ, _KEYBOARD_READ_NOT_EMPTY

        LD      DE, _CURSOR_BLINK_PERIOD
        ADD     HL, DE
        LD      A, H
        AND     A
        JP      M, _CURSOR_HIDDEN

        CALL    _SHOW_CURSOR
        JR      _KEYBOARD_READ_EMPTY

_CURSOR_HIDDEN:
        CALL    _HIDE_CURSOR
        JR      _KEYBOARD_READ_EMPTY

_KEYBOARD_READ_NOT_EMPTY:

        ; Advance buffer begin pointer
        LD      HL, _KEY_BUF
        LD      D, 0
        ADD     HL, DE
        LD      A, E
        ADD     A, 4
        AND     _KEY_BUF_SIZE-1
        LD      (_KEY_BUF_BEGIN), A

        ; Get ASCII code, modifier state and AT scan code from buffer
        LD      E, (HL)
        INC     HL
        LD      D, (HL)
        INC     HL
        LD      C, (HL)

        CALL    _HIDE_CURSOR

        XOR     A
        POP     HL
        RET

_SHOW_CURSOR:
        LD      A, (_CURSOR_SHOWN)
        AND     A
        RET     NZ

        PUSH    BC
        PUSH    DE
        PUSH    HL
        
        ; Read character under cursor
        LD      HL, (_POS)
#IF _NUM_DEVICES > 1
        CALL    _SELECT_DEVICE
#ENDIF
        CALL    _SELECT_ADDRESS
        CALL    LPVDP_READ
        LD      (_CURSOR_CHAR), A
        CALL    LPVDP_READ_NEXT
        LD      (_CURSOR_COL), A
        CALL    LPVDP_END_READ

        ; Write cursor character
        LD      DE, $F000
        CALL    _WRITE_CHAR

        LD      A, 1
        LD      (_CURSOR_SHOWN), A
        
        POP     HL
        POP     DE
        POP     BC
        RET

_HIDE_CURSOR:
        LD      A, (_CURSOR_SHOWN)
        AND     A
        RET     Z
        
        PUSH    DE

        ; Restore character under cursor
        LD      DE, (_CURSOR_CHAR)
        CALL    _WRITE_CHAR

        XOR     A
        LD      (_CURSOR_SHOWN), A

        POP     DE
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

        PUSH    DE
        PUSH    HL

        PUSH    BC
        LD      C, A

        ; Advance END ptr if no overflow
        LD      A, (_KEY_BUF_END)
        LD      E, A
        ADD     A, 4
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
        POP     BC
        CP      9
        LD      A, (_MODIFIER_KEYS)
        JP      M, _NOT_KEYPAD
        OR      $80
_NOT_KEYPAD:

        ; Insert modifiers into buffer
        INC     HL
        LD      (HL), A

        ; Insert AT scan code into buffer
        CALL    _MSX_CODE_TO_AT
        INC     HL
        LD      (HL), A

_KEY_PRESSED_DONE:
        POP     HL
        POP     DE
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

        ; Toggle CAPS? (row 6 col 3)
        CP      6*8+3
        CALL    Z, _TOGGLE_CAPS_LOCK_KEY

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
        JP      M, _RETURN_ASCII
        CP      'Z'+1
        JP      P, _RETURN_ASCII

        ; Check for CTRL code
        LD      A, (_MODIFIER_KEYS)
        AND     $02
        JR      Z, _CHECK_CAPS_LOCK

        ; Retain only low 5-bits of ASCII code, yielding 0-26.
        LD      A, D
        AND     $1F
        LD      D, A
        JR      _RETURN_ASCII

_CHECK_CAPS_LOCK:
        ; Check caps lock is enabled
        LD      A, (_MODIFIER_KEYS)
        AND     $40
        JR      Z, _RETURN_ASCII

        ; Swap case
        LD      A, D
        XOR     $20
        LD      D, A

_RETURN_ASCII:
        LD      A, D
        POP     HL
        POP     DE
        POP     BC
        RET


_MSX_CODE_TO_AT:
        PUSH    DE
        PUSH    HL

        ; TODO: this doesn't work!
        LD      HL, _AT_CODES
        LD      D, 0
        ADD     HL, DE
        LD      A, (HL)

        POP     HL
        POP     DE
        RET


_TOGGLE_CAPS_LOCK_KEY:
        PUSH    BC
        PUSH    DE

        LD      A, (_MODIFIER_KEYS)
        XOR     $40
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



_KEY_BUF_BEGIN:         .DB     0
_KEY_BUF_END:           .DB     0
_MODIFIER_KEYS:         .DB     0
_LAST_KEY_STATE:        .FILL   11, 0
_KEY_BUF:               .FILL   _KEY_BUF_SIZE, 0

_POS                    .DW     0
_SCROLL                 .DB     0
_COLORS                 .DB     0
_ATTRS                  .DB     0
_CURSOR_SHOWN           .DB     0
_CURSOR_CHAR            .DB     0
_CURSOR_COL             .DB     0

; ASCII codes >=E0 are assigned as in RomWBW Architecture doc.
_ASCII_LOWER:           .DB     "01234567"                                      ; row 0
                        .DB     "89-=\\[];"                                     ; row 1
                        .DB     $27, $60, $2C, $2E, "/", $F3, "ab"              ; row 2
                        .DB     "cdefghij"                                      ; row 3
                        .DB     "klmnopqr"                                      ; row 4
                        .DB     "stuvwxyz"                                      ; row 5
                        .DB     $00, $00, $00, $00, $00, $E0, $E1, $E2          ; row 6
                        .DB     $E3, $E4, $1B, $09, $F5, $08, $F4, $0D          ; row 7
                        .DB     $20, $F2, $F0, $F1, $F8, $F6, $F7, $F9          ; row 8
                        .DB     "*+/01234"                                      ; row 9
                        .DB     "56789-,."                                      ; row 10

_ASCII_UPPER:           .DB     ")!@#$%^&"                                      ; row 0
                        .DB     "*(_+|{}:"                                      ; row 1
                        .DB     "\"~<>?", $F3, "AB"                             ; row 2
                        .DB     "CDEFGHIJ"                                      ; row 3
                        .DB     "KLMNOPQR"                                      ; row 4
                        .DB     "STUVWXYZ"                                      ; row 5
                        .DB     $00, $00, $00, $00, $00, $E0, $E1, $E2          ; row 6
                        .DB     $E3, $E4, $1B, $09, $F5, $08, $F4, $0D          ; row 7
                        .DB     $20, $F2, $F0, $F1, $F8, $F6, $F7, $F9          ; row 8
                        .DB     "*+/01234"                                      ; row 9
                        .DB     "56789-,."                                      ; row 10

                        ;       0    1    2    3    4    5    6    7    
_AT_CODES:              .DB     $45, $16, $1E, $26, $25, $2E, $36, $3D          ; row 0
                        .DB     $3E, $46, $4E, $55, $5D, $54, $5B, $4C          ; row 1
                        .DB     $76, $0E, $41, $49, $4A, $69, $1C, $32          ; row 2
                        .DB     $21, $23, $24, $2B, $34, $33, $43, $3B          ; row 3
                        .DB     $42, $4B, $3A, $31, $44, $4D, $15, $2D          ; row 4
                        .DB     $1B, $2C, $3C, $2A, $1D, $22, $35, $1A          ; row 5
                        .DB     $00, $00, $00, $00, $00, $05, $06, $04          ; row 6
                        .DB     $0C, $03, $76, $0D, $7A, $66, $7D, $5A          ; row 7
                        .DB     $29, $6C, $70, $71, $6B, $75, $72, $74          ; row 8
                        .DB     $7C, $79, $4A, $70, $69, $72, $7A, $6B          ; row 9
                        .DB     $73, $74, $6C, $75, $7D, $7B, $41, $71          ; row 10

_PALETTE:
                        .DB     %00000000
                        .DB     %00000011
                        .DB     %00011000
                        .DB     %00100011
                        .DB     %10000000
                        .DB     %10100000
                        .DB     %10000100
                        .DB     %11101101
                        .DB     %01010010
                        .DB     %01010110
                        .DB     %01110010
                        .DB     %00111111
                        .DB     %11010010
                        .DB     %11010111
                        .DB     %11111011
                        .DB     %11111111
_PALETTE_END:

PVDP_IDAT:
        .DB     _PORT_RSEL
        .DB     _PORT_RDAT
        .DB     _PORT_OP
