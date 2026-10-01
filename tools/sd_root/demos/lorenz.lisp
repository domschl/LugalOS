; The Lorenz attractor on the canvas -- phase 37's showcase, in integers.
;
;   (load "/sd0/demos/lorenz.lisp")
;   (lorenz 3000)      3000 steps of the butterfly, seen from the side (x, z)
;
; This Lisp has no floating point, so x, y and z are kept in thousandths
; (fixed point, scale 1000) and the classic parameters are built in:
; sigma 10, rho 28, beta 8/3, time step 0.01. Every product stays below
; 2^31: |x|, |y| < 30 000 and z < 60 000 in thousandths.
;
;   dx = sigma (y - x)          ->  x + (y - x) / 10
;   dy = x (rho - z) - y        ->  y + (x (28000 - z) / 1000 - y) / 100
;   dz = x y - beta z           ->  z + (x y / 1000 - 8 z / 3) / 100
;
; Each step is drawn as a line from the last point, so the curve stays
; unbroken. Ctrl-C stops it; it redraws itself when the canvas comes back.

(define (lz-sx x w) (+ (quotient w 2) (quotient (* x 9) 1000)))
(define (lz-sy z h) (- h 8 (quotient (* z 8) 1000)))

(define (lz-run x y z px py n w h)
  (if (> n 0)
      (let* ((x2 (+ x (quotient (- y x) 10)))
             (y2 (+ y (quotient (- (quotient (* x (- 28000 z)) 1000) y) 100)))
             (z2 (+ z (quotient (- (quotient (* x y) 1000) (quotient (* 8 z) 3)) 100)))
             (sx (lz-sx x2 w))
             (sy (lz-sy z2 h)))
        (canvas-line px py sx sy)
        (lz-run x2 y2 z2 sx sy (- n 1) w h))
      'done))

(define (lorenz steps)
  (canvas-window 'split)
  (canvas-title "Lorenz attractor")
  (canvas-fill 0)
  (canvas-on-redraw (lambda () (lorenz steps)))
  (let* ((size (canvas-size))
         (w (car size))
         (h (car (cdr size))))
    (canvas-text 6 4 "x-z, sigma 10, rho 28, beta 8/3")
    (lz-run 1000 1000 1000 (lz-sx 1000 w) (lz-sy 1000 h) steps w h)))
