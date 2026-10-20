; sim65 startup for QBE 6502 backend tests.
;
; sim65 binary format: the program must have
; a 12-byte header (EXEHDR segment) and start
; execution at the STARTUP segment.
;
; Exit: load exit code in A, JMP $FFF9

.export __STARTUP__ : absolute = 1
.import _test, __RAM_START__, __RAM_SIZE__

.ifdef IS_6510
ZPSP = $02
ZPRET = $04
.else
ZPSP = $00
ZPRET = $02
.endif

.segment "EXEHDR"
	.byte $73, $69, $6D, $36, $35 ; "sim65"
	.byte 2                        ; version
	.byte 1                        ; CPU: 6502
	.byte ZPSP                     ; sp
	.addr __RAM_START__            ; load addr
	.word start                    ; reset vec

.segment "STARTUP"

start:
	; init 6502 stack
	ldx #$FF
	txs

	; init software stack pointer
	lda #<(__RAM_START__ + __RAM_SIZE__)
	sta ZPSP
	lda #>(__RAM_START__ + __RAM_SIZE__)
	sta ZPSP + 1

	jsr _test

	; exit: low byte of ZpRet as exit code
	lda ZPRET
	jmp $FFF9
