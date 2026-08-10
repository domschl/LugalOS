;; LugalOS Scheme / Lisp Standard Library
;; /sd0/system/stdlib.lisp

(define not (lambda (x) (if x #f #t)))

(define print (lambda (msg)
  (begin
    (display msg)
    (newline))))
