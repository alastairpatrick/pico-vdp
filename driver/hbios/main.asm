        .ORG    $8000

MAIN:
        CALL    PVDP_INIT

        LD      D, $68
        CALL    PVDP_SET_CURSOR_STYLE

        LD      C, 0
_LOOP1:        
        CALL    PVDP_KEYBOARD_READ
        LD      A, E

        CP      $0D
        JR      Z, _MAIN_SCROLL_FOR

        CP      $09
        JR      Z, _MAIN_SCROLL_BACK

        CP      $F8
        JR      Z, _MAIN_SCROLL_REVERSE

        CP      $F7
        JR      Z, _MAIN_SCROLL_COPY

        CP      $F6
        JR      Z, _MAIN_SCROLL_HOME

        CALL    PVDP_WRITE_CHAR

        LD      A, (_COL)
        LD      E, A
        INC     A
        LD      (_COL), A
        CALL    PVDP_SET_CHAR_COLOR

        JR      _LOOP1

_MAIN_SCROLL_FOR:
        LD      E, 1
        CALL    PVDP_SCROLL
        JR      _LOOP1

_MAIN_SCROLL_BACK:
        LD      E, -1
        CALL    PVDP_SCROLL
        JR      _LOOP1

_MAIN_SCROLL_REVERSE:
        LD      E, $04
        CALL    PVDP_SET_CHAR_ATTR
        JR      _LOOP1
        
_MAIN_SCROLL_COPY:
        LD      DE, 0
        LD      L, 100
        CALL    PVDP_COPY
        JR      _LOOP1
        
_MAIN_SCROLL_HOME:
        LD      DE, 0
        CALL    PVDP_SET_CURSOR_POS
        JR      _LOOP1


_COL:   .DB     0

_DELAY:
        PUSH    AF
        PUSH    BC
        LD      BC, 10000
_DLOOP:
        DEC     BC
        LD      A, B
        OR      C
        JR      NZ, _DLOOP
        POP     BC
        POP     AF
        RET

; Stubs for RomWBW API
CIO_IDLE:
VDA_ADDENT:
TERM_ATTACH:
        RET

TRUE        .EQU  1
FALSE       .EQU  0
TERMENABLE  .EQU  FALSE
USELZSA2    .EQU  TRUE
VDA_FNCNT   .EQU  15

#INCLUDE "pvdp.asm"

FONT6X8:
#IF USELZSA2
#INCLUDE "font6x8c.asm"
#INCLUDE "unlzsa2s.asm"
#ELSE
#INCLUDE "font6x8u.asm"
#ENDIF

.END
