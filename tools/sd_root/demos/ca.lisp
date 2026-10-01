; Elementary cellular automata on the canvas -- phase 37's showcase.
;
;   (load "/sd0/demos/ca.lisp")
;   (ca 30 3)     rule 30: chaos from one cell (Wolfram's random generator)
;   (ca 110 3)    rule 110: Turing-complete, growing to the left
;   (ca 90 3)     rule 90: the Sierpinski triangle
;
; The second number is the cell size in pixels: 3 fills the canvas in a few
; minutes on the RP2350-LCD-7, 1 is finer and slower. Ctrl-C stops it. The
; picture is redrawn when the canvas comes back after a layout change.
;
; A row is a list of 0s and 1s. Each new cell is bit (4 left + 2 self +
; right) of the rule number.
;
; The row is wider than the canvas. On an infinite line the pattern would
; simply grow past the edges; on a finite row the cells past the end count
; as 0, which is wrong once the pattern reaches them, and the error walks
; back inward one cell per generation. So the row extends past each edge by
; as many cells as the pattern could still need -- the generations left
; minus the seed's distance to that edge (its light cone) -- and is drawn
; shifted left by the left margin; the canvas clips the rest. (The first
; version had no margin, and rule 30's regular left half went wrong as soon
; as it reached the border: the owner's catch.)

(define (ca-bit n i)
  (if (= i 0) (modulo n 2) (ca-bit (quotient n 2) (- i 1))))

(define (ca-table rule)
  (map (lambda (i) (ca-bit rule i)) (list 0 1 2 3 4 5 6 7)))

(define (ca-step tab left cells acc)
  (if (null? cells)
      (reverse acc)
      (let ((right (if (null? (cdr cells)) 0 (car (cdr cells)))))
        (ca-step tab (car cells) (cdr cells)
                 (cons (list-ref tab (+ (* 4 left) (* 2 (car cells)) right)) acc)))))

(define (ca-seed i n where acc)
  (if (= i n)
      (reverse acc)
      (ca-seed (+ i 1) n where (cons (if (= i where) 1 0) acc))))

(define (ca-run tab row x y gens scale)
  (if (> gens 0)
      (begin
        (canvas-row x y row scale)
        (ca-run tab (ca-step tab 0 row '()) x (+ y scale) (- gens 1) scale))
      'done))

(define (ca rule scale)
  (canvas-window 'split)
  (canvas-title (string-append "Rule " (number->string rule)))
  (canvas-fill 0)
  (canvas-on-redraw (lambda () (ca rule scale)))
  (let* ((size (canvas-size))
         (w (quotient (car size) scale))
         (h (quotient (car (cdr size)) scale))
         (seed (if (= rule 110) (- w 1) (quotient w 2)))
         (ml (max 0 (- h seed)))
         (mr (max 0 (- h (- w 1 seed)))))
    (ca-run (ca-table rule)
            (ca-seed 0 (+ ml w mr) (+ ml seed) '())
            (- 0 (* ml scale)) 0 h scale)))
