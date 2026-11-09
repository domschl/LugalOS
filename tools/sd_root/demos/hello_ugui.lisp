; hello ugui -- a Lisp program with a window of its own (76.2).
;
;   (load "/sd0/demos/hello_ugui.lisp")
;   (hello-ugui)
;
; A button that counts, a checkbox, a switch, a slider with a progress bar, a
; stepper, a text field, and a list to pick from. All of it works by touch,
; by mouse, and by keys alone: Tab and the arrows move the focus, Enter or
; Space acts on it, Left and Right move the slider. Quit, `q` or the close
; box ends it; so does Ctrl-C in the terminal.
;
; The shape of every `ui-` program: describe the whole picture in one pass
; between (ui-begin) and (ui-end) -- positions and sizes in points, the same
; on every panel -- then wait for an event, change the program's own state,
; and describe the picture again. Pressing, hovering, the focus ring, typing
; in the text field and scrolling the list happen in the window without the
; program; it hears only what it has to act on.
;
; (hu-start) and (hu-loop ms) are the same program in two halves -- open and
; draw, then the event loop, ended once ms pass with no event -- for the test
; suite, which sends each input on the line that runs the loop.

(define hu-count 0)
(define hu-show #t)
(define hu-fast #f)
(define hu-level 40)
(define hu-size 3)
(define hu-greeting "")
(define hu-picked "nothing yet")
(define hu-picked-index -1)
(define hu-fruits (list "Apple" "Banana" "Cherry" "Date" "Elderberry" "Fig" "Grape"
                        "Honeydew" "Kiwi" "Lemon" "Mango" "Nectarine"))

; The widgets' ids: what (ui-hit ev) answers.
(define hu-id-count 1)
(define hu-id-show 2)
(define hu-id-fast 3)
(define hu-id-level 4)
(define hu-id-smaller 5)
(define hu-id-bigger 6)
(define hu-id-name 7)
(define hu-id-fruit 8)
(define hu-id-quit 9)

(define (hu-draw)
  (let* ((size (ui-size))
         (w (if size (car size) 600))
         (h (if size (cadr size) 320))
         (wide (>= w 560))
         (col (if wide 280 (- w 32)))
         (lx (if wide 316 16))
         (ly (if wide 12 316))
         (lw (if wide (- w 332) (- w 32)))
         (lh (if wide (- h 24) 150)))
    (ui-begin)
    (ui-label 16 12 "Hello, ugui" #t)
    (ui-button hu-id-count "Count" 16 40 110 28)
    (ui-label 140 46 (if (= hu-count 1) "Pressed once"
                         (string-append "Pressed " (number->string hu-count) " times")))
    (ui-checkbox hu-id-show "Show the list" hu-show 16 78 col 26)
    (ui-toggle hu-id-fast "Fast" hu-fast 16 108 col 26)
    (ui-slider hu-id-level hu-level 0 100 16 140 (- col 50) 24)
    (ui-label (- col 22) 144 (number->string hu-level))
    (ui-progress (* hu-level 10) 16 170 (- col 50) 8)
    (ui-disable (+ (if (<= hu-size 1) 1 0) (if (>= hu-size 9) 2 0)))
    (ui-stepper hu-id-smaller hu-id-bigger "Size" hu-size 16 186 col 28)
    (ui-label 16 224 "Name:")
    (ui-textfield hu-id-name 70 220 (- col 54) 26)
    (if (equal? hu-greeting "")
        (ui-label 16 252 "Type a name, then Enter." #f #t)
        (ui-label 16 252 hu-greeting #t))
    (ui-button hu-id-quit "Quit" 16 280 80 26)
    (if hu-show
        (begin
          (ui-section "Fruit" lx ly lw)
          (ui-list hu-id-fruit hu-fruits hu-picked-index lx (+ ly 22) lw (- lh 50))
          (ui-label lx (+ ly (- lh 22)) (string-append "Picked: " hu-picked))))
    (ui-end)))

; One event: #t to go on, #f to stop.
(define (hu-handle ev)
  (let ((type (car ev))
        (id (ui-hit ev))
        (value (list-ref ev 3)))
    (cond ((eq? type 'close) #f)
          ((eq? type 'resize) (hu-draw) #t)
          ((eq? type 'key) (not (= value 113)))          ; q
          ((not id) #t)
          ((= id hu-id-quit) #f)
          ((= id hu-id-count) (set! hu-count (+ hu-count 1)) (hu-draw) #t)
          ((= id hu-id-show) (set! hu-show (not hu-show)) (hu-draw) #t)
          ((= id hu-id-fast) (set! hu-fast (not hu-fast)) (hu-draw) #t)
          ((= id hu-id-level) (set! hu-level value) (hu-draw) #t)
          ((= id hu-id-smaller) (if (> hu-size 1) (set! hu-size (- hu-size 1))) (hu-draw) #t)
          ((= id hu-id-bigger) (if (< hu-size 9) (set! hu-size (+ hu-size 1))) (hu-draw) #t)
          ((and (= id hu-id-name) (eq? type 'submit))
           (set! hu-greeting (string-append "Hello, " (ui-text hu-id-name) "!"))
           (hu-draw)
           #t)
          ((= id hu-id-fruit)
           (set! hu-picked-index (list-ref ev 2))
           (set! hu-picked (list-ref hu-fruits hu-picked-index))
           (hu-draw)
           #t)
          (else #t))))

(define (hu-start)
  (if (ui-open "Hello ugui")
      (begin (hu-draw) #t)
      (begin (display "hello-ugui: no window on this board") (newline) #f)))

; The loop of (hello-ugui), ended once ms pass without an event: the test
; suite sends its input on the same line, before the program waits, so the
; input reaches it inside the wait as a person's does.
(define (hu-loop ms)
  (let loop ((ev (ui-event ms)))
    (cond ((null? ev) #t)
          ((hu-handle ev) (loop (ui-event ms)))
          (else (ui-close) #f))))

; What the program holds, for the tests.
(define (hu-state)
  (string-append "count=" (number->string hu-count)
                 " show=" (if hu-show "yes" "no")
                 " fast=" (if hu-fast "yes" "no")
                 " level=" (number->string hu-level)
                 " size=" (number->string hu-size)
                 " picked=" hu-picked
                 " greeting=" hu-greeting))

(define (hello-ugui)
  (if (hu-start)
      (let loop ((ev (ui-event)))
        (if (and (not (null? ev)) (hu-handle ev))
            (loop (ui-event))
            (ui-close)))))
