;;; LugalOS Sequence & List Library
;;; /sd0/system/lib/seq.lisp

;; Tail-recursive fold-left: (fold-left + 0 '(1 2 3 4)) -> 10
(define (fold-left kons knil lst)
  (let loop ((acc knil) (l lst))
    (if (null? l)
        acc
        (loop (kons acc (car l)) (cdr l)))))

;; fold-right: (fold-right cons '() '(1 2 3)) -> '(1 2 3)
(define (fold-right kons knil lst)
  (if (null? lst)
      knil
      (kons (car lst) (fold-right kons knil (cdr lst)))))

;; (iota count [start] [step])
(define (iota count . opts)
  (let* ((start (if (null? opts) 0 (car opts)))
         (step (if (or (null? opts) (null? (cdr opts))) 1 (cadr opts))))
    (let loop ((i (- count 1)) (acc '()))
      (if (< i 0)
          acc
          (loop (- i 1) (cons (+ start (* i step)) acc))))))

;; (range start end [step])
(define (range start end . opts)
  (let ((step (if (null? opts) 1 (car opts))))
    (if (> step 0)
        (let loop ((curr start) (acc '()))
          (if (>= curr end)
              (reverse acc)
              (loop (+ curr step) (cons curr acc))))
        (let loop ((curr start) (acc '()))
          (if (<= curr end)
              (reverse acc)
              (loop (+ curr step) (cons curr acc)))))))

;; (take lst n)
(define (take lst n)
  (let loop ((l lst) (i n) (acc '()))
    (if (or (<= i 0) (null? l))
        (reverse acc)
        (loop (cdr l) (- i 1) (cons (car l) acc)))))

;; (drop lst n)
(define (drop lst n)
  (let loop ((l lst) (i n))
    (if (or (<= i 0) (null? l))
        l
        (loop (cdr l) (- i 1)))))

;; (zip list1 list2 ...)
(define (zip . lists)
  (if (or (null? lists) (null? (car lists)))
      '()
      (let loop ((ls lists))
        (if (any null? ls)
            '()
            (cons (map car ls) (loop (map cdr ls)))))))

;; (partition pred lst) -> ((matching...) . (non-matching...))
(define (partition pred lst)
  (let loop ((l lst) (yes '()) (no '()))
    (if (null? l)
        (cons (reverse yes) (reverse no))
        (if (pred (car l))
            (loop (cdr l) (cons (car l) yes) no)
            (loop (cdr l) yes (cons (car l) no))))))

;; (every pred lst)
(define (every pred lst)
  (let loop ((l lst))
    (cond ((null? l) #t)
          ((not (pred (car l))) #f)
          (else (loop (cdr l))))))

;; (any pred lst)
(define (any pred lst)
  (let loop ((l lst))
    (cond ((null? l) #f)
          ((pred (car l)) #t)
          (else (loop (cdr l))))))

;; (flatten lst)
(define (flatten lst)
  (let loop ((l lst) (acc '()))
    (cond ((null? l) acc)
          ((pair? (car l))
           (loop (cdr l) (loop (car l) acc)))
          (else
           (cons (car l) (loop (cdr l) acc))))))
