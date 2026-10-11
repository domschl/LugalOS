;;; LugalOS Standard Control Macros
;;; /sd0/system/lib/macros.lisp

(define-macro (dotimes spec . body)
  (let ((var (car spec))
        (count (cadr spec)))
    `(let loop ((,var 0))
       (if (< ,var ,count)
           (begin
             ,@body
             (loop (+ ,var 1)))))))

(define-macro (dolist spec . body)
  (let ((var (car spec))
        (lst (cadr spec)))
    `(let loop ((_rest ,lst))
       (if (pair? _rest)
           (let ((,var (car _rest)))
             ,@body
             (loop (cdr _rest)))))))

(define-macro (inc! var . delta)
  (let ((d (if (null? delta) 1 (car delta))))
    `(set! ,var (+ ,var ,d))))

(define-macro (dec! var . delta)
  (let ((d (if (null? delta) 1 (car delta))))
    `(set! ,var (- ,var ,d))))

(define-macro (push! item var)
  `(set! ,var (cons ,item ,var)))

(define-macro (pop! var)
  `(let ((_top (car ,var)))
     (set! ,var (cdr ,var))
     _top))
