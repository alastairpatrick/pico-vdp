        .ORG    $8000

MAIN:
        CALL    PVDP_INIT

        LD      C, 0
_LOOP1:        
        CALL    PVDP_KEYBOARD_READ
        LD      A, E

        CP      $0D
        JR      Z, _MAIN_SCROLL_FOR

        CP      $09
        JR      Z, _MAIN_SCROLL_BACK

        CP      $F7
        JR      Z, _MAIN_SCROLL_COPY

        CALL    PVDP_WRITE_CHAR

        LD      E, C
        INC     C
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

_MAIN_SCROLL_COPY:
        LD      D, 12
        LD      E, 0
        LD      L, 255
        CALL    PVDP_COPY
        JR      _LOOP1
        
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
USELZSA2    .EQU  FALSE
VDA_FNCNT   .EQU  15

#INCLUDE "pvdp.asm"
#INCLUDE "font6x8u.asm"

.END
