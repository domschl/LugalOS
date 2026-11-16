; Sensor dashboard for a 20x4 character LCD (see `clcd` in the shell manual).
;
;   (load "/sd0/demos/clcd_dash.lisp")      or paste the definitions at the prompt
;   (clcd-dash 30)      30 rounds, one every 2 s; 0 runs until Ctrl-C
;   clcd dash [rounds]  from the shell, on a board with no filesystem: the program is in the image
;
; It turns the console mirror off while it runs. `(clcd-dash n)` turns it on again at the end; after a
; Ctrl-C, `clcd mirror on` does (the shell command does it itself).
;
; The display shows what the sensor hub has cached, so a round costs no I2C wait:
;
;   T 23.9C  RH 33%
;   P 948hPa  Lux 713
;   CO2 569ppm Dew 8.4C
;   2026-11-16 08:12
;
; A missing sensor shows "--". Only the characters that changed go over the bus.

(define (cd-pad s n)
  (if (< (string-length s) n) (cd-pad (string-append s " ") n) (substring s 0 n)))

; 0.01 units -> one decimal: 2394 -> "23.9"
(define (cd-f1 v)
  (let* ((a (abs v)) (t (quotient (+ a 5) 10)))
    (string-append (if (< v 0) "-" "") (number->string (quotient t 10)) "." (number->string (remainder t 10)))))

; 0.01 units -> whole number: 3280 -> "33"
(define (cd-i2 v) (number->string (quotient (+ v 50) 100)))

; Pa -> hPa
(define (cd-hpa v) (number->string (quotient (+ v 50) 100)))

(define (cd-val chan f)
  (let ((v (sensor-read chan)))
    (if v (f v) "--")))

(define (cd-draw)
  (let ((d (date)))
    (lcd-at 0 0 (cd-pad (string-append "T " (cd-val 'temp cd-f1) "C  RH " (cd-val 'humidity cd-i2) "%") 20))
    (lcd-at 0 1 (cd-pad (string-append "P " (cd-val 'pressure cd-hpa) "hPa  Lux " (cd-val 'lux cd-i2)) 20))
    (lcd-at 0 2 (cd-pad (string-append "CO2 " (cd-val 'co2 number->string) "ppm Dew " (cd-val 'dew-point cd-f1) "C") 20))
    (lcd-at 0 3 (cd-pad (substring d 0 16) 20))
    (lcd-flush)))

(define (clcd-dash n)
  (lcd-mirror #f)
  (lcd-clear)
  (let ((i 0))
    (while (or (<= n 0) (< i n))
      (cd-draw)
      (sleep 2000)
      (set! i (+ i 1))))
  (lcd-clear)
  (lcd-mirror #t))
