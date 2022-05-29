        .ORG    $8000

MAIN:
        CALL    PVDP_INIT

_LOOP1:        
        CALL    PVDP_KEYBOARD_READ

        LD      A, E
        CP      $0D
        JR      Z, _MAIN_SCROLL

        CALL    PVDP_WRITE_CHAR
        JR      _LOOP1

_MAIN_SCROLL:
        LD      E, 1
        CALL    PVDP_SCROLL
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
VDA_ADDENT:
TERM_ATTACH:
        RET

TRUE        .EQU  1
FALSE       .EQU  0
TERMENABLE  .EQU  FALSE
VDA_FNCNT   .EQU 15

#INCLUDE "vda.asm"